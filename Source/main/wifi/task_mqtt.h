#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_manager.h"

extern TaskHandle_t task_mqtt_handle;

#define TASK_MQTT_NOTIFY_DATA_BIT    (1U << 0)
#define TASK_MQTT_NOTIFY_EVENT_BIT   (1U << 1)

void task_mqtt_start(const config_user_t *cfg);
