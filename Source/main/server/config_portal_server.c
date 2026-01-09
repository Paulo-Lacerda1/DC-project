// Fornece um portal HTTP no SoftAP para introduzir credenciais Wi-Fi.
// Integra o módulo de configuração inicial/fallback do dispositivo.
#include "config_portal_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "config_manager.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "system_status.h"

typedef enum {
    PORTAL_STATE_IDLE = 0,
    PORTAL_STATE_CONNECTING,
    PORTAL_STATE_CONNECTED,
    PORTAL_STATE_ERROR,
} portal_state_t;

static const char *TAG = "config_portal";

static httpd_handle_t s_portal_server = NULL;
static bool s_portal_handlers_registered = false;
static portMUX_TYPE s_portal_lock = portMUX_INITIALIZER_UNLOCKED;
static portal_state_t s_portal_state = PORTAL_STATE_IDLE;
static char s_portal_error[128] = {0};
static TaskHandle_t s_portal_reset_task = NULL;

extern const uint8_t config_portal_html_start[] asm("_binary_config_portal_html_start");
extern const uint8_t config_portal_html_end[] asm("_binary_config_portal_html_end");

// Atualiza o estado interno do portal protegendo o acesso com spinlock.
// Chamado pelos handlers de Wi-Fi e POST para refletir progresso/erros.
static void portal_set_state(portal_state_t new_state, const char *error_msg)
{
    taskENTER_CRITICAL(&s_portal_lock);
    s_portal_state = new_state;
    if (new_state == PORTAL_STATE_ERROR && error_msg) {
        strlcpy(s_portal_error, error_msg, sizeof(s_portal_error));
    } else if (new_state != PORTAL_STATE_ERROR) {
        s_portal_error[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_portal_lock);
}

// Devolve um snapshot atómico do estado/mensagem de erro atual.
// Usado pelas rotas HTTP para responder à página de status.
static portal_state_t portal_get_state(char *error_buf, size_t error_len)
{
    taskENTER_CRITICAL(&s_portal_lock);
    portal_state_t snapshot = s_portal_state;
    if (error_buf && error_len > 0) {
        strlcpy(error_buf, s_portal_error, error_len);
    }
    taskEXIT_CRITICAL(&s_portal_lock);
    return snapshot;
}

// Helper para marcar o portal como a tentar ligar com novas credenciais.
// Chamado imediatamente após receber um POST válido.
static void portal_mark_connecting(void)
{
    portal_set_state(PORTAL_STATE_CONNECTING, NULL);
}

// Marca o portal como ligado depois do STA obter IP válido.
// Invocado pelo handler de eventos Wi-Fi.
static void portal_mark_connected(void)
{
    portal_set_state(PORTAL_STATE_CONNECTED, NULL);
}

// Regista o erro ocorrido durante o processo de ligação.
// Usado quando o Wi-Fi falha a autenticação ou ocorre timeout.
static void portal_mark_error(const char *reason)
{
    portal_set_state(PORTAL_STATE_ERROR, reason);
}

// Task que aguarda um instante e reinicia a board para permitir resposta HTTP.
static void portal_delayed_reset_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGW(TAG, "Reset solicitado através do portal HTTP.");
    esp_restart();    
}

// Agenda o reset se ainda não existir outro em progresso.
static esp_err_t portal_schedule_reset(void)
{
    if (s_portal_reset_task) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t created = xTaskCreate(portal_delayed_reset_task, "portal_reset", 2048,
                                     NULL, tskIDLE_PRIORITY + 3, &s_portal_reset_task);
    if (created != pdPASS) {
        s_portal_reset_task = NULL;
        ESP_LOGE(TAG, "Não foi possível criar task para reset.");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Serve o HTML estático do portal configurador.
// Handler associado à raiz do servidor SoftAP.
static esp_err_t portal_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = (size_t)(config_portal_html_end - config_portal_html_start);
    return httpd_resp_send(req, (const char *)config_portal_html_start, len);
}

// Converte o estado interno em strings usadas pelo front-end.
// Utilizado pelas respostas JSON do portal.
static const char *portal_state_to_string(portal_state_t state)
{
    switch (state) {
        case PORTAL_STATE_CONNECTING:
            return "connecting";
        case PORTAL_STATE_CONNECTED:
            return "connected";
        case PORTAL_STATE_ERROR:
            return "error";
        case PORTAL_STATE_IDLE:
        default:
            return "idle";
    }
}

// Constrói a mensagem exibida no portal a partir do estado e snapshot Wi-Fi.
// Chamado periodicamente pela página via XHR.
static void build_status_message(portal_state_t state,
                                 const system_status_snapshot_t *snapshot,
                                 const char *error_msg,
                                 char *out_msg,
                                 size_t out_len)
{
    if (!out_msg || out_len == 0) {
        return;
    }

    if (state == PORTAL_STATE_CONNECTING) {
        strlcpy(out_msg, "A ligar ao Wi-Fi...", out_len);
        return;
    }

    if (state == PORTAL_STATE_CONNECTED && snapshot && snapshot->wifi.sta_connected) {
        snprintf(out_msg, out_len, "Ligado com sucesso. IP STA: %s", snapshot->wifi.ip_addr);
        return;
    }

    if (state == PORTAL_STATE_ERROR) {
        if (error_msg && error_msg[0] != '\0') {
            strlcpy(out_msg, error_msg, out_len);
        } else {
            strlcpy(out_msg, "Não foi possível ligar. Verifique as credenciais.", out_len);
        }
        return;
    }

    if (config_manager_has_saved_wifi_credentials()) {
        strlcpy(out_msg,
                "Credenciais guardadas. Volte a clicar em Guardar para tentar nova ligação.",
                out_len);
    } else {
        strlcpy(out_msg,
                "Introduza o SSID e a password da rede Wi-Fi e prima Guardar.",
                out_len);
    }
}

// Endpoint JSON que devolve estado atual do portal e Wi-Fi.
// Consumido pelo frontend para atualizar indicadores sem recarregar.
static esp_err_t portal_status_handler(httpd_req_t *req)
{
    portal_state_t state;
    char error_msg[128] = {0};
    state = portal_get_state(error_msg, sizeof(error_msg));

    system_status_snapshot_t snapshot = {0};
    system_status_snapshot(&snapshot);

    char message[192];
    build_status_message(state, &snapshot, error_msg, message, sizeof(message));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "state", portal_state_to_string(state));
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddBoolToObject(root, "staConnected", snapshot.wifi.sta_connected);
    cJSON_AddStringToObject(root, "staIp",
                            snapshot.wifi.sta_connected ? snapshot.wifi.ip_addr : "—");
    cJSON_AddStringToObject(root, "savedSsid",
                            g_config_user.wifi_ssid[0] ? g_config_user.wifi_ssid : "—");
    cJSON_AddStringToObject(root, "softapIp", "192.168.4.1");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return res;
}

// Processa o POST enviado pelo portal com novo SSID/password.
// Valida, guarda no SD e sinaliza o config_manager para tentar ligação.
static esp_err_t portal_wifi_post_handler(httpd_req_t *req)
{
    // Valida o tamanho do conteúdo recebido
    if (req->content_len == 0 || req->content_len > 256) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Pedido inválido.");
    }

    // Buffer para receber o corpo do POST
    char buffer[257] = {0};
    size_t received = 0;
    
    // Lê o corpo completo do pedido em chunks
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            // Se timeout, tenta novamente
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            // Erro ao receber corpo do POST via socket
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "Erro ao ler pedido.");
        }
        received += ret;
    }
    buffer[received] = '\0';

    // Buffers para extrair SSID e password do URL-encoded
    char ssid[CONFIG_MANAGER_WIFI_SSID_MAX_LEN + 1] = {0};
    char password[CONFIG_MANAGER_WIFI_PASS_MAX_LEN + 1] = {0};

    // Extrai os valores dos campos ssid e password
    if (httpd_query_key_value(buffer, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        httpd_query_key_value(buffer, "password", password, sizeof(password)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Campos obrigatórios em falta.");
    }

    // Valida as credenciais (comprimento, caracteres, etc.)
    if (config_manager_check_wifi_credentials(ssid, password) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Credenciais inválidas.");
    }

    // Grava as credenciais no SD (config_user.json)
    esp_err_t save_status = config_manager_save_wifi_credentials(ssid, password);
    if (save_status != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao gravar credenciais (%s).", esp_err_to_name(save_status));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Não foi possível guardar as credenciais.");
    }

    // Marca o portal como "a ligar"
    portal_mark_connecting();
    
    // Aplica as credenciais na interface STA e tenta ligar
    esp_err_t apply_status = config_manager_apply_sta_credentials(ssid, password);
    if (apply_status != ESP_OK) {
        // Se falhou, marca erro com mensagem descritiva
        char msg[96];
        snprintf(msg, sizeof(msg), "Erro ao iniciar ligação Wi-Fi (%s).",
                 esp_err_to_name(apply_status));
        portal_mark_error(msg);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Falha ao iniciar ligação. Verifique o log.");
    }

    // Responde com sucesso ao cliente
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Credenciais guardadas. A ligar ao Wi-Fi...");
}

// Handler que permite pedir reset remoto da placa através do portal.
static esp_err_t portal_reset_post_handler(httpd_req_t *req)
{
    esp_err_t err = portal_schedule_reset();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Já existe um reset pendente. Aguarde alguns segundos.");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Não foi possível agendar o reset. Tente novamente.");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req,
                              "Reset agendado. A placa ficará indisponível por alguns segundos.");
}

static const httpd_uri_t portal_root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = portal_root_handler,
};

static const httpd_uri_t portal_status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = portal_status_handler,
};

static const httpd_uri_t portal_wifi_uri = {
    .uri = "/wifi",
    .method = HTTP_POST,
    .handler = portal_wifi_post_handler,
};

static const httpd_uri_t portal_reset_uri = {
    .uri = "/reset",
    .method = HTTP_POST,
    .handler = portal_reset_post_handler,
};

// Encerra o servidor HTTP do portal e liberta o handle.
// Chamado quando o SoftAP é desligado ou não há necessidade de portal.
static void portal_stop_server(void)
{
    if (s_portal_server) {
        httpd_stop(s_portal_server);
        s_portal_server = NULL;
    }
}

// Inicia o servidor HTTP simples no SoftAP e regista as rotas.
// Invocado quando o AP arranca ou no boot caso esteja ativo.
static void portal_start_server(void)
{
    if (s_portal_server) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_portal_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Não foi possível iniciar o portal HTTP (%s).", esp_err_to_name(err));
        s_portal_server = NULL;
        return;
    }

    httpd_register_uri_handler(s_portal_server, &portal_root_uri);
    httpd_register_uri_handler(s_portal_server, &portal_status_uri);
    httpd_register_uri_handler(s_portal_server, &portal_wifi_uri);
    httpd_register_uri_handler(s_portal_server, &portal_reset_uri);
    printf("=============================================\n");
    printf("Site de configuração : http://192.168.4.1/\n");
    printf("=============================================\n");

    char sd_err[96];
    if (config_manager_pop_pending_sd_error(sd_err, sizeof(sd_err))) {
        ESP_LOGE("app", "%s", sd_err);
    }
}

// Handler partilhado para eventos WIFI/IP relevantes ao portal.
// Atualiza o estado e arranca/para o servidor consoante o AP.
static void portal_wifi_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_START) {
            portal_start_server();
        } else if (event_id == WIFI_EVENT_AP_STOP) {
            portal_stop_server();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            portal_state_t current = portal_get_state(NULL, 0);
            if (current == PORTAL_STATE_CONNECTING || current == PORTAL_STATE_CONNECTED) {
                char msg[96];
                uint8_t reason = disc ? disc->reason : 0;
                snprintf(msg, sizeof(msg), "Falha a ligar (motivo %u).", (unsigned)reason);
                portal_mark_error(msg);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        portal_mark_connected();
    }
}

// Inicializa o estado interno com base na ligação STA existente.
// Evita mostrar estado incorreto ao abrir o portal após reboot.
static void portal_init_state(void)
{
    system_status_snapshot_t snapshot = {0};
    system_status_snapshot(&snapshot);
    if (snapshot.wifi.sta_connected) {
        portal_mark_connected();
    } else {
        portal_set_state(PORTAL_STATE_IDLE, NULL);
    }
}

// Ponto de entrada público que registra handlers e garante servidor ativo.
// Chamado pelo app_main antes de disponibilizar o modo de configuração.
void config_portal_server_init(void)
{
    portal_init_state();

    if (!s_portal_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                                   &portal_wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP,
                                                   &portal_wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   &portal_wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &portal_wifi_event_handler, NULL));
        s_portal_handlers_registered = true;
    }

    portal_start_server();
}
