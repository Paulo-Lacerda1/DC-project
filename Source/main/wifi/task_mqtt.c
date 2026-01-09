// Implementa a task MQTT responsável por publicar leituras no broker TLS.
// Faz parte do módulo Wi-Fi/MQTT que sincroniza sensores com a cloud.
#include "task_mqtt.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "mqtt_client.h"

#include "logging.h"
#include "mqtt_utils.h"
#include "sensor_data_store.h"
#include "config_manager.h"
#include "system_status.h"
#include "led.h"
#include "security_hmac.h"

#define MQTT_CONNECTED_BIT          (1U << 0)
#define MQTT_RETRY_BASE_MS          1000
#define MQTT_RETRY_MAX_MS           8000
#define MQTT_CONNECT_WAIT_MS        10000
#define MQTT_NOTIFY_WAIT_MS         1000
#define MQTT_EVENT_ESCAPED_MODULE_LEN   (LOG_EVENT_MODULE_MAX_LEN * 2U)
#define MQTT_EVENT_ESCAPED_MESSAGE_LEN  (LOG_EVENT_MESSAGE_MAX_LEN * 2U)
#define MQTT_EVENT_PAYLOAD_MAX_LEN      512U

typedef struct {
    char uri[128];
    char topic[64];
    char events_topic[64];
    char client_id[64];
    char username[64];
    char password[64];
} mqtt_params_t;

static const char *TAG = "task_mqtt";

TaskHandle_t task_mqtt_handle = NULL;

static mqtt_params_t s_params = {0};
static EventGroupHandle_t s_mqtt_events = NULL;
static esp_mqtt_client_handle_t s_client = NULL;
static char *s_ca_pem = NULL;
static size_t s_ca_len = 0;
static volatile bool s_connected = false;
static volatile bool s_need_retry = false;
static int s_retry_delay_ms = MQTT_RETRY_BASE_MS;
static TickType_t s_last_retry_tick = 0;
static bool s_client_started = false;
static bool s_sd_missing = false;
static led_state_t s_last_steady_led_state = LED_STATE_NORMAL;
static int s_pending_pub_msg_id = -1;
static char s_device_uid[64] = {0};  // cache do UID para assinatura MQTT
static bool s_device_uid_loaded = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void mqtt_task(void *pvParameters);
static bool mqtt_wait_connected(TickType_t wait_ticks);
static void mqtt_handle_reconnect(void);
static void mqtt_publish_latest(void);
static void mqtt_process_event_queue(void);
static void mqtt_publish_event(const log_event_t *event);
static void mqtt_escape_json_string(const char *input, char *output, size_t output_size);
static void restore_led_if_blinking(const char *reason);
static bool mqtt_load_device_uid(void);
static void digest_to_hex(const uint8_t digest[32], char output[65]);

// Regressa o LED ao estado estável anterior após transmissões.
// Usado quando recebemos PUBACK e já não precisamos do efeito de envio.
static void restore_led_if_blinking(const char *reason)
{
    led_set_state_with_reason(s_last_steady_led_state, reason);
}

// Prepara parâmetros, lê o certificado do SD e cria a task MQTT.
// Chamado pelo app_main depois de carregar config_user.
void task_mqtt_start(const config_user_t *cfg)
{
    // Valida a configuração recebida
    if (!cfg) {
        log_error(TAG, "Configuração MQTT inválida (NULL).");
        return;
    }

    // Verifica se a task já está em execução
    if (task_mqtt_handle != NULL) {
        log_warn(TAG, "Tarefa MQTT já se encontra ativa.");
        return;
    }

    // Se não há URI configurado, não inicia MQTT
    if (cfg->mqtt_uri[0] == '\0') {
        log_warn(TAG, "URI MQTT vazio; tarefa MQTT não será iniciada.");
        system_status_set_mqtt_connected(false);
        system_status_set_mqtt_buffered(0);
        return;
    }

    // Copia parâmetros de configuração para estrutura estática
    memset(&s_params, 0, sizeof(s_params));
    strlcpy(s_params.uri, cfg->mqtt_uri, sizeof(s_params.uri));
    strlcpy(s_params.topic, cfg->mqtt_topic[0] ? cfg->mqtt_topic : "data", sizeof(s_params.topic));
    strlcpy(s_params.events_topic,
            cfg->mqtt_topic_events[0] ? cfg->mqtt_topic_events : "eventos",
            sizeof(s_params.events_topic));
    strlcpy(s_params.client_id, cfg->mqtt_client_id[0] ? cfg->mqtt_client_id : "esp32_default", sizeof(s_params.client_id));
    strlcpy(s_params.username, cfg->mqtt_username, sizeof(s_params.username));
    strlcpy(s_params.password, cfg->mqtt_password, sizeof(s_params.password));

    // Carrega o certificado CA do SD para autenticação TLS
    s_ca_pem = mqtt_load_ca_cert_from_sd("/sdcard/certs/mosquitto.crt", &s_ca_len);
    if (!s_ca_pem) {
        log_error(TAG, "Certificado CA MQTT indisponível; abortar tarefa MQTT.");
        return;
    }
    log_info(TAG, "Certificado CA carregado (%zu bytes).", s_ca_len);

    // Cria EventGroup para sincronização de estados MQTT
    s_mqtt_events = xEventGroupCreate();
    if (!s_mqtt_events) {
        log_error(TAG, "Não foi possível criar EventGroup para MQTT.");
        free(s_ca_pem);
        s_ca_pem = NULL;
        return;
    }

    // Cria a task MQTT com prioridade elevada
    if (xTaskCreate(mqtt_task,
                    "task_mqtt",
                    4096,                          // Stack size
                    NULL,
                    configMAX_PRIORITIES - 3,     // Prioridade alta
                    &task_mqtt_handle) != pdPASS) {
        // Se falhou, limpa recursos alocados
        log_error(TAG, "Não foi possível criar a tarefa MQTT.");
        vEventGroupDelete(s_mqtt_events);
        s_mqtt_events = NULL;
        free(s_ca_pem);
        s_ca_pem = NULL;
        task_mqtt_handle = NULL;
    }
}

// Loop principal que gere a ligação MQTT, reconexões e publicação de leituras.
// Criado por task_mqtt_start e executa até o sistema encerrar.
static void mqtt_task(void *pvParameters)
{
    (void)pvParameters;
    
    // Inicialização de variáveis da tarefa

    // Guarda o handle da tarefa para permitir notificações externas
    task_mqtt_handle = xTaskGetCurrentTaskHandle();
    // Inicializa o atraso de reconexão com o valor base (backoff exponencial)
    s_retry_delay_ms = MQTT_RETRY_BASE_MS;
    // Marca o momento inicial para controlar intervalos de reconexão
    s_last_retry_tick = xTaskGetTickCount();
    // Atualiza o estado global indicando que MQTT não está conectado
    system_status_set_mqtt_connected(false);
    system_status_set_mqtt_buffered(0);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_params.uri,                                                         // - URI (endereço do broker MQTT)
        .credentials.client_id = s_params.client_id,                                                // - Client ID (identificador único deste cliente)
        .credentials.username = s_params.username[0] ? s_params.username : NULL,                    // - Username/Password (credenciais de autenticação)
        .credentials.authentication.password = s_params.password[0] ? s_params.password : NULL, 
        .broker.verification.certificate = s_ca_pem,                                                // - Certificado CA (para validação TLS do broker)

        .session.keepalive = 5,                                                                     // keepalive default é 120s; quanto menos, a tualizacao fica mais rápida
    };

    // Cria a instância do cliente MQTT com a configuração definida
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        // Se falhar a inicialização, limpa todos os recursos alocados e termina a tarefa
        log_error(TAG, "Falha ao inicializar cliente MQTT.");
        vEventGroupDelete(s_mqtt_events);
        s_mqtt_events = NULL;
        free(s_ca_pem);
        s_ca_pem = NULL;
        task_mqtt_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // - MQTT_EVENT_CONNECTED: quando a ligação é estabelecida
    // - MQTT_EVENT_DISCONNECTED: quando a ligação é perdida
    // - MQTT_EVENT_PUBLISHED: confirmação de publicação (PUBACK)
    // - MQTT_EVENT_ERROR: erros de TLS ou transporte

    esp_err_t evt_status = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (evt_status != ESP_OK) {
        log_error(TAG, "Não foi possível registar event handler MQTT (%s).", esp_err_to_name(evt_status));
    }

    // Início do cliente MQTT
    esp_err_t start_status = esp_mqtt_client_start(s_client);
    if (start_status != ESP_OK) {
        // Se o arranque falhar, limpa recursos e termina a tarefa
        log_error(TAG, "Falha ao iniciar cliente MQTT (%s).", esp_err_to_name(start_status));
        vEventGroupDelete(s_mqtt_events);
        s_mqtt_events = NULL;
        free(s_ca_pem);
        s_ca_pem = NULL;
        task_mqtt_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_client_started = true;    //cliente conectado
    system_status_mark_mqtt_attempt();      // Regista a tentativa de conexão nas estatísticas do sistema


    // Aguarda até 10 segundos pela primeira conexão ao broker.
    // Se não conectar neste período, marca que é necessário tentar reconectar no loop principal.
    if (!mqtt_wait_connected(pdMS_TO_TICKS(MQTT_CONNECT_WAIT_MS))) {
        log_warn(TAG, "MQTT não conectou dentro do timeout inicial.");
        s_need_retry = true;
    }

    // Loop principal da tarefa MQTT

    while (1) {

        // Verificação da disponibilidade do cartão SD
        bool sd_ok = config_manager_is_sd_available();
        if (sd_ok) {
            sd_ok = config_manager_check_sd_alive();
        }

        // Caso 1: Cartão SD não disponível
        // Para o cliente MQTT e suspende operações até o SD voltar
        if (!sd_ok) {
            if (!s_sd_missing) {
                log_error(TAG, "Cartão microSD não está inserido. Publicações MQTT suspensas.");
                s_sd_missing = true;
            }
            // Para o cliente MQTT se estiver a correr
            if (s_client_started && s_client) {
                esp_mqtt_client_stop(s_client);
                s_client_started = false;
            }
            // Limpa estados de conexão
            s_connected = false;
            s_need_retry = false;
            if (s_mqtt_events) {
                xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            }
            system_status_set_mqtt_connected(false);
            // Aguarda 1 segundo antes de verificar novamente
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        } else if (s_sd_missing) {
            // Caso 2: Cartão SD voltou a estar disponível
            // Reinicia o cliente MQTT para retomar operações normais
            log_info(TAG, "Cartão SD disponível novamente; a retomar MQTT.");
            if (!s_client_started && s_client) {
                esp_err_t restart = esp_mqtt_client_start(s_client);
                if (restart == ESP_OK) {
                    s_client_started = true;
                    s_need_retry = true;
                    s_retry_delay_ms = MQTT_RETRY_BASE_MS;
                    system_status_mark_mqtt_attempt();
                } else if (restart != ESP_ERR_INVALID_STATE) {
                    log_error(TAG, "Falha ao reiniciar cliente MQTT (%s).", esp_err_to_name(restart));
                }
            }
            s_sd_missing = false;
        }

        mqtt_handle_reconnect();

        // Aguarda notificações da tarefa de sensores/eventos indicando que há trabalho pendente.
        uint32_t notify_bits = 0;
        BaseType_t wait_status = xTaskNotifyWait(0,
                                                 UINT32_MAX,
                                                 &notify_bits,
                                                 pdMS_TO_TICKS(MQTT_NOTIFY_WAIT_MS));
        if (wait_status == pdTRUE) {
            do {
                if (notify_bits & TASK_MQTT_NOTIFY_DATA_BIT) {
                    mqtt_publish_latest();
                }
                if (notify_bits & TASK_MQTT_NOTIFY_EVENT_BIT) {
                    mqtt_process_event_queue();
                }
            } while (xTaskNotifyWait(0, UINT32_MAX, &notify_bits, 0) == pdTRUE);
        } else {
            // Timeout: garante que eventos pendentes são verificados mesmo sem nova notificação.
            mqtt_process_event_queue();
        }
    }
}
// Carrega e memoriza o UID do dispositivo para assinar as mensagens.
// Chamado antes de cada publicação para garantir que o HMAC usa um identificador válido.
static bool mqtt_load_device_uid(void)
{
    if (s_device_uid_loaded && s_device_uid[0] != '\0') {
        return true;
    }

    esp_err_t err = config_manager_get_device_uid(s_device_uid, sizeof(s_device_uid));
    if (err == ESP_OK && s_device_uid[0] != '\0') {
        s_device_uid_loaded = true;
        return true;
    }

    ESP_LOGW(TAG, "UID do dispositivo indisponível para assinar MQTT (%s).", esp_err_to_name(err));
    s_device_uid_loaded = false;
    s_device_uid[0] = '\0';
    return false;
}

// Converte digest binário SHA-256 em string hex minúscula.
// Auxiliar usado ao construir o payload assinado.
static void digest_to_hex(const uint8_t digest[32], char output[65])
{
    for (size_t i = 0; i < 32; ++i) {
        snprintf(&output[i * 2], 3, "%02x", digest[i]);
    }
    output[64] = '\0';
}

// Obtém a última leitura e publica via MQTT com assinatura HMAC.
// Invocado quando task_sensor notifica novos dados ou em flush manual.
static void mqtt_publish_latest(void)
{
    // Verifica se o cliente MQTT foi inicializado e está ativo
    if (!s_client || !s_client_started) {
        log_error(TAG, "Cliente MQTT não está inicializado.");
        return;
    }

    // Verifica se a ligação MQTT está estabelecida
    if (!s_connected) {
        s_need_retry = true;
        system_status_set_mqtt_connected(false);
        log_warn(TAG, "MQTT não conectado; leitura não será publicada até recuperar ligação.");
        return;
    }

    // Obtém a última leitura do sensor via data store thread-safe
    sensor_reading_t reading;
    if (!sensor_get_last_reading(&reading)) {
        log_warn(TAG, "Leitura do sensor indisponível; não publicar.");
        return;
    }

    // Carrega o UID do dispositivo para assinatura (do SD)
    if (!mqtt_load_device_uid()) {
        return;
    }

    // Constrói string canónica para assinatura HMAC no formato: uid|temperatura|humidade
    char canonical[160];
    int canonical_len = snprintf(canonical, sizeof(canonical),
                                 "%s|%.2f|%.2f",
                                 s_device_uid,
                                 reading.temperatura_c,
                                 reading.humidade_rh);
    if (canonical_len < 0 || canonical_len >= (int)sizeof(canonical)) {
        ESP_LOGW(TAG, "String canónica MQTT demasiado longa; mensagem não publicada.");
        return;
    }

    // Calcula HMAC-SHA256 da string canónica usando chave do eFuse
    uint8_t digest[32];
    esp_err_t hmac_err = security_hmac_compute((const uint8_t *)canonical, (size_t)canonical_len, digest);
    if (hmac_err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao calcular HMAC da leitura MQTT (%s); mensagem não publicada.",
                 esp_err_to_name(hmac_err));
        return;
    }

    // Converte o digest binário para string hexadecimal
    char hex_digest[65];
    digest_to_hex(digest, hex_digest);

    // Constrói o payload JSON com temperatura, humidade e HMAC
    char payload[192];
    int payload_len = snprintf(payload, sizeof(payload),
                               "{\n"
                               "Temperatura %.2f º \n"
                               "Humidade %.2f %%\n"
                               "HMAC: \"%s\"\n"
                               "}",
                               reading.temperatura_c,
                               reading.humidade_rh,
                               hex_digest);
    if (payload_len < 0 || payload_len >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "Payload MQTT excede o limite do buffer; mensagem não publicada.");
        return;
    }

    // Publica a mensagem no broker MQTT
    // Parâmetros: cliente, tópico, payload, comprimento (0=auto), QoS (0), retain (0)
    int msg_id = esp_mqtt_client_publish(s_client,
                                         s_params.topic,
                                         payload,
                                         0,
                                         0,  
                                         0);     
    if (msg_id < 0) {
        // Publicação falhou
        log_error(TAG, "Publicação falhou no tópico %s.", s_params.topic);
        system_status_increment_mqtt_fail();
    } else {
        // Publicação iniciada com sucesso
        s_pending_pub_msg_id = msg_id;
        s_last_steady_led_state = LED_STATE_TRANSMISSION;
        
        // Altera LED para amarelo durante transmissão
        if (led_get_state() != LED_STATE_TRANSMISSION) {
            led_set_state_with_reason(LED_STATE_TRANSMISSION, "publicação MQTT em curso");
        }
        // Incrementa contador de mensagens enviadas
        system_status_increment_mqtt_sent();
    }
}

// Draina a fila de eventos de log e publica cada entrada no tópico configurado.
// Chamado quando recebemos notificações de eventos ou em timeouts periódicos.
static void mqtt_process_event_queue(void)
{
    if (!s_client || !s_client_started || !s_connected) {
        return;
    }

    log_event_t event;
    while (log_dequeue_event(&event)) {
        mqtt_publish_event(&event);
    }
}

// Publica um único evento já preparado pelo módulo de logging no tópico de eventos.
static void mqtt_publish_event(const log_event_t *event)
{
    if (!event) {
        return;
    }

    char module_buf[MQTT_EVENT_ESCAPED_MODULE_LEN];
    char message_buf[MQTT_EVENT_ESCAPED_MESSAGE_LEN];
    mqtt_escape_json_string(event->module, module_buf, sizeof(module_buf));
    mqtt_escape_json_string(event->message, message_buf, sizeof(message_buf));

    const char *topic = s_params.events_topic[0] ? s_params.events_topic : "eventos";
    char canonical_payload[MQTT_EVENT_PAYLOAD_MAX_LEN];
    int canonical_len = snprintf(canonical_payload,
                                 sizeof(canonical_payload),
                                 "{\n"
                                 "\"timestamp\": \"%s\",\n"
                                 "\"level\": \"%s\",\n"
                                 "\"module\": \"%s\",\n"
                                 "\"message\": \"%s\"\n"
                                 "}",
                                 event->timestamp,
                                 event->level,
                                 module_buf,
                                 message_buf);
    if (canonical_len < 0 || canonical_len >= (int)sizeof(canonical_payload)) {
        ESP_LOGW(TAG, "Evento MQTT descartado: payload excede limite.");
        return;
    }

    uint8_t digest[32];
    esp_err_t hmac_err = security_hmac_compute((const uint8_t *)canonical_payload,
                                               (size_t)canonical_len,
                                               digest);
    if (hmac_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Falha ao calcular HMAC do evento MQTT (%s); mensagem não publicada.",
                 esp_err_to_name(hmac_err));
        return;
    }

    char hex_digest[65];
    digest_to_hex(digest, hex_digest);

    char payload[MQTT_EVENT_PAYLOAD_MAX_LEN];
    int payload_len = snprintf(payload,
                               sizeof(payload),
                               "{\n"
                               "\"timestamp\": \"%s\",\n"
                               "\"level\": \"%s\",\n"
                               "\"module\": \"%s\",\n"
                               "\"message\": \"%s\",\n"
                               "\"hmac\": \"%s\"\n"
                               "}",
                               event->timestamp,
                               event->level,
                               module_buf,
                               message_buf,
                               hex_digest);
    if (payload_len < 0 || payload_len >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "Evento MQTT descartado: payload excede limite.");
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publicação falhou no tópico de eventos %s.", topic);
    }
}

// Escapa caracteres sensíveis (", \ e controlo) para construir JSON seguro.
static void mqtt_escape_json_string(const char *input, char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return;
    }

    size_t out_idx = 0;
    output[0] = '\0';
    if (!input) {
        return;
    }

    for (size_t i = 0; input[i] != '\0' && out_idx + 1 < output_size; ++i) {
        char c = input[i];
        const char *escape_seq = NULL;
        switch (c) {
        case '\"':
            escape_seq = "\\\"";
            break;
        case '\\':
            escape_seq = "\\\\";
            break;
        case '\n':
            escape_seq = "\\n";
            break;
        case '\r':
            escape_seq = "\\r";
            break;
        case '\t':
            escape_seq = "\\t";
            break;
        default:
            break;
        }

        if (escape_seq) {
            size_t esc_len = strlen(escape_seq);
            if (out_idx + esc_len >= output_size) {
                out_idx = output_size - 1;
                break;
            }
            memcpy(&output[out_idx], escape_seq, esc_len);
            out_idx += esc_len;
        } else if ((unsigned char)c >= 0x20) {
            output[out_idx++] = c;
        }
    }
    output[out_idx] = '\0';
}

// Controla a política de reconexão exponencial ao broker MQTT.
// Chamado a cada iteração do loop principal quando a ligação cai.
static void mqtt_handle_reconnect(void)
{
    if (!s_client || !s_client_started || !s_need_retry) {
        return;
    }

    system_status_set_mqtt_connected(false);

    TickType_t now = xTaskGetTickCount();
    if ((now - s_last_retry_tick) < pdMS_TO_TICKS(s_retry_delay_ms)) {
        return;
    }

    esp_err_t res = esp_mqtt_client_reconnect(s_client);
    system_status_mark_mqtt_attempt();
    if (res == ESP_OK) {
        log_info(TAG, "A tentar reconectar MQTT (próximo atraso %d ms).", s_retry_delay_ms);
    } else {
        log_warn(TAG, "Reconexão MQTT falhou (%s).", esp_err_to_name(res));
        system_status_increment_mqtt_fail();
    }

    s_last_retry_tick = now;
    if (s_retry_delay_ms < MQTT_RETRY_MAX_MS) {
        s_retry_delay_ms *= 2;
        if (s_retry_delay_ms > MQTT_RETRY_MAX_MS) {
            s_retry_delay_ms = MQTT_RETRY_MAX_MS;
        }
    }
}

// Bloqueia até receber o bit de ligação estabelecida ou expirar o tempo.
// Usado após esp_mqtt_client_start para saber se precisamos marcar retry.
static bool mqtt_wait_connected(TickType_t wait_ticks)
{
    if (!s_mqtt_events) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_mqtt_events,
                                           MQTT_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           wait_ticks);
    return (bits & MQTT_CONNECTED_BIT) != 0;
}

// Callback registado no cliente MQTT para atualizar estados e LEDs.
// Reage a eventos de ligação, PUBACK e erros transport layer.
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_need_retry = false;
        s_retry_delay_ms = MQTT_RETRY_BASE_MS;
        s_last_retry_tick = xTaskGetTickCount();
        if (s_mqtt_events) {
            xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "MQTT ligado.");
        log_info(TAG, "MQTT ligado.");
        system_status_set_mqtt_connected(true);
        s_last_steady_led_state = LED_STATE_TRANSMISSION;
        led_set_state_with_reason(LED_STATE_TRANSMISSION, "MQTT ligado");
        s_pending_pub_msg_id = -1;
        if (task_mqtt_handle != NULL) {
            xTaskNotify(task_mqtt_handle, TASK_MQTT_NOTIFY_EVENT_BIT, eSetBits);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        s_need_retry = true;
        s_retry_delay_ms = MQTT_RETRY_BASE_MS;
        if (s_mqtt_events) {
            xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        }
        ESP_LOGW(TAG, "MQTT desligado.");
        log_warn(TAG, "MQTT desligado.");
        system_status_set_mqtt_connected(false);
        system_status_increment_mqtt_fail();
        s_pending_pub_msg_id = -1;
        s_last_steady_led_state = LED_STATE_NORMAL;
        led_set_state_with_reason(LED_STATE_NORMAL, "MQTT desconectado");
        break;
    case MQTT_EVENT_ERROR:
        s_connected = false;
        s_need_retry = true;
        s_retry_delay_ms = MQTT_RETRY_BASE_MS;
        if (s_mqtt_events) {
            xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        }
        if (event && event->error_handle) {
            ESP_LOGE(TAG, "MQTT erro (tls=%d errno=%d code=%d).",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_transport_sock_errno,
                     event->error_handle->connect_return_code);
            log_error(TAG, "MQTT erro (tls=%d errno=%d code=%d).",
                      event->error_handle->esp_tls_last_esp_err,
                      event->error_handle->esp_transport_sock_errno,
                      event->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "MQTT erro sem detalhes.");
            log_error(TAG, "MQTT erro sem detalhes.");
        }
        system_status_set_mqtt_connected(false);
        system_status_increment_mqtt_fail();
        s_pending_pub_msg_id = -1;
        s_last_steady_led_state = LED_STATE_NORMAL;
        led_set_state_with_reason(LED_STATE_NORMAL, "MQTT erro/desligado");
        break;
    case MQTT_EVENT_PUBLISHED:
        if (event && event->msg_id == s_pending_pub_msg_id) {
            restore_led_if_blinking("restaurar estado após PUBACK");
            s_pending_pub_msg_id = -1;
        }
        break;
    default:
        break;
    }
}
