// Monitoriza o botão físico e envia eventos ao controlador de sensores.
// Pertence ao módulo de entrada manual acionado pelo utilizador.
#include "task_button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "board_pins.h"
#include "task_sensor.h"

#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_POLL_MS     10

// Task que faz polling do botão com debounce e gera notificações para task_sensor.
// Criada em task_button_start e fica ativa durante todo o ciclo de vida.
static void task_button(void *pvParameters)
{
    (void)pvParameters;
    // Lê o estado inicial do botão (1 = não pressionado, 0 = pressionado)
    bool last_level = gpio_get_level(PIN_BUTTON);

    while (1) {
        // Lê o estado atual do botão
        bool level = gpio_get_level(PIN_BUTTON);
        
        // Se o estado mudou desde a última leitura
        if (level != last_level) {
            // Aguarda um tempo para eliminar ruído (debounce)
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            
            // Confirma se o estado continua o mesmo após o debounce
            bool confirm = gpio_get_level(PIN_BUTTON);
            if (confirm == level) {
                // Estado confirmado, atualiza o último nível conhecido
                last_level = level;
                
                // botão pressionado 
                if (level == 0) {
                    // Sinaliza ao sensor que o utilizador quer entrar em standby.
                    if (task_sensor_handle != NULL) {
                        xTaskNotify(task_sensor_handle, 1, eSetValueWithOverwrite);
                    }
                } else {
                    // botão libertado
                    // Liberta o sensor para retomar o loop após sair de standby.
                    if (task_sensor_handle != NULL) {
                        xTaskNotify(task_sensor_handle, 2, eSetValueWithOverwrite);
                    }
                }
            }
        }
        
        // Aguarda antes da próxima verificação para não sobrecarregar a CPU
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

// Entrada pública que cria a task do botão logo após configurar o GPIO.
// Chamado por app_main para permitir que o utilizador pause/resume leituras.
void task_button_start(void)
{
    xTaskCreate(task_button, "task_button", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
}
