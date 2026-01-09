// Mantém um snapshot global do estado do sistema (Wi-Fi, MQTT, SD, standby).
// Serve o módulo core e os servidores web com informações sincronizadas.
#include "system_status.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#define MUTEX_WAIT_MS      50

static system_status_snapshot_t s_state = {
    .wifi = {
        .sta_connected = false,
        .ip_addr = "desconhecido",
        .rssi = 0,
    },
    .mqtt = {
        .connected = false,
        .last_attempt_ts = 0,
        .fail_count = 0,
        .messages_sent = 0,
        .messages_buffered = 0,
        .broker_host = "desconhecido",
    },
    .sd = {
        .mounted = false,
        .free_space_bytes = 0,
        .log_entries = 0,
        .last_log_ts = 0,
        .data_entries = 0,
        .data_last_log_ts = 0,
        .data_logging_enabled = false,
    },
    .general = {
        .uptime_seconds = 0,
        .firmware_version = "desconhecido",
        .device_uid = "desconhecido",
    },
    .standby_active = false,
};

static SemaphoreHandle_t s_mutex = NULL;

// Converte o tempo de espera padrão do mutex para ticks.
// Usado por todas as operações para evitar bloqueio infinito.
static inline TickType_t mutex_wait_ticks(void)
{
    return pdMS_TO_TICKS(MUTEX_WAIT_MS);
}

// Converte o esp_timer em segundos desde boot para estatísticas.
// Usado para timestamps e uptime no snapshot.
static uint64_t now_seconds(void)
{
    // Obtém o tempo em microssegundos desde o boot e converte para segundos
    return (uint64_t)(esp_timer_get_time() / 1000000ULL);
}

// Cria o mutex do estado global se ainda não existir.
// Chamado no arranque antes de qualquer acesso ao snapshot.
void system_status_init(void)
{
    // Cria o mutex apenas se ainda não foi criado
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

// Atualiza versão e UID expostos no dashboard.
// Invocado após ler a descrição do firmware e o manufacturer UID.
void system_status_set_firmware_info(const char *version, const char *device_uid)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza a versão do firmware se fornecida
        if (version) {
            strlcpy(s_state.general.firmware_version, version, sizeof(s_state.general.firmware_version));
        }
        // Atualiza o UID do dispositivo se fornecido
        if (device_uid) {
            strlcpy(s_state.general.device_uid, device_uid, sizeof(s_state.general.device_uid));
        }
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Marca o estado de ligação STA e limpa IP/RSSI quando desconecta.
// Chamado a partir dos handlers Wi-Fi.
void system_status_set_wifi_connected(bool connected)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza o estado de ligação
        s_state.wifi.sta_connected = connected;
        // Se desconectou, limpa informações de IP e RSSI
        if (!connected) {
            strlcpy(s_state.wifi.ip_addr, "desconhecido", sizeof(s_state.wifi.ip_addr));
            s_state.wifi.rssi = 0;
        }
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Regista o IP atual da STA para exibição na UI.
// Usado quando IP_EVENT_STA_GOT_IP é emitido.
void system_status_set_wifi_ip(const char *ip_addr)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    // Valida parâmetros
    if (!s_mutex || !ip_addr) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Copia o endereço IP para a estrutura de estado
        strlcpy(s_state.wifi.ip_addr, ip_addr, sizeof(s_state.wifi.ip_addr));
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Grava o RSSI atual reportado pelo driver.
// Chamado periodicamente pelo config_manager após ligação.
void system_status_set_wifi_rssi(int rssi_dbm)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza o valor de RSSI (força do sinal em dBm)
        s_state.wifi.rssi = rssi_dbm;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Guarda timestamp da última tentativa de ligação MQTT.
// Usado para o diagnóstico no dashboard.
void system_status_mark_mqtt_attempt(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Regista o timestamp atual da tentativa de ligação
        s_state.mqtt.last_attempt_ts = now_seconds();
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Marca se a ligação MQTT está ativa.
// Atualizado pelo event handler da task_mqtt.
void system_status_set_mqtt_connected(bool connected)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza o estado de ligação MQTT
        s_state.mqtt.connected = connected;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Leitura thread-safe para saber se o MQTT está ligado.
// Usado por outras tasks como sensor para decidir o LED.
bool system_status_is_mqtt_connected(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return false;
    }

    // Variável para armazenar o estado
    bool connected = false;
    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Lê o estado de ligação
        connected = s_state.mqtt.connected;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
    return connected;
}

// Incrementa o contador de falhas MQTT.
// Chamado sempre que uma tentativa de reconexão falha.
void system_status_increment_mqtt_fail(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Incrementa o contador de falhas
        s_state.mqtt.fail_count++;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Incrementa total de mensagens publicadas com sucesso.
// Atualizado quando recebemos confirmação de publish.
void system_status_increment_mqtt_sent(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Incrementa o contador de mensagens enviadas
        s_state.mqtt.messages_sent++;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Atualiza o número de mensagens pendentes em buffer.
// Usado quando o stack MQTT reporta backlog.
void system_status_set_mqtt_buffered(uint32_t buffered)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza o número de mensagens em buffer
        s_state.mqtt.messages_buffered = buffered;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Guarda o IP/host do broker MQTT lido do config_user.json no snapshot.
// Usado para mostrar essa informação no dashboard HTTPS.
void system_status_set_mqtt_broker_host(const char *host)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        const char *value = (host && host[0]) ? host : "desconhecido";
        strlcpy(s_state.mqtt.broker_host, value, sizeof(s_state.mqtt.broker_host));
        xSemaphoreGive(s_mutex);
    }
}

// Atualiza estado do SD (montado, espaço livre, contadores).
// Chamado pelo config_manager quando monta/desmonta ou verifica saúde.
void system_status_set_sd_state(bool mounted, uint64_t free_space_bytes)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza o estado de montagem do SD
        s_state.sd.mounted = mounted;
        // Atualiza o espaço livre em bytes
        s_state.sd.free_space_bytes = free_space_bytes;
        // Se o SD não está montado, limpa todos os contadores
        if (!mounted) {
            s_state.sd.log_entries = 0;
            s_state.sd.last_log_ts = 0;
            s_state.sd.data_entries = 0;
            s_state.sd.data_last_log_ts = 0;
            s_state.sd.data_logging_enabled = false;
        }
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Incrementa contador e timestamp da última escrita no system.log.
// Invocado sempre que o logger consegue escrever no SD.
void system_status_note_log_write(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Incrementa o contador de entradas de log
        s_state.sd.log_entries++;
        // Atualiza o timestamp da última escrita
        s_state.sd.last_log_ts = now_seconds();
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Regista estatísticas de escrita no ficheiro data.txt.
// Chamado pela task_sensor quando guarda uma leitura no SD.
void system_status_note_data_log_write(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Incrementa o contador de entradas de dados
        s_state.sd.data_entries++;
        // Atualiza o timestamp da última escrita de dados
        s_state.sd.data_last_log_ts = now_seconds();
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Limpa contadores associados ao ficheiro de dados.
// Usado quando o SD foi reinserido ou o arquivo é recriado.
void system_status_reset_data_log_stats(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Reseta o contador de entradas de dados
        s_state.sd.data_entries = 0;
        // Limpa o timestamp da última escrita
        s_state.sd.data_last_log_ts = 0;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Guarda o estado do toggle de logging configurado via HTTPS.
// Permite que o dashboard mostre se a gravação está ativa.
void system_status_set_data_logging_enabled(bool enabled)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }

    // Tenta adquirir o mutex
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza o estado do toggle de logging
        s_state.sd.data_logging_enabled = enabled;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Indica se o sistema está em standby manual imposto pelo botão.
// Usado pela task_sensor quando entra ou sai do modo pausa.
void system_status_set_standby_active(bool active)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return;
    }
    // Tenta adquirir o mutex para atualizar o estado de standby
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Atualiza o flag de standby ativo
        s_state.standby_active = active;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}

// Consulta protegida para saber se estamos em standby.
// Usada por display e outras tasks para ajustar comportamento.
bool system_status_is_standby_active(void)
{
    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }
    if (!s_mutex) {
        return false;
    }

    // Variável para armazenar o estado
    bool active = false;
    // Tenta adquirir o mutex para ler o estado de standby
    if (xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Copia o valor atual do estado de standby
        active = s_state.standby_active;
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
    return active;
}

// Copia todo o estado para o buffer fornecido e atualiza o uptime.
// Função base usada pelos servidores web e pelo display.
void system_status_snapshot(system_status_snapshot_t *out)
{
    // Valida o ponteiro de saída
    if (!out) {
        return;
    }

    // Garante que o mutex foi inicializado
    if (!s_mutex) {
        system_status_init();
    }

    // Tenta adquirir o mutex
    if (s_mutex && xSemaphoreTake(s_mutex, mutex_wait_ticks()) == pdTRUE) {
        // Copia toda a estrutura de estado para o buffer de saída
        *out = s_state;
        // Atualiza o campo de uptime com o valor atual
        out->general.uptime_seconds = now_seconds();
        // Liberta o mutex
        xSemaphoreGive(s_mutex);
    }
}
