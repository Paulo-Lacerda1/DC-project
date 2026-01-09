// Guarda estados de runtime que dependem do utilizador (ex. logging SD ativo).
// Faz parte do módulo de configuração dinâmica usado pelas tasks HTTP e sensor.
#include "config_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_runtime_mutex = NULL;

bool g_enable_sd_logging = true;                // Flag global protegida pelo mutex.

// Converte o tempo de espera padrão em ticks para os semáforos.
// Usado por todas as operações para evitar bloqueios indefinidos.
static inline TickType_t mutex_wait_ticks(void)
{
    return pdMS_TO_TICKS(50);
}

// Cria o mutex usado para proteger acesso aos parâmetros dinâmicos.
// Chamado no arranque e lazy-initializado pelas funções de getter/setter.
void config_runtime_init(void)
{
    if (!s_runtime_mutex) {
        s_runtime_mutex = xSemaphoreCreateMutex();
    }
}

// Devolve o estado atual do toggle de logging em SD de forma thread-safe.
// Chamado pelo servidor HTTPS e pela task_sensor antes de escrever ficheiros.
bool config_runtime_get_sd_logging_enabled(void)
{
    if (!s_runtime_mutex) {
        config_runtime_init();
    }

    if (!s_runtime_mutex) {
        return g_enable_sd_logging;
    }

    bool enabled = false;
    if (xSemaphoreTake(s_runtime_mutex, mutex_wait_ticks()) == pdTRUE) {
        enabled = g_enable_sd_logging;
        xSemaphoreGive(s_runtime_mutex);
    }
    return enabled;
}

// Atualiza o toggle de logging SD e propaga para quem consulta o runtime.
// Invocado pelo portal HTTPS quando o utilizador altera a opção.
void config_runtime_set_sd_logging_enabled(bool enabled)
{
    if (!s_runtime_mutex) {
        config_runtime_init();
    }

    if (!s_runtime_mutex) {
        g_enable_sd_logging = enabled;
        return;
    }

    if (xSemaphoreTake(s_runtime_mutex, mutex_wait_ticks()) == pdTRUE) {
        g_enable_sd_logging = enabled;
        xSemaphoreGive(s_runtime_mutex);
    }
}
