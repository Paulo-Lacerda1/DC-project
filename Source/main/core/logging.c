// Implementa o subsistema de logging persistente no cartão SD e no display.
// Faz parte do core, oferecendo APIs log_info/warn/error thread-safe.
#include "logging.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config_manager.h"
#include "led.h"
#include "security_hmac.h"
#include "system_status.h"
#include "display.h"
#include "task_mqtt.h"

#define LOG_MOUNT_POINT             "/sdcard"
#define LOG_FILE_PATH               LOG_MOUNT_POINT "/system.log"
#define LOG_MAX_CONSECUTIVE_ERRORS  3
#define LOG_STACK_BUFFER_SIZE       256U
#define LOG_MUTEX_WAIT_MS           200U
#define LOG_EVENT_QUEUE_LENGTH      16U

typedef enum {
    LOG_LEVEL_INFO = 0,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} log_level_t;

static SemaphoreHandle_t s_log_mutex = NULL;
static bool s_log_ready = false;
static FILE *s_log_file = NULL;
static bool s_truncate_on_open = true;
static uint32_t s_log_error_streak = 0;
static bool s_log_fatal_shown = false;
static QueueHandle_t s_event_queue = NULL;
static const char *TAG = "system_log";

static esp_err_t log_open_file_locked(void);
static void log_append_locked(log_level_t level, const char *tag, const char *message);
static esp_err_t log_write_entry(const char *timestamp, const char *level_str, const char *tag, const char *message, size_t *written_bytes);
static void log_handle_write_error_locked(const char *context);
static void log_write_common(log_level_t level, const char *tag, const char *fmt, va_list args);
static const char *log_level_to_string(log_level_t level);
static void log_build_timestamp(char *buffer, size_t buffer_size);
static void log_show_fatal_banner(void);
static int log_noop_vprintf(const char *fmt, va_list args);
static void log_enqueue_event(log_level_t level, const char *tag, const char *message);
static bool log_ensure_event_queue(void);

// Converte o tempo máximo de espera do mutex do logger para ticks.
// Usado em todas as operações protegidas contra concorrência.
static inline TickType_t mutex_wait_ticks(void)
{
    return pdMS_TO_TICKS(LOG_MUTEX_WAIT_MS);
}

// Prepara o mutex, monta o SD e abre system.log em modo seguro.
// Chamado no arranque para permitir logging persistente.
esp_err_t log_init(void)
{
    // Cria o mutex se ainda não existir
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
        if (!s_log_mutex) {
            ESP_LOGE(TAG, "Não foi possível criar mutex do logger.");
            return ESP_ERR_NO_MEM;
        }
    }

    // Tenta adquirir o mutex com espera infinita
    if (xSemaphoreTake(s_log_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Mutex do logger indisponível.");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    // Verifica se o logger ainda não está pronto
    if (!s_log_ready || !s_log_file) {
        // Tenta montar o cartão SD
        err = config_manager_mount_sd();
        if (err == ESP_OK) {
            // Se o SD foi montado, abre o ficheiro de log
            err = log_open_file_locked();
        }

        // Atualiza os estados com base no resultado
        if (err == ESP_OK) {
            s_log_ready = true;
            s_log_fatal_shown = false;
        } else {
            s_log_ready = false;
        }
    }

    // Liberta o mutex
    xSemaphoreGive(s_log_mutex);

    // Regista mensagem de sucesso ou erro
    if (err == ESP_OK) {
        log_info(TAG, "Logging inicializado em %s (sem limite configurado).", LOG_FILE_PATH);
    } else {
        ESP_LOGE(TAG, "Não foi possível iniciar logging (%s).", esp_err_to_name(err));
    }

    return err;
}

// Formata e escreve mensagem de nível INFO.
// API pública usada por todos os módulos do firmware.
void log_info(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write_common(LOG_LEVEL_INFO, tag, fmt, args);
    va_end(args);
}

// Formata e escreve mensagem de nível WARN.
// API pública para alertas não críticos registados no SD.
void log_warn(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write_common(LOG_LEVEL_WARN, tag, fmt, args);
    va_end(args);
}

// Formata e escreve mensagem de nível ERROR.
// Promove mensagens críticas tanto para SD como para display.
void log_error(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write_common(LOG_LEVEL_ERROR, tag, fmt, args);
    va_end(args);
}

// Caminho comum que trata da formatação, envio para display e ficheiro.
// Invocado por log_info/warn/error para partilhar a lógica.
static void log_write_common(log_level_t level, const char *tag, const char *fmt, va_list args)
{
    // Valida a string de formato
    if (!fmt) {
        return;
    }

    // Usa buffer na stack por defeito para eficiência
    char stack_buf[LOG_STACK_BUFFER_SIZE];
    char *message = stack_buf;
    size_t buffer_size = sizeof(stack_buf);
    bool heap_buffer = false;

    // Calcula o tamanho necessário para a mensagem formatada
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) {
        ESP_LOGE(TAG, "Erro a formatar mensagem de log.");
        return;
    }

    // Se precisar de mais espaço, aloca buffer no heap
    size_t required = (size_t)needed + 1U;
    if (required > buffer_size) {
        char *tmp = (char *)malloc(required);
        if (tmp) {
            message = tmp;
            buffer_size = required;
            heap_buffer = true;
        }
    }

    // Formata a mensagem para o buffer
    va_list args_format;
    va_copy(args_format, args);
    vsnprintf(message, buffer_size, fmt, args_format);
    va_end(args_format);

    // Prepara linha formatada para o display (apenas mensagem, sem tag)
    char display_line[DISPLAY_LOG_LINE_MAX_LEN];
    const size_t max_len = sizeof(display_line) - 1;
    size_t msg_len = strlen(message);
    if (msg_len > max_len) {
        msg_len = max_len;
    }

    snprintf(display_line, sizeof(display_line), "%.*s", (int)msg_len, message);

    // Envia para o display
    display_add_log(display_line);

    // Tenta escrever no ficheiro de log do SD se disponível
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, mutex_wait_ticks()) == pdTRUE) {
        if (s_log_ready && s_log_file) {
            log_append_locked(level, tag, message);
        }
        xSemaphoreGive(s_log_mutex);
    }

    log_enqueue_event(level, tag, message);

    // Liberta memória se foi alocada no heap
    if (heap_buffer) {
        free(message);
    }
}

// Escreve a linha formatada no ficheiro (com tolerância a falhas consecutivas).
// Executado com mutex já adquirido para serializar acessos ao SD.
static void log_append_locked(log_level_t level, const char *tag, const char *message)
{
    if (!message || !s_log_file) {
        return;
    }

    char timestamp[32];
    log_build_timestamp(timestamp, sizeof(timestamp));

    const char *level_str = log_level_to_string(level);
    size_t written = 0;
    esp_err_t err = log_write_entry(timestamp, level_str, tag, message, &written);
    if (err != ESP_OK) {
        log_handle_write_error_locked("escrever no ficheiro");
        return;
    }

    system_status_note_log_write();
}

// Converte digest binário SHA-256 em hex minúsculo (para HMAC do log).
// Usado ao assinar cada linha antes de escrever no cartão SD.
static void digest_to_hex(const uint8_t digest[32], char output[65])
{
    for (size_t i = 0; i < 32; ++i) {
        snprintf(&output[i * 2], 3, "%02x", digest[i]);
    }
    output[64] = '\0';
}

// Monta a linha completa do log, calcula o HMAC e escreve no ficheiro.
// Executado com o mutex detido para garantir consistência no SD.
static esp_err_t log_write_entry(const char *timestamp, const char *level_str, const char *tag, const char *message, size_t *written_bytes)
{
    // Valida os parâmetros de entrada
    if (!timestamp || !level_str || !message || !s_log_file) {
        return ESP_ERR_INVALID_ARG;
    }

    // Inicializa contador de bytes escritos
    if (written_bytes) {
        *written_bytes = 0;
    }

    // Usa "-" se a tag estiver vazia
    const char *tag_str = (tag && *tag) ? tag : "-";

    // Calcula o tamanho necessário para a linha base (sem HMAC)
    int base_needed = snprintf(NULL, 0, "%s | %s | %s | %s", timestamp, level_str, tag_str, message);
    if (base_needed < 0) {
        return ESP_FAIL;
    }

    // Aloca buffer para a linha base (stack ou heap)
    size_t base_required = (size_t)base_needed + 1U;
    char base_stack[LOG_STACK_BUFFER_SIZE];
    char *base_line = base_stack;
    bool base_heap = false;

    if (base_required > sizeof(base_stack)) {
        base_line = (char *)malloc(base_required);
        if (!base_line) {
            return ESP_ERR_NO_MEM;
        }
        base_heap = true;
    }

    // Formata a linha base
    snprintf(base_line, base_required, "%s | %s | %s | %s", timestamp, level_str, tag_str, message);
    size_t base_len = strnlen(base_line, base_required - 1);

    // Calcula o HMAC-SHA256 da linha base
    uint8_t digest[32];
    esp_err_t hmac_err = security_hmac_compute((const uint8_t *)base_line, base_len, digest);
    if (hmac_err != ESP_OK) {
        ESP_LOGW(TAG, "Falha a calcular HMAC do log (%s). Entrada ignorada.", esp_err_to_name(hmac_err));
        if (base_heap) {
            free(base_line);
        }
        return hmac_err;
    }

    // Converte o digest binário para hexadecimal
    char hex_digest[65];
    digest_to_hex(digest, hex_digest);

    // Calcula o tamanho necessário para a linha final (com HMAC)
    int final_needed = snprintf(NULL, 0, "%s | HMAC=%s\n", base_line, hex_digest);
    if (final_needed < 0) {
        if (base_heap) {
            free(base_line);
        }
        return ESP_FAIL;
    }

    // Aloca buffer para a linha final (stack ou heap)
    size_t final_required = (size_t)final_needed + 1U;
    char final_stack[LOG_STACK_BUFFER_SIZE];
    char *final_line = final_stack;
    bool final_heap = false;

    if (final_required > sizeof(final_stack)) {
        final_line = (char *)malloc(final_required);
        if (!final_line) {
            if (base_heap) {
                free(base_line);
            }
            return ESP_ERR_NO_MEM;
        }
        final_heap = true;
    }

    // Formata a linha final com o HMAC
    snprintf(final_line, final_required, "%s | HMAC=%s\n", base_line, hex_digest);
    size_t final_len = strnlen(final_line, final_required - 1);

    // Escreve a linha no ficheiro
    size_t written = fwrite(final_line, 1, final_len, s_log_file);
    // Força flush para o SD
    int flush_status = fflush(s_log_file);
    // Sincroniza com o disco
    int sync_status = fsync(fileno(s_log_file));
    // Determina se a escrita foi bem-sucedida
    esp_err_t write_status = (written == final_len && flush_status == 0 && sync_status == 0)
                                 ? ESP_OK
                                 : ESP_FAIL;

    // Atualiza contador de bytes escritos
    if (written_bytes) {
        *written_bytes = (write_status == ESP_OK) ? written : 0;
    }

    // Liberta buffers se foram alocados no heap
    if (base_heap) {
        free(base_line);
    }
    if (final_heap) {
        free(final_line);
    }

    return write_status;
}

static esp_err_t log_open_file_locked(void)
{
    if (s_log_file) {
        return ESP_OK;
    }

    const char *mode = s_truncate_on_open ? "w" : "a";
    s_log_file = fopen(LOG_FILE_PATH, mode);
    if (!s_log_file) {
        ESP_LOGE(TAG, "Falha ao abrir %s (errno=%d).", LOG_FILE_PATH, errno);
        return ESP_FAIL;
    }

    if (s_truncate_on_open) {
        s_truncate_on_open = false;
    }

    return ESP_OK;
}

static void log_handle_write_error_locked(const char *context)
{
    ESP_LOGE(TAG, "Falha ao %s no log (SD indisponível?). Logging desativado.", context ? context : "aceder");
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_log_ready = false;
    s_log_error_streak = 0;
    log_show_fatal_banner();
    led_set_state_with_reason(LED_STATE_ERROR, "falhas consecutivas no logger");
    mark_sd_card_unavailable("falhas consecutivas a escrever system.log");
    esp_log_set_vprintf(log_noop_vprintf);  // descarta logs futuros imediatamente
    esp_log_level_set("*", ESP_LOG_NONE);   // silencia logs futuros para que o banner seja o último
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static const char *log_level_to_string(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_INFO:
        return "INFO";
    case LOG_LEVEL_WARN:
        return "WARN";
    case LOG_LEVEL_ERROR:
    default:
        return "ERROR";
    }
}

static void log_build_timestamp(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }

    const time_t now = time(NULL);
    struct tm tm_info;
    if (now >= 1577836800 && localtime_r(&now, &tm_info)) {
        strftime(buffer, buffer_size, "%H:%M:%S", &tm_info);
        return;
    }

    int64_t micros = esp_timer_get_time();
    int64_t total_ms = micros / 1000;
    int64_t total_sec = total_ms / 1000;
    int64_t sec = total_sec % 60;
    int64_t total_min = total_sec / 60;
    int64_t min = total_min % 60;
    int64_t hours = total_min / 60;

    snprintf(buffer, buffer_size, "T+%03lld:%02lld:%02lld",
             (long long)hours,
             (long long)min,
             (long long)sec);
}

// Implementação neutra usada para silenciar logs do IDF numa crise.
// Registada temporariamente quando não queremos repetir mensagens.
static int log_noop_vprintf(const char *fmt, va_list args)
{
    (void)fmt;
    (void)args;
    return 0;
}

// Mostra mensagem no console/display quando o logging falha permanentemente.
// Chamado uma única vez quando o erro atinge o limite configurado.
static void log_show_fatal_banner(void)
{
    if (s_log_fatal_shown) {
        return;
    }
    s_log_fatal_shown = true;
    printf("\033[31mCartaoSD removido, a encerrar o sistema.\033[0m\n");
}

bool log_dequeue_event(log_event_t *event_out)
{
    if (!event_out || !s_event_queue) {
        return false;
    }
    return xQueueReceive(s_event_queue, event_out, 0) == pdTRUE;
}

static bool log_ensure_event_queue(void)
{
    if (s_event_queue) {
        return true;
    }
    s_event_queue = xQueueCreate(LOG_EVENT_QUEUE_LENGTH, sizeof(log_event_t));
    return s_event_queue != NULL;
}

static void log_enqueue_event(log_level_t level, const char *tag, const char *message)
{
    if (!message) {
        return;
    }

    if (!log_ensure_event_queue()) {
        return;
    }

    log_event_t event = {0};
    log_build_timestamp(event.timestamp, sizeof(event.timestamp));
    strlcpy(event.level, log_level_to_string(level), sizeof(event.level));
    const char *module = (tag && *tag) ? tag : "-";
    strlcpy(event.module, module, sizeof(event.module));
    strlcpy(event.message, message, sizeof(event.message));

    if (xQueueSendToBack(s_event_queue, &event, 0) != pdTRUE) {
        return;
    }

    if (task_mqtt_handle != NULL) {
        xTaskNotify(task_mqtt_handle, TASK_MQTT_NOTIFY_EVENT_BIT, eSetBits);
    }
}
