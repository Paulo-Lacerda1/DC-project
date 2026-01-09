#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Inicializa o módulo de logging, preparando mutex, SD e ficheiro system.log.
 *
 * @return ESP_OK em caso de sucesso ou um código de erro do ESP-IDF.
 */
esp_err_t log_init(void);

void log_info(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_warn(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_error(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_EVENT_TIMESTAMP_MAX_LEN  32U
#define LOG_EVENT_LEVEL_MAX_LEN      8U
#define LOG_EVENT_MODULE_MAX_LEN     32U
#define LOG_EVENT_MESSAGE_MAX_LEN    160U

typedef struct {
    char timestamp[LOG_EVENT_TIMESTAMP_MAX_LEN];
    char level[LOG_EVENT_LEVEL_MAX_LEN];
    char module[LOG_EVENT_MODULE_MAX_LEN];
    char message[LOG_EVENT_MESSAGE_MAX_LEN];
} log_event_t;

bool log_dequeue_event(log_event_t *event_out);
