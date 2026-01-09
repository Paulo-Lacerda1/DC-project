#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#define TASK_SENSOR_NOTIFY_PERIOD_UPDATE_BIT   (1UL << 2)

extern TaskHandle_t task_sensor_handle;

/* Entrada da tarefa que lê o DHT20. */
void task_sensor(void *pvParameters);

/* Acorda a tarefa para aplicar imediatamente um novo período. */
esp_err_t task_sensor_notify_period_update(void);
