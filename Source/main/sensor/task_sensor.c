// Task responsável por ler o DHT20, gerir standby e sincronizar dados com SD/MQTT.
// Integra o módulo de sensores e orquestra notificações para outros componentes.
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sensor_dht20.h"
#include "led.h"
#include "config_manager.h"
#include "logging.h"
#include "sensor_data_store.h"
#include "task_config.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "task_sensor.h"
#include "task_mqtt.h"
#include "config_runtime.h"
#include "https_server_app.h"
#include "system_status.h"
#include "display.h"

#define CONFIG_RELOAD_INTERVAL_MS 5000U
#define CONFIG_RELOAD_TIMEOUT_MS  2000U
#define DATA_LOG_MAX_CONSECUTIVE_ERRORS 2U      //numeros de erros de logs
#define TAG "TASK_SENSOR"
#define SENSOR_DATA_PATH "/sdcard/data.txt"

static void wait_for_sd_card(void);
static void reload_user_config_from_sd(void);
static void handle_period_update_notification(uint32_t note);
static bool handle_sensor_notification(uint32_t note, bool *paused);
static void append_sd_log_if_enabled(float temperature, float humidity);
static void ensure_data_log_cleared(void);
static void mark_data_log_needs_clear(void);
static void suppress_i2c_logs(void);
static void set_led_normal_if_mqtt_disconnected(const char *reason);

static bool s_data_file_cleared = false;
static uint32_t s_data_log_error_streak = 0;

TaskHandle_t task_sensor_handle = NULL;

// Handle da tarefa do display (para notificação)
extern TaskHandle_t task_display_handle;

// Task FreeRTOS que gere o sensor DHT20, logs no SD e notificações às outras tasks.
// Criada por app_main e corre continuamente enquanto houver SD e energia.
void task_sensor(void *pvParameters)
{
    // Guarda o handle da task para que outras tasks possam notificá-la
    task_sensor_handle = xTaskGetCurrentTaskHandle();
    suppress_i2c_logs();

    // Declara handles para o bus I2C e dispositivo DHT20
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dht;
    const TickType_t reload_interval_ticks = pdMS_TO_TICKS(CONFIG_RELOAD_INTERVAL_MS);
    bool period_is_valid = true;
    bool paused = false;
    system_status_set_standby_active(false);

    // Inicializa o sensor DHT20
    if (dht20_init(&bus, &dht) != ESP_OK) {
        // Falha crítica na inicialização do sensor
        log_error(TAG, "Erro a inicializar DHT20");
        led_set_state_with_reason(LED_STATE_ERROR, "falha ao inicializar o sensor DHT20");
        vTaskDelete(NULL);
    }

    // Variáveis para armazenar temperatura e humidade
    float t, h;

    // Loop principal da task
    while (1) {
        // Verifica se o cartão SD está disponível e funcional
        if (!config_manager_is_sd_available() || !config_manager_check_sd_alive()) {
            wait_for_sd_card();
        }

        // Garante que o ficheiro de log está limpo se necessário
        ensure_data_log_cleared();

        // Verifica se há notificações pendentes (sem bloquear)
        uint32_t notify_val = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &notify_val, 0) == pdTRUE) {
            // Processa a notificação recebida
            bool standby_changed = handle_sensor_notification(notify_val, &paused);
            handle_period_update_notification(notify_val);
            
            // Se o estado de standby mudou, notifica o display
            if (standby_changed && task_display_handle != NULL) {
                xTaskNotifyGive(task_display_handle);
            }
        }

        // Se o sensor está em modo de pausa (standby)
        if (paused) {
            led_set_state_with_reason(LED_STATE_ERROR, "modo de espera ativado pelo botão");
            
            // Fica em loop esperando sair do modo pausa
            while (paused) {
                uint32_t pause_note = 0;
                // Aguarda indefinidamente por uma notificação
                if (xTaskNotifyWait(0, UINT32_MAX, &pause_note, portMAX_DELAY) == pdTRUE) {
                    bool standby_changed = handle_sensor_notification(pause_note, &paused);
                    handle_period_update_notification(pause_note);
                    if (standby_changed && task_display_handle != NULL) {
                        xTaskNotifyGive(task_display_handle);
                    }
                }
            }
            // Saiu do modo pausa, restaura LED normal se MQTT desligado
            set_led_normal_if_mqtt_disconnected("retomar após modo de espera");
            continue;
        }

        // Obtém o período de leitura atual da configuração
        uint32_t current_period_ms = config_manager_get_period_ms();
        bool current_period_valid = (current_period_ms >= CONFIG_MANAGER_PERIOD_MIN_MS) &&
                                    (current_period_ms <= CONFIG_MANAGER_PERIOD_MAX_MS);

        // Se o período configurado é inválido
        if (!current_period_valid) {
            // Loga erro apenas na primeira deteção
            if (period_is_valid) {
                log_error(TAG, "Período inválido (%lu ms). LED em erro até atualizar o SD.",
                          (unsigned long)current_period_ms);
            }
            period_is_valid = false;
            led_set_state_with_reason(LED_STATE_ERROR, "período inválido carregado");
            
            // Aguarda notificação ou timeout para verificar novamente
            uint32_t wait_note = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &wait_note, reload_interval_ticks) == pdTRUE) {
                bool standby_changed = handle_sensor_notification(wait_note, &paused);
                handle_period_update_notification(wait_note);
                if (standby_changed && task_display_handle != NULL) {
                    xTaskNotifyGive(task_display_handle);
                }
            }
            continue;
        } else if (!period_is_valid) {
            // Período tornou-se válido novamente
            period_is_valid = true;
            log_info(TAG, "Período válido restaurado (%lu ms).", (unsigned long)current_period_ms);
        }

        // Controla se já foi registado um erro do sensor (evita spam de logs)
        static bool sensor_fault_noted = false;
        
        // Tenta ler temperatura e humidade do DHT20
        if (dht20_read(&t, &h, dht) == ESP_OK) {
            // Leitura bem-sucedida
            sensor_data_store_set(t, h);
            ESP_LOGI("DHT20", "Temperatura: %.2f °C | Humidade: %.2f %%", t, h);
            append_sd_log_if_enabled(t, h);
            sensor_fault_noted = false;
            
            // Verifica novamente se SD está disponível
            if (!config_manager_is_sd_available()) {
                continue;
            }

            // Notifica a task_display para atualizar o ecrã
            if (task_display_handle != NULL) {
                xTaskNotifyGive(task_display_handle);
            }
            
            // Notifica a task_mqtt para publicar os dados
            if (task_mqtt_handle != NULL) {
                xTaskNotify(task_mqtt_handle, TASK_MQTT_NOTIFY_DATA_BIT, eSetBits);
            }
            set_led_normal_if_mqtt_disconnected("leituras válidas do DHT20");
        } else {
            // Erro ao ler o sensor
            log_error(TAG, "Erro ao ler o DHT20");
            
            // Regista aviso apenas na primeira falha
            if (!sensor_fault_noted) {
                ESP_LOGE("DHT20", "Leitura falhou. Verifica o SENSOR DHT20.");
                log_warn(TAG, "Verifique a posição do sensor DHT20.");
                sensor_fault_noted = true;
            }
            
            // Invalida os dados do sensor e sinaliza erro no LED
            sensor_data_store_invalidate();
            led_set_state_with_reason(LED_STATE_ERROR, "erro ao ler o DHT20");
            
            // Notifica o display para mostrar erro
            if (task_display_handle != NULL) {
                xTaskNotifyGive(task_display_handle);
            }
        }

        // Aguarda o período configurado ou uma notificação
        TickType_t wait_ticks = pdMS_TO_TICKS(current_period_ms);
        uint32_t wait_note = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &wait_note, wait_ticks) == pdTRUE) {
            // Recebeu notificação antes do timeout
            bool standby_changed = handle_sensor_notification(wait_note, &paused);
            handle_period_update_notification(wait_note);
            if (standby_changed && task_display_handle != NULL) {
                xTaskNotifyGive(task_display_handle);
            }
        }
        // Se não recebeu notificação, o timeout expirou e o loop reinicia
    }
}

// Desliga os logs verbosos do driver I2C para não poluir o terminal.
// Chamado uma vez antes de iniciar o ciclo principal.
static void suppress_i2c_logs(void)
{
    static bool muted = false;
    if (muted) {
        return;
    }
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("i2c", ESP_LOG_NONE);
    muted = true;
}

// Repõe o LED verde se o MQTT não estiver ligado para evitar alertas persistentes.
// Usado ao sair de standby ou após leituras válidas.
static void set_led_normal_if_mqtt_disconnected(const char *reason)
{
    if (!system_status_is_mqtt_connected()) {
        led_set_state_with_reason(LED_STATE_NORMAL, reason);
    }
}

// API que outras tasks usam para sinalizar que o período foi alterado.
// Envia um bit de notificação à task_sensor a partir de contexto task/ISR.
esp_err_t task_sensor_notify_period_update(void)
{
    // Permite outras tasks sinalizarem que o período foi alterado (p.ex. via HTTPS).
    if (task_sensor_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t higher_woken = pdFALSE;
    BaseType_t res;

    if (xPortInIsrContext()) {
        res = xTaskNotifyFromISR(task_sensor_handle,
                                 TASK_SENSOR_NOTIFY_PERIOD_UPDATE_BIT,
                                 eSetBits,
                                 &higher_woken);
        if (higher_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    } else {
        res = xTaskNotify(task_sensor_handle,
                          TASK_SENSOR_NOTIFY_PERIOD_UPDATE_BIT,
                          eSetBits);
    }

    return (res == pdPASS) ? ESP_OK : ESP_FAIL;
}

// Trata notificações vindas do botão ou de pedidos de atualização de período.
// Atualiza o estado de standby e indica se houve mudança.
static bool handle_sensor_notification(uint32_t note, bool *paused)
{
    if (!paused) {
        return false;
    }

    bool state_changed = false;

    if (note == 1U) {
        if (!*paused) {
            state_changed = true;
        }
        *paused = true;
        log_warn(TAG, "Botão premido: sensor em modo de espera.");
        ESP_LOGE(TAG, "=== SENSOR EM STANDBY ===");
        system_status_set_standby_active(true);
    } else if (note == 2U) {
        bool was_paused = *paused;
        *paused = false;
        if (was_paused) {
            state_changed = true;
            log_info(TAG, "Botão largado: retomar leituras.");
            ESP_LOGW(TAG, "Sensor retomado após standby.");
        }
        system_status_set_standby_active(false);
    } else if (note & TASK_SENSOR_NOTIFY_PERIOD_UPDATE_BIT) {
        // Apenas desperta a tarefa para aplicar novo período.
    }

    return state_changed;
}

// Recarrega a configuração apenas quando o bit de período está presente.
// Evita leituras desnecessárias do SD.
static void handle_period_update_notification(uint32_t note)
{
    if (!(note & TASK_SENSOR_NOTIFY_PERIOD_UPDATE_BIT)) {
        return;
    }

    // Só recarrega a configuração quando o dispositivo pede, evitando leituras periódicas do SD.
    reload_user_config_from_sd();
}

// Solicita à task_config que recarregue o ficheiro config_user no SD.
// Usado sempre que o utilizador altera o período via interface web.
static void reload_user_config_from_sd(void)
{
    const TickType_t reload_timeout = pdMS_TO_TICKS(CONFIG_RELOAD_TIMEOUT_MS);
    log_info(TAG, "A recarregar configuração do período a partir do SD.");
    // Executa o pedido de bloquear para obter o período acabado de gravar no cartão SD.
    esp_err_t reload_status = task_config_request_reload(reload_timeout);
    if (reload_status == ESP_OK) {
        uint32_t refreshed_period = config_manager_get_period_ms();
        log_info(TAG, "Período atualizado para %lu ms após notificação.",
                 (unsigned long)refreshed_period);
    } else if (reload_status == ESP_ERR_TIMEOUT) {
        log_warn(TAG, "Timeout a recarregar configuração após notificação.");
    } else {
        log_warn(TAG, "Falha ao recarregar configuração (%s).",
                 esp_err_to_name(reload_status));
    }
}

// Escreve no data.txt apenas se o toggle SD estiver ativo.
// Também controla falhas consecutivas para desligar o SD com segurança.
static void append_sd_log_if_enabled(float temperature, float humidity)
{
    if (!config_runtime_get_sd_logging_enabled()) {
        return;
    }

    if (!config_manager_is_sd_available()) {
        log_warn(TAG, "Logging no SD está ativo mas o cartão não está disponível.");
        return;
    }

    FILE *file = fopen(SENSOR_DATA_PATH, "a");
    if (!file) {
        s_data_log_error_streak++;
        log_warn(TAG,
                 "Não foi possível abrir %s para escrita. Falha %u/%u antes de desativar SD.",
                 SENSOR_DATA_PATH,
                 (unsigned)s_data_log_error_streak,
                 (unsigned)DATA_LOG_MAX_CONSECUTIVE_ERRORS);
        if (s_data_log_error_streak >= DATA_LOG_MAX_CONSECUTIVE_ERRORS) {
            mark_sd_card_unavailable("falhas consecutivas ao abrir data.txt para append");
        }
        return;
    }

    char timestamp[32];
    bool has_timestamp = false;
    time_t now = time(NULL);
    if (now > 0) {
        struct tm tm_info;
        if (localtime_r(&now, &tm_info)) {
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
            has_timestamp = true;
        }
    }

    if (!has_timestamp) {
        long long ms = (long long)(esp_timer_get_time() / 1000ULL);
        snprintf(timestamp, sizeof(timestamp), "%lld", ms);
    }

    int written = fprintf(file, "%s, %.2f, %.2f\n", timestamp, temperature, humidity);
    fclose(file);
    if (written <= 0) {
        s_data_log_error_streak++;
        log_warn(TAG,
                 "Falha ao escrever dados no ficheiro do SD. Falha %u/%u antes de desativar SD.",
                 (unsigned)s_data_log_error_streak,
                 (unsigned)DATA_LOG_MAX_CONSECUTIVE_ERRORS);
        if (s_data_log_error_streak >= DATA_LOG_MAX_CONSECUTIVE_ERRORS) {
            mark_sd_card_unavailable("falhas consecutivas ao escrever data.txt");
        }
    } else {
        s_data_log_error_streak = 0;
        system_status_note_data_log_write();
    }
}

// Garante que o ficheiro data.txt é limpo no arranque ou após remoção do SD.
// Evita que dados de sessões anteriores causem confusão.
static void ensure_data_log_cleared(void)
{
    if (s_data_file_cleared) {
        return;
    }

    FILE *file = fopen(SENSOR_DATA_PATH, "w");
    if (!file) {
        log_warn(TAG, "Não foi possível limpar ficheiro de dados (%s).", SENSOR_DATA_PATH);
        return;
    }
    fclose(file);
    s_data_file_cleared = true;
    system_status_reset_data_log_stats();
    log_info(TAG, "Ficheiro de dados limpo em %s.", SENSOR_DATA_PATH);
}

// Marca que o ficheiro data.txt deve ser recriado antes da próxima escrita.
// Chamado quando o SD é removido ou invalidado.
static void mark_data_log_needs_clear(void)
{
    s_data_file_cleared = false;
}

// Entrar em modo de espera até que o SD fique disponível novamente.
// Suspende Wi-Fi/HTTPS, limpa estados e tenta remontar periodicamente.
static void wait_for_sd_card(void)
{
    // Variáveis estáticas para manter estado entre chamadas
    static bool notified = false;
    static bool sd_logs_suppressed = false;
    static bool wifi_suspended = false;
    static bool off_message_shown = false;
    static bool config_logs_suppressed = false;
    static esp_log_level_t prev_config_level = ESP_LOG_INFO;
    static bool sdmmc_logs_suppressed = false;
    static esp_log_level_t prev_sdmmc_level = ESP_LOG_WARN;
    static esp_log_level_t prev_diskio_level = ESP_LOG_WARN;
    const TickType_t retry_delay = pdMS_TO_TICKS(CONFIG_RELOAD_INTERVAL_MS);

    // Marca que o ficheiro de dados precisa ser limpo quando o SD voltar
    mark_data_log_needs_clear();

    // Invalida os dados do sensor armazenados
    sensor_data_store_invalidate();
    // Muda LED para vermelho indicando erro de SD
    led_set_state_with_reason(LED_STATE_ERROR, "cartão SD não inserido");

    // Para o servidor HTTPS para libertar recursos
    https_server_app_stop();

    // Tenta parar o Wi-Fi para economizar energia
    if (!wifi_suspended) {
        esp_err_t wifi_stop = esp_wifi_stop();
        if (wifi_stop == ESP_OK) {
            wifi_suspended = true;
        }
    }

    // Suprime logs verbosos na primeira entrada para evitar poluição do terminal
    if (!notified) {
        // Suprime logs do sistema de ficheiros FAT
        if (!sd_logs_suppressed) {
            esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
            sd_logs_suppressed = true;
        }
        // Suprime logs do driver SD/MMC
        if (!sdmmc_logs_suppressed) {
            prev_sdmmc_level = esp_log_level_get("sdmmc_cmd");
            prev_diskio_level = esp_log_level_get("diskio_sdmmc");
            esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
            esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);
            sdmmc_logs_suppressed = true;
        }
        // Suprime logs do config_manager
        if (!config_logs_suppressed) {
            prev_config_level = esp_log_level_get("config_manager");
            esp_log_level_set("config_manager", ESP_LOG_NONE);
            config_logs_suppressed = true;
        }
        // Mostra mensagem de erro apenas uma vez
        if (!off_message_shown) {
            log_error(TAG, "Cartão SD removido; operações suspensas.");
            display_show_sd_removed();
            off_message_shown = true;
        }
        notified = true;
    }

    // Loop de espera até o SD ficar disponível
    while (!config_manager_is_sd_available()) {
        // Tenta parar Wi-Fi novamente se ainda não foi parado
        if (!wifi_suspended) {
            esp_err_t wifi_stop = esp_wifi_stop();
            if (wifi_stop == ESP_OK || wifi_stop == ESP_ERR_WIFI_NOT_INIT || wifi_stop == ESP_ERR_WIFI_NOT_STARTED) {
                wifi_suspended = true;
            }
        }
        // Tenta montar o cartão SD
        if (config_manager_mount_sd() == ESP_OK) {
            // Se o SD requer restart, reinicia o sistema para limpeza completa
            if (config_manager_should_restart_on_sd_return()) {
                esp_restart();
            }
            break;
        }
        // Aguarda antes de tentar novamente
        vTaskDelay(retry_delay);
    }

    // Reseta flags para próxima eventual falha do SD
    notified = false;
    off_message_shown = false;
    
    // Restaura níveis de log anteriores
    if (sd_logs_suppressed) {
        esp_log_level_set("vfs_fat_sdmmc", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
        sd_logs_suppressed = false;
    }
    if (sdmmc_logs_suppressed) {
        esp_log_level_set("sdmmc_cmd", prev_sdmmc_level);
        esp_log_level_set("diskio_sdmmc", prev_diskio_level);
        sdmmc_logs_suppressed = false;
    }
    if (config_logs_suppressed) {
        esp_log_level_set("config_manager", prev_config_level);
        config_logs_suppressed = false;
    }

    // Solicita recarga da configuração do SD
    esp_err_t reload_status = task_config_request_reload(pdMS_TO_TICKS(CONFIG_RELOAD_TIMEOUT_MS));
    if (reload_status == ESP_OK) {
        log_info(TAG, "Cartão SD disponível novamente; configuração recarregada.");
    } else {
        log_warn(TAG, "Cartão SD disponível mas falhou recarregar config (%s).",
                 esp_err_to_name(reload_status));
    }

    // Se o Wi-Fi foi suspenso, tenta retomar ligação
    if (wifi_suspended) {
        char wifi_ssid[33] = {0};
        char wifi_pass[65] = {0};
        if (load_wifi_config(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass))) {
            esp_err_t wifi_status = wifi_connect_with_credentials(wifi_ssid, wifi_pass);
            if (wifi_status == ESP_OK) {
                log_info(TAG, "Wi-Fi retomado após reinserção do cartão SD.");
            } else {
                log_warn(TAG, "Wi-Fi não retomou (%s) após reinserção do SD.",
                         esp_err_to_name(wifi_status));
            }
        } else {
            log_warn(TAG, "Configuração Wi-Fi indisponível após reinserção do SD; ligação não retomada.");
        }
        wifi_suspended = false;
    }
}
