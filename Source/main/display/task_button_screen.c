// Lê o botão dedicado à mudança de ecrãs no display e notifica a task_display.
// Parte do módulo de interface que gere a navegação no TFT.
#include "task_button_screen.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

#include "display.h"
#include "task_brightness.h"
#include "board_pins.h"

#define SCREEN_BUTTON_DEBOUNCE_MS 50
#define SCREEN_BUTTON_POLL_MS     10

extern TaskHandle_t task_display_handle;

static const char *TAG = "task_button_screen";

// Task de polling do botão de ecrãs com debounce simples.
// Cria notificações para a task_display assim que o utilizador pressiona.
static void task_button_screen(void *pvParameters)
{
    (void)pvParameters;
    int last_level = gpio_get_level(SCREEN_BUTTON_GPIO);

    while (1) {
        int level = gpio_get_level(SCREEN_BUTTON_GPIO);
        if (level != last_level) {
            vTaskDelay(pdMS_TO_TICKS(SCREEN_BUTTON_DEBOUNCE_MS));
            int confirm_level = gpio_get_level(SCREEN_BUTTON_GPIO);
            if (confirm_level == level) {
                last_level = confirm_level;
                if (confirm_level == 0) {
                    display_screen_t screen = display_get_screen(); //ecra atual
                    screen = (screen + 1) % SCREEN_MAX;             //passa para o próximo ecra
                    display_set_screen(screen);
                    task_brightness_notify_user_activity();         //envia notificação para o display, para dizer que esta acordado
                    if (task_display_handle != NULL) {
                        xTaskNotifyGive(task_display_handle);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCREEN_BUTTON_POLL_MS));
    }
}

// Configura o GPIO do botão e cria a task que faz o polling.
// Chamado no app_main logo após iniciar o display.
void task_button_screen_start(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SCREEN_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar GPIO23 para o botão (%s)", esp_err_to_name(err));
        return;
    }

    if (xTaskCreate(task_button_screen, "task_button_screen", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Não foi possível criar a task do botão de ecrãs.");
    }
}
