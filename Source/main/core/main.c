// Função principal que coordena a inicialização de todos os módulos.
// Parte do core e prepara sensores, Wi-Fi, servidores e tarefas.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "config_manager.h"
#include "led.h"
#include "logging.h"
#include "sensor_dht20.h"
#include "task_brightness.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "sensor_data_store.h"
#include "https_server_app.h"
#include "task_config.h"
#include "task_sensor.h"
#include "task_mqtt.h"
#include "task_button.h"
#include "task_button_screen.h"
#include "driver/gpio.h"
#include "board_pins.h"
#include "system_status.h"
#include "config_portal_server.h"
#include "config_runtime.h"

static const char *TAG = "app";

// protótipos
extern void task_display(void *pv);

// Reduz o ruído de logs dos drivers IDF para focar no output da aplicação.
// Chamado logo após o logger estar disponível.
static void suppress_driver_logs(void)
{
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("phy", ESP_LOG_ERROR);
    esp_log_level_set("net80211", ESP_LOG_ERROR);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_ERROR);
    esp_log_level_set("sdspi_transaction", ESP_LOG_ERROR);
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("i2c", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
}

// Entry point do firmware 

void app_main(void)
{
    // Inicializa subsistemas de estado e configuração em runtime
    system_status_init();
    config_runtime_init();
    ESP_LOGI(TAG, "Arranque da app.");
    system_status_set_data_logging_enabled(config_runtime_get_sd_logging_enabled());
    
    // Tenta inicializar o sistema de logging no SD
    esp_err_t log_status = log_init();
    if (log_status != ESP_OK) {
        // Se falhar, continua sem logging persistente
        log_warn(TAG, "Logger indisponível (%s). Continuar sem SD?",
                 esp_err_to_name(log_status));
    }

    // Reduz ruído de logs dos drivers ESP-IDF
    suppress_driver_logs();

    log_info(TAG, "Arranque do sistema.");

    // Regista timestamp de boot em milissegundos
    int64_t boot_ms = esp_timer_get_time() / 1000;
    log_info(TAG, "Boot timestamp (uptime): %lld ms", (long long)boot_ms);

    // Verifica se o relógio RTC está sincronizado
    time_t now = time(NULL);
    struct tm tm_info;
    if (now > 1672531200 && localtime_r(&now, &tm_info)) { 
        // RTC válido (timestamp posterior a 01/01/2023)
        char boot_buf[32];
        strftime(boot_buf, sizeof(boot_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
        log_info(TAG, "Hora do boot (RTC): %s", boot_buf);
    } else {
        // RTC ainda não sincronizado via SNTP
        log_warn(TAG, "Relógio ainda não sincronizado; a aguardar SNTP.");
    }

    // Obtém informações do firmware compilado
    const esp_app_desc_t *desc = esp_app_get_description();
    log_info(TAG, "Firmware: %s v%s (%s %s)",
             desc->project_name[0] ? desc->project_name : "app",
             desc->version,
             desc->date,
             desc->time);

    // Tenta obter o UID único do dispositivo da eFuse
    char device_uid[64] = {0};
    esp_err_t uid_status = config_manager_get_device_uid(device_uid, sizeof(device_uid));
    if (uid_status == ESP_OK) {
        // UID obtido com sucesso
        log_info(TAG, "UID do dispositivo: %s", device_uid);
    } else {
        // Falha ao ler UID do fabricante
        log_error(TAG, "Não foi possível obter UID do fabricante (%s).",
                  esp_err_to_name(uid_status));
    }
    system_status_set_firmware_info(desc->version,
                                    (uid_status == ESP_OK && device_uid[0]) ? device_uid : "desconhecido");

    // Inicializa periféricos básicos
    led_init();  // inicializa o LED
    led_set_state_with_reason(LED_STATE_NORMAL, "estado inicial do sistema");
    sensor_data_store_init();

    // Prepara buffers para credenciais Wi-Fi
    char wifi_ssid[33] = {0};
    char wifi_pass[65] = {0};
    bool wifi_started = false;

    // Tenta montar o cartão SD
    esp_err_t sd_status = config_manager_mount_sd();

    if (sd_status != ESP_OK) {
        // SD não disponível
        ESP_LOGE(TAG, "SD falhou (%s).", esp_err_to_name(sd_status));
    }

    // Se o SD está montado, tenta carregar credenciais Wi-Fi
    if (sd_status == ESP_OK) {
        ESP_LOGI(TAG, "SD OK.");
        if (load_wifi_config(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass))) {
            // Credenciais carregadas, tenta conectar ao Wi-Fi em modo STA
            esp_err_t wifi_status = wifi_connect_with_credentials(wifi_ssid, wifi_pass);
            if (wifi_status != ESP_OK) {
                // Falha na conexão Wi-Fi
                ESP_LOGE(TAG, "Wi-Fi não iniciou (%s).", esp_err_to_name(wifi_status));
            } else {
                // Conexão Wi-Fi bem-sucedida
                wifi_started = true;
            }
        } else {
            // Não há credenciais válidas no SD
            ESP_LOGE(TAG, "Wi-Fi sem configuração válida; arrancar apenas o SoftAP.");
        }
    }

    // Se o Wi-Fi não iniciou em modo STA, ativa o SoftAP como fallback
    if (!wifi_started) {
        esp_err_t fallback_status = wifi_connect_with_credentials(NULL, NULL);
        if (fallback_status != ESP_OK) {
            // Falha ao iniciar SoftAP
            ESP_LOGE(TAG, "SoftAP não iniciou (%s).", esp_err_to_name(fallback_status));
        } else {
            // SoftAP ativo, utilizador pode configurar via portal
            ESP_LOGW(TAG, "SoftAP ativo; configure o Wi-Fi via dashboard.");
            wifi_started = true;
        }
    }
    
    // Inicializa servidores web (HTTP e HTTPS)
    config_portal_server_init();  // servidor http
    https_server_app_init();      // servidor https

    // Cria a task de carregamento de configuração
    if (xTaskCreate(task_config_loader, "task_config_loader", 4096, NULL, 5, &task_config_handle) != pdPASS) {
        // Falha crítica ao criar task
        log_error(TAG, "Não foi possível iniciar task_config_loader.");
        return;
    }

    // Solicita carregamento inicial da configuração do SD
    esp_err_t cfg_status = task_config_request_initial_load(pdMS_TO_TICKS(5000));
    if (cfg_status != ESP_OK) {
        // SD indisponível, usar defaults
        log_warn(TAG, "Sem acesso ao cartão SD (%s); a utilizar valores por defeito.",
                 esp_err_to_name(cfg_status));
    }

    // Valida o período de leitura configurado
    const uint32_t period_min_ms = CONFIG_MANAGER_PERIOD_MIN_MS;
    const uint32_t period_max_ms = CONFIG_MANAGER_PERIOD_MAX_MS;
    uint32_t current_period_ms = config_manager_get_period_ms();

    // Verifica se o período está dentro do intervalo permitido [1 s - 3600 s]
    if (current_period_ms < period_min_ms || current_period_ms > period_max_ms) {
        // Período inválido, não inicia as tasks de leitura
        log_error(TAG, "Erro: valor de período inválido (%lu ms). Deve estar entre %lu e %lu ms.",
                  (unsigned long)current_period_ms,
                  (unsigned long)period_min_ms,
                  (unsigned long)period_max_ms);
        log_error(TAG, "Não é possível iniciar as leituras com este valor.");
        led_set_state_with_reason(LED_STATE_ERROR, "período inválido ao arrancar");
        return; // não cria as tarefas
    }

    log_info(TAG, "Período configurado: %lu ms",
             (unsigned long)current_period_ms);

    // Inicia todas as tasks do sistema
    task_brightness_start();
    xTaskCreate(task_display, "task_display", 4096, NULL, 4, NULL);
    task_button_screen_start();
    xTaskCreate(task_sensor, "task_sensor", 4096, NULL, 5, NULL);
    task_mqtt_start(&g_config_user);
    
    // Configura o GPIO do botão físico
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    task_button_start();
}
