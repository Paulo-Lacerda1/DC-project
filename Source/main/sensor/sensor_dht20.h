#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    float temperature;
    float humidity;
} sensor_data_t;

esp_err_t dht20_init(i2c_master_bus_handle_t *bus_out, i2c_master_dev_handle_t *dev_out);
esp_err_t dht20_read(float *temperature, float *humidity, i2c_master_dev_handle_t dev);
