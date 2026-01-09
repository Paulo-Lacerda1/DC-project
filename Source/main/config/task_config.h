#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

extern TaskHandle_t task_config_handle;

void task_config_loader(void *pvParameters);
esp_err_t task_config_request_initial_load(TickType_t wait_ticks);
esp_err_t task_config_request_reload(TickType_t wait_ticks);
