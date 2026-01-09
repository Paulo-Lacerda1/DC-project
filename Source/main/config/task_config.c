// Coordena pedidos de (re)carregamento da configuração no cartão SD.
// Pertence ao módulo de configuração que sincroniza task_sensor e serviços HTTP.
#include "task_config.h"

#include <limits.h>
#include <stdbool.h>

#include "freertos/portmacro.h"
#include "config_manager.h"
#include "logging.h"

#define CONFIG_TASK_NOTIFY_INIT_BIT     (1U << 0)
#define CONFIG_TASK_NOTIFY_RELOAD_BIT   (1U << 1)

TaskHandle_t task_config_handle = NULL;

static const char *TAG = "task_config";
static bool s_config_initialized = false;
static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_waiting_task = NULL;

static void notify_requester(esp_err_t status);
static esp_err_t issue_command(uint32_t command_bit, TickType_t wait_ticks);

// Task principal que recebe notificações para inicializar ou recarregar config.
// Criada pelo app_main para serializar acesso ao SD e evitar corridas.
void task_config_loader(void *pvParameters)
{
    // Guarda o handle da task para permitir notificações externas
    task_config_handle = xTaskGetCurrentTaskHandle();

    // Loop principal que processa comandos de configuração
    while (1) {
        uint32_t command = 0;
        
        // Aguarda indefinidamente por uma notificação com comando
        BaseType_t notified = xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY);
        if (notified != pdTRUE) {
            // Sem notificação recebida, continua aguardando
            continue;
        }

        esp_err_t status = ESP_OK;
        
        // Valida que não foram pedidos init e reload simultaneamente
        if ((command & CONFIG_TASK_NOTIFY_INIT_BIT) && (command & CONFIG_TASK_NOTIFY_RELOAD_BIT)) {
            log_error(TAG, "Comando inválido (init+reload ao mesmo tempo).");
            status = ESP_ERR_INVALID_ARG;
            notify_requester(status);
            continue;
        }

        // Se recebeu comando de inicialização
        if (command & CONFIG_TASK_NOTIFY_INIT_BIT) {
            status = config_manager_init();
            if (status == ESP_OK) {
                // Inicialização bem-sucedida
                s_config_initialized = true;
            } else {
                // Falha na inicialização
                s_config_initialized = false;
            }
            notify_requester(status);
            continue;
        }

        // Se recebeu comando de reload
        if (command & CONFIG_TASK_NOTIFY_RELOAD_BIT) {
            if (!s_config_initialized) {
                // Não pode fazer reload sem inicialização prévia
                status = ESP_ERR_INVALID_STATE;
            } else {
                // Guarda período atual antes do reload
                uint32_t previous_period = config_manager_get_period_ms();
                
                // Recarrega configuração do SD
                status = config_manager_reload_user_config();
                
                // Verifica se o período mudou
                uint32_t updated_period = config_manager_get_period_ms();
                if (status == ESP_OK && updated_period != previous_period) {
                    log_info(TAG, "Período requisitado no SD: %lu ms.",
                             (unsigned long)updated_period);
                }
            }
            notify_requester(status);
            continue;
        }

        // Comando desconhecido
        notify_requester(ESP_ERR_INVALID_ARG);
    }
}

// Ponto de entrada para tarefas que precisam carregar config pela primeira vez.
// Usado no boot para montar SD e aplicar config antes de outras tasks arrancarem.
esp_err_t task_config_request_initial_load(TickType_t wait_ticks)
{
    return issue_command(CONFIG_TASK_NOTIFY_INIT_BIT, wait_ticks);
}

// Solicita à task_config_loader que volte a ler o SD e atualize o período.
// Chamado quando o utilizador altera valores via portal ou HTTPS.
esp_err_t task_config_request_reload(TickType_t wait_ticks)
{
    return issue_command(CONFIG_TASK_NOTIFY_RELOAD_BIT, wait_ticks);
}

// Encapsula o envio de notificações e aguarda resposta da task loader.
// Utilizado pelas APIs públicas para evitar duplicar lógica de espera.
static esp_err_t issue_command(uint32_t command_bit, TickType_t wait_ticks)
{
    if (task_config_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t requester = xTaskGetCurrentTaskHandle();

    // Garante exclusividade para que apenas uma tarefa aguarde o resultado de cada pedido.
    taskENTER_CRITICAL(&s_request_lock);
    if (s_waiting_task != NULL) {
        taskEXIT_CRITICAL(&s_request_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_waiting_task = requester;
    taskEXIT_CRITICAL(&s_request_lock);

    if (xTaskNotify(task_config_handle, command_bit, eSetBits) != pdPASS) {
        taskENTER_CRITICAL(&s_request_lock);
        s_waiting_task = NULL;
        taskEXIT_CRITICAL(&s_request_lock);
        return ESP_FAIL;
    }

    // Fica à espera de o loader responder com o status em bruto via notificação.
    uint32_t raw_status = ESP_FAIL;
    BaseType_t wait_result = xTaskNotifyWait(0, UINT32_MAX, &raw_status, wait_ticks);

    taskENTER_CRITICAL(&s_request_lock);
    if (s_waiting_task == requester) {
        s_waiting_task = NULL;
    }
    taskEXIT_CRITICAL(&s_request_lock);

    if (wait_result != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return (esp_err_t)raw_status;
}

// Envia o resultado do comando para quem o solicitou e liberta o lock.
// Chamado pela task loader após processar init/reload ou erros.
static void notify_requester(esp_err_t status)
{
    taskENTER_CRITICAL(&s_request_lock);
    TaskHandle_t target = s_waiting_task;
    s_waiting_task = NULL;
    taskEXIT_CRITICAL(&s_request_lock);

    if (target != NULL) {
        xTaskNotify(target, (uint32_t)status, eSetValueWithOverwrite);
    } else {
        log_warn(TAG, "Sem tarefa à espera do resultado (status=%d).", (int)status);
    }
}
