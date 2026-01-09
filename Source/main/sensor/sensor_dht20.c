// Driver de alto nível para inicializar e ler o sensor DHT20 via I2C.
// Integra o módulo sensor responsável por obter temperatura/humidade.
#include "sensor_dht20.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logging.h"
#include "board_pins.h"

#define TAG "DHT20"
#define DHT20_ADDR 0x38
#define I2C_FREQ_HZ 100000

// Configura o bus I2C e regista o dispositivo DHT20 junto ao driver mestre.
// Chamado pela task_sensor no arranque para obter handles reutilizáveis.
esp_err_t dht20_init(i2c_master_bus_handle_t *bus_out, i2c_master_dev_handle_t *dev_out)
{
    // Valida os parâmetros de saída
    if (bus_out == NULL || dev_out == NULL) {
        log_error(TAG, "Parâmetros inválidos para inicializar o DHT20");
        return ESP_ERR_INVALID_ARG;
    }

    // Configura o bus I2C com os pinos e parâmetros definidos
    i2c_master_bus_config_t buscfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,  // Ignora até 7 glitches no bus
    };
    
    // Cria o bus I2C mestre
    esp_err_t err = i2c_new_master_bus(&buscfg, bus_out);
    if (err != ESP_OK) {
        log_error(TAG, "Falha ao criar o bus I2C (%s)", esp_err_to_name(err));
        return err;
    }

    // Configura o dispositivo DHT20 no bus
    i2c_device_config_t devcfg = {
        .device_address = DHT20_ADDR,      // Endereço I2C do DHT20 (0x38)
        .scl_speed_hz = I2C_FREQ_HZ,       // Velocidade do clock (100kHz)
    };
    
    // Adiciona o dispositivo DHT20 ao bus I2C
    err = i2c_master_bus_add_device(*bus_out, &devcfg, dev_out);
    if (err != ESP_OK) {
        log_error(TAG, "Falha ao adicionar o dispositivo DHT20 (%s)", esp_err_to_name(err));
        // Limpa o bus se falhou adicionar o dispositivo
        i2c_del_master_bus(*bus_out);
        *bus_out = NULL;
        return err;
    }

    // Aguarda 100ms para estabilização do sensor
    vTaskDelay(pdMS_TO_TICKS(100));
    log_info(TAG, "DHT20 inicializado com sucesso.");
    return ESP_OK;
}

// Dispara a medição, lê os 7 bytes do DHT20 e converte para unidades físicas.
// Chamado a cada ciclo de leitura pela task_sensor.
esp_err_t dht20_read(float *temperature, float *humidity, i2c_master_dev_handle_t dev)
{
    // Prepara comando de trigger para medição (0xAC 0x33 0x00)
    uint8_t cmd[3] = {0xAC, 0x33, 0x00};
    
    // Envia comando ao DHT20 via I2C
    esp_err_t err = i2c_master_transmit(dev, cmd, 3, 100 / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        log_error(TAG, "Falha I2C ao enviar comando para o DHT20 (%s)", esp_err_to_name(err));
        return err;
    }
    
    // Aguarda 80ms para o sensor completar a medição
    vTaskDelay(pdMS_TO_TICKS(80));

    // Buffer para receber os 7 bytes de dados do sensor
    uint8_t data[7];
    
    // Lê os dados do DHT20
    err = i2c_master_receive(dev, data, 7, 100 / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        log_error(TAG, "Falha I2C ao receber dados do DHT20 (%s)", esp_err_to_name(err));
        return err;
    }

    // Verifica bit de ocupado no byte de status (bit 7)
    if (data[0] & 0x80) {
        log_error(TAG, "DHT20 ocupado ou sem dados válidos (status 0x%02X)", data[0]);
        return ESP_ERR_INVALID_STATE;
    }

    // Extrai humidade dos bytes 1-3 (20 bits, shift right 4)
    uint32_t rawHum = ((data[1] << 16) | (data[2] << 8) | data[3]) >> 4;
    
    // Extrai temperatura dos bytes 3-5 (20 bits, 4 bits baixos de data[3])
    uint32_t rawTemp = ((data[3] & 0x0F) << 16) | (data[4] << 8) | data[5];

    // Converte humidade raw para percentagem (0-100%)
    *humidity = (rawHum * 100.0) / 1048576.0;
    
    // Converte temperatura raw para Celsius (-50°C a +150°C)
    *temperature = (rawTemp * 200.0 / 1048576.0) - 50.0;

    // Valida se os valores estão dentro dos limites esperados do sensor
    if (!(*humidity >= 0.0f && *humidity <= 100.0f) ||  
        !(*temperature >= -40.0f && *temperature <= 125.0f)) {
        log_warn(TAG, "Valores do DHT20 fora do intervalo: T=%.2f H=%.2f", *temperature, *humidity);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
