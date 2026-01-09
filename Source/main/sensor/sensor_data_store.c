// Guarda as últimas leituras do sensor DHT20 com proteção por mutex.
// Módulo partilhado entre as tasks de sensor, MQTT e display.
#include "sensor_data_store.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sensor_data_store";

static sensor_data_t s_latest = {0};
static bool s_has_valid_data = false;
static SemaphoreHandle_t s_data_mutex = NULL;
static uint64_t s_last_timestamp_ms = 0;

// Cria o mutex interno e prepara o armazenamento da última leitura.
// Chamado no arranque antes de qualquer leitura do sensor.
void sensor_data_store_init(void)
{
    // Verifica se o mutex já foi criado anteriormente
    if (s_data_mutex) {
        return;
    }

    // Cria um mutex FreeRTOS para proteger o acesso aos dados do sensor
    s_data_mutex = xSemaphoreCreateMutex();
    if (!s_data_mutex) {
        // Regista erro se a criação do mutex falhar
        ESP_LOGE(TAG, "Falha ao criar mutex para partilhar os dados do sensor.");
    }
}

// Atualiza a última leitura válida e o timestamp associado.
// Invocado pela task_sensor imediatamente após ler o DHT20.
void sensor_data_store_set(float temperature, float humidity)
{
    // Garante que o mutex foi inicializado
    if (!s_data_mutex) {
        sensor_data_store_init();
    }

    // Verifica novamente após a tentativa de inicialização
    if (!s_data_mutex) {
        return;
    }

    // Tenta adquirir o mutex com timeout de 50ms
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Atualiza os valores de temperatura e humidade
        s_latest.temperature = temperature;
        s_latest.humidity = humidity;
        
        // Marca os dados como válidos
        s_has_valid_data = true;
        
        // Guarda o timestamp atual em milissegundos
        s_last_timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        
        // Liberta o mutex
        xSemaphoreGive(s_data_mutex);
    } else {
        // Avisa se não conseguiu obter o mutex
        ESP_LOGW(TAG, "Não foi possível bloquear mutex para atualizar leitura.");
    }
}

// Marca os dados como inválidos (p.ex. quando ocorre erro ou SD ausente).
// Usado para prevenir que consumidores usem valores desatualizados.
void sensor_data_store_invalidate(void)
{
    // Garante que o mutex foi inicializado
    if (!s_data_mutex) {
        sensor_data_store_init();
    }

    // Verifica novamente após a tentativa de inicialização
    if (!s_data_mutex) {
        return;
    }

    // Tenta adquirir o mutex com timeout de 50ms
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Marca os dados como inválidos
        s_has_valid_data = false;
        
        // Limpa o timestamp
        s_last_timestamp_ms = 0;
        
        // Liberta o mutex
        xSemaphoreGive(s_data_mutex);
    }
}

// Copia a última temperatura/humidade para o formato simples sensor_data_t.
// Função thread-safe usada pelo servidor HTTPS.
bool sensor_data_store_get(sensor_data_t *out)
{
    // Valida o ponteiro de saída
    if (!out) {
        return false;
    }

    // Garante que o mutex foi inicializado
    if (!s_data_mutex) {
        sensor_data_store_init();
    }

    // Verifica novamente após a tentativa de inicialização
    if (!s_data_mutex) {
        return false;
    }

    // Variável para indicar se os dados são válidos
    bool valid = false;
    
    // Tenta adquirir o mutex com timeout de 50ms
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Copia a estrutura completa de dados
        *out = s_latest;
        
        // Obtém o estado de validade dos dados
        valid = s_has_valid_data;
        
        // Liberta o mutex
        xSemaphoreGive(s_data_mutex);
    }

    return valid;
}

// Fornece a última leitura detalhada, incluindo timestamp em ms.
// Chamado pela task_mqtt antes de publicar dados.
bool sensor_get_last_reading(sensor_reading_t *out)
{
    // Valida o ponteiro de saída
    if (!out) {
        return false;
    }

    // Garante que o mutex foi inicializado
    if (!s_data_mutex) {
        sensor_data_store_init();
    }

    // Verifica novamente após a tentativa de inicialização
    if (!s_data_mutex) {
        return false;
    }

    // Variável para indicar se os dados são válidos
    bool valid = false;
    
    // Tenta adquirir o mutex com timeout de 50ms
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Preenche a estrutura de leitura com temperatura
        out->temperatura_c = s_latest.temperature;
        
        // Preenche com humidade
        out->humidade_rh = s_latest.humidity;
        
        // Adiciona o timestamp da leitura
        out->timestamp_ms = s_last_timestamp_ms;
        
        // Obtém o estado de validade dos dados
        valid = s_has_valid_data;
        
        // Liberta o mutex
        xSemaphoreGive(s_data_mutex);
    }

    return valid;
}
