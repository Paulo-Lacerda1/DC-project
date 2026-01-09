// Task responsável por sincronizar dados com o módulo de display TFT.
// Pertence ao subsistema de interface visual do dispositivo.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "esp_log.h"
#include "sensor_dht20.h"
#include "sensor_data_store.h"
#include "config_manager.h"
#include "display.h"
#include "system_status.h"

TaskHandle_t task_display_handle = NULL;

static const char *TAG = "task_display";


// Aguarda uma janela curta até o cartão SD estar pronto antes de iniciar o display.
// Evita que o driver ST7735 consuma o mesmo bus SPI enquanto o SD monta.
static void wait_sd_before_display(void)
{
    const TickType_t wait_step = pdMS_TO_TICKS(200);
    const TickType_t max_wait = pdMS_TO_TICKS(2000);
    TickType_t start = xTaskGetTickCount();

    while (!config_manager_is_sd_available()) {
        if (config_manager_mount_sd() == ESP_OK) {
            break;
        }

        if ((xTaskGetTickCount() - start) >= max_wait) {
            ESP_LOGW(TAG, "Cartão SD indisponível após espera; continuar com display mesmo assim.");
            break;
        }

        vTaskDelay(wait_step);
    }
}

// Task FreeRTOS que aguarda notificações de novas leituras e atualiza o TFT.
// Criada no app_main e executa um loop infinito a desenhar o ecrã ativo.
void task_display(void *pvParameters)
{
    // Guarda o handle da task para receber notificações
    task_display_handle = xTaskGetCurrentTaskHandle();

    // Aguarda o SD estar pronto antes de inicializar o display (partilham SPI)
    wait_sd_before_display();

    // Inicializa o driver do display ST7735
    if (display_init() != ESP_OK) {
        // Falha crítica, termina a task
        ESP_LOGE(TAG, "Falha ao inicializar o módulo do display.");
        vTaskDelete(NULL);
    }

    // Variáveis para armazenar últimos valores válidos
    float last_temp = 0.0f;
    float last_hum = 0.0f;
    bool has_valid_data = false;
    
    // Define o ecrã principal como ativo
    display_set_screen(SCREEN_MAIN);

    // Exibe mensagem de boas-vindas por 5 segundos
    display_show_welcome_message();
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // Limpa o ecrã e desenha a interface principal
    display_clear();
    display_draw_table(last_temp, last_hum, false, system_status_is_standby_active());

    // Loop principal de atualização do display
    while (1) {
        // Aguarda notificação de nova leitura do sensor (bloqueante)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Tenta obter dados do sensor
        sensor_data_t data;
        bool sensor_ok = sensor_data_store_get(&data);
        
        if (sensor_ok) {
            // Dados válidos recebidos, atualiza valores locais
            last_temp = data.temperature;
            last_hum = data.humidity;
            has_valid_data = true;
            
            // Armazena medição para o gráfico
            display_store_measurement(last_temp, last_hum);
        }

        // Define valores a mostrar (usa últimos válidos ou zero)
        float temp_to_display = has_valid_data ? last_temp : 0.0f;
        float hum_to_display = has_valid_data ? last_hum : 0.0f;

        // Verifica se o sistema está em standby
        bool standby_active = system_status_is_standby_active();
        
        // Desenha o ecrã conforme a seleção atual
        switch (display_get_screen()) {
        case SCREEN_MAIN:
            // Ecrã principal com tabela de dados
            display_draw_table(temp_to_display, hum_to_display, sensor_ok, standby_active);
            break;
        case SCREEN_LOGS:
            // Ecrã de logs do sistema
            display_draw_logs();
            break;
        case SCREEN_GRAPH_TEMP_HUM:
            // Ecrã com gráfico de temperatura e humidade
            display_draw_graph_temp_hum();
            break;
        default:
            // Ecrã desconhecido, volta ao principal
            display_draw_table(temp_to_display, hum_to_display, sensor_ok, standby_active);
            break;
        }
    }
}
