// Centraliza IO de configuração no SD e sincroniza Wi-Fi, MQTT e período de leitura.
// Módulo core de configuração persistente e runtime do firmware.
#include "config_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "logging.h"
#include "mbedtls/base64.h"
#include "security_aes.h"
#include "system_status.h"

#define CONFIG_MOUNT_POINT                 "/sdcard"
#define CONFIG_USER_FILE_PATH              CONFIG_MOUNT_POINT "/config_user.json"
#define CONFIG_WIFI_FILE_PATH              CONFIG_MOUNT_POINT "/config.json"
#define CONFIG_MANUFACTURER_FILE_PATH      CONFIG_MOUNT_POINT "/config_manufacturer.json"
#define CONFIG_DEFAULT_PERIOD_MS           2000U
#define WIFI_JSON_SSID_KEY                 "wifi_ssid"
#define WIFI_JSON_PASS_KEY                 "wifi_pass"
#define WIFI_JSON_PASS_ENC_KEY             "wifi_pass_enc"
#define MQTT_PASSWORD_JSON_KEY             "mqtt_password"
#define MQTT_PASSWORD_JSON_ENC_KEY         "mqtt_password_enc"
#define MQTT_URI_JSON_KEY                  "mqtt_uri"
#define MQTT_URI_JSON_ENC_KEY              "mqtt_uri_enc"
#define WIFI_SSID_MAX_LEN                  CONFIG_MANAGER_WIFI_SSID_MAX_LEN
#define WIFI_PASS_MAX_LEN                  CONFIG_MANAGER_WIFI_PASS_MAX_LEN
#define WIFI_PASS_MIN_LEN                  CONFIG_MANAGER_WIFI_PASS_MIN_LEN
#define SOFTAP_DEFAULT_SSID                "ESP_Config"   // Estes sao os valores defaults, caso os da placa nao funcionarem
#define SOFTAP_DEFAULT_PASS                "dispositivos"
#define SOFTAP_DEFAULT_CHANNEL             6

static const char *TAG = "config_manager";
static const char *JSON_CONSOLE_TAG = "JSON";
static const char *WIFI_CONSOLE_TAG = "WiFi";

static portMUX_TYPE s_period_lock = portMUX_INITIALIZER_UNLOCKED;

// Valor ativo do período de leitura, partilhado com o resto do firmware.
uint32_t leitura_periodo_ms = CONFIG_DEFAULT_PERIOD_MS;

config_user_t g_config_user = {
    .periodo_leitura_ms = CONFIG_DEFAULT_PERIOD_MS,
    .wifi_ssid = {0},
    .wifi_pass = {0},
    .mqtt_uri = {0},
    .mqtt_topic = "data",
    .mqtt_topic_events = "eventos",
    .mqtt_client_id = "esp32_default",
    .mqtt_username = {0},
    .mqtt_password = {0},
};

// Flags que permitem evitar inicializações redundantes do barramento e do SD.
static bool s_spi_bus_ready = false;
static bool s_sd_mounted = false;
static sdmmc_card_t *s_card = NULL;
static bool s_sd_restart_on_insert = false;
static bool s_sd_mount_err_pending = false;
static esp_err_t s_sd_last_mount_err = ESP_OK;
static esp_netif_t *s_wifi_sta_netif = NULL;
static esp_netif_t *s_wifi_ap_netif = NULL;
static bool s_wifi_handlers_registered = false;
static char s_wifi_ssid[33] = {0};
static bool s_time_sync_started = false;
static bool s_time_synced = false;
static bool s_netif_logs_suppressed = false;

#define SOFTAP_MAX_CLIENTS               CONFIG_MANAGER_SOFTAP_MAX_CLIENTS

// Informação acompanhada para cada cliente ligado ao SoftAP.
typedef struct {
    bool valid;
    uint8_t mac[6];
    uint16_t aid;
    esp_ip4_addr_t ip;
} softap_client_entry_t;

static softap_client_entry_t s_softap_clients[SOFTAP_MAX_CLIENTS];

typedef esp_err_t (*config_create_fn_t)(void);
typedef esp_err_t (*config_read_fn_t)(void);
typedef void (*config_defaults_fn_t)(void);

// Atualiza o período em memória partilhada (RAM e g_config_user).
static void set_period_ms(uint32_t period_ms);
// Restaura config_user com valores conhecidos para arrancar sem SD.
static void apply_default_user_config(void);
// Garante que o barramento SPI usado pelo SD está pronto.
static esp_err_t ensure_spi_bus_initialized(void);
// Monta (se necessário) o cartão SD e atualiza os estados internos.
static esp_err_t ensure_sd_card_mounted(void);
void mark_sd_card_unavailable(const char *reason);
// Abstracção para criar/reler ficheiros JSON com defaults.
static esp_err_t ensure_json_ready(const char *path,
                                   const char *label,
                                   config_create_fn_t create_fn,
                                   config_read_fn_t read_fn,
                                   config_defaults_fn_t defaults_fn,
                                   config_create_fn_t rewrite_fn);
static esp_err_t read_user_config_wrapper(void);
// Carrega config_user.json e opcionalmente regrava caso falhe validação.
static esp_err_t read_user_config(bool rewrite_if_needed);
// Persiste config_user.json no SD com o período pedido.
static esp_err_t persist_user_config(uint32_t period_ms);
// Gera um ficheiro config_user.json novo com defaults seguros.
static esp_err_t create_default_user_config(void);
static esp_err_t read_entire_file(const char *path, char **buffer_out);
static esp_err_t write_text_file(const char *path, const char *text);
static esp_err_t base64_encode_alloc(const uint8_t *data, size_t data_len, char **out_b64);
static esp_err_t base64_decode_alloc(const char *b64, uint8_t **out_buf, size_t *out_len);
// Recupera uma string cifrada do JSON e converte-a para texto plano.
// Chamado ao carregar config_user.json para preencher g_config_user.
static esp_err_t decrypt_json_string(const cJSON *root,
                                     const char *enc_key,
                                     char *out,
                                     size_t out_size,
                                     size_t min_len,
                                     size_t max_len,
                                     const char *label);
// Cifra e grava uma string sensível em paralelo com a versão plaintext.
// Usado ao guardar credenciais Wi-Fi e MQTT no SD.
static esp_err_t encrypt_and_store_string(cJSON *root,
                                          const char *plain_key,
                                          const char *enc_key,
                                          const char *value);
static esp_err_t store_encrypted_fields(cJSON *root);
static void ensure_plain_string(cJSON *root, const char *key, const char *value);
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void start_time_sync(void);
static void time_sync_notification_cb(struct timeval *tv);
static bool validate_ascii_string(const char *value, size_t min_len, size_t max_len);
static const char *wifi_reason_to_string(uint8_t reason);
static esp_err_t configure_softap_netif(void);
static softap_client_entry_t *softap_client_find(const uint8_t *mac);
static softap_client_entry_t *softap_client_alloc(const uint8_t *mac);
static bool softap_client_get_ip(const uint8_t *mac, esp_ip4_addr_t *ip_out);
static void softap_client_remove(const uint8_t *mac);
static uint64_t get_sd_free_space_bytes(void);
static void update_wifi_rssi(void);
static void update_mqtt_broker_host_status_from_uri(const char *uri);

// Sincroniza o período ativo entre a variável partilhada e g_config_user.
// Usado sempre que carregamos defaults ou dados vindos do SD.
static void set_period_ms(uint32_t period_ms)
{
    // Entra em secção crítica
    taskENTER_CRITICAL(&s_period_lock);
    
    leitura_periodo_ms = period_ms;
    
    // Sincroniza o valor na estrutura de configuração do utilizador
    g_config_user.periodo_leitura_ms = period_ms;
    
    // Sai da secção crítica
    taskEXIT_CRITICAL(&s_period_lock);
}

// Lê o período de leitura atual de forma atómica.
// Chamado por várias tasks para agendar o intervalo de medições.
uint32_t config_manager_get_period_ms(void)
{
    // Entra em secção crítica
    taskENTER_CRITICAL(&s_period_lock);
    
    // Copia o valor do período atual para variável local
    uint32_t current = leitura_periodo_ms;
    
    // Sai da secção crítica
    taskEXIT_CRITICAL(&s_period_lock);
    
    // Retorna o valor lido de forma segura
    return current;
}

// Repõe valores seguros quando não existe SD ou o ficheiro falha validação.
// Invocado no arranque antes de expor g_config_user a outros módulos.
static void apply_default_user_config(void)
{
    // Define o período de leitura com o valor padrão (2000ms)
    set_period_ms(CONFIG_DEFAULT_PERIOD_MS);
    
    // Limpa as credenciais Wi-Fi (strings vazias indicam sem configuração)
    g_config_user.wifi_ssid[0] = '\0';
    g_config_user.wifi_pass[0] = '\0';
    
    // Limpa o URI do servidor MQTT
    g_config_user.mqtt_uri[0] = '\0';
    update_mqtt_broker_host_status_from_uri(NULL);
    
    // Define valores padrão para o tópico MQTT onde os dados serão publicados
    strlcpy(g_config_user.mqtt_topic, "data", sizeof(g_config_user.mqtt_topic));
    strlcpy(g_config_user.mqtt_topic_events, "eventos", sizeof(g_config_user.mqtt_topic_events));
    
    // Define um ID de cliente MQTT padrão para identificar este dispositivo
    strlcpy(g_config_user.mqtt_client_id, "esp32_default", sizeof(g_config_user.mqtt_client_id));
    
    // Limpa as credenciais de autenticação MQTT
    g_config_user.mqtt_username[0] = '\0';
    g_config_user.mqtt_password[0] = '\0';
}

// Prepara o barramento SPI usado pelo cartão SD com pinos partilhados.
// Chamado sempre que precisamos montar o cartão e ainda não há bus válido.
static esp_err_t ensure_spi_bus_initialized(void)
{
    // Verifica se o barramento SPI já foi inicializado anteriormente
    if (s_spi_bus_ready) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,        // Pino de dados: Master Out Slave In
        .miso_io_num = PIN_SD_MISO,        // Pino de dados: Master In Slave Out
        .sclk_io_num = PIN_SD_SCK,         // Pino do clock SPI
        .quadwp_io_num = GPIO_NUM_NC,      // Não usado (modo quad)
        .quadhd_io_num = GPIO_NUM_NC,      // Não usado (modo quad)
        .max_transfer_sz = 16 * 1024,      // Tamanho máximo de transferência: 16KB
        .flags = SPICOMMON_BUSFLAG_MASTER, // Opera em modo master
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    
    if (ret == ESP_ERR_INVALID_STATE) {
        log_warn(TAG, "Barramento SPI já se encontra inicializado; reutilizar.");
        ret = ESP_OK;
    }

    // Atualiza flag e regista sucesso ou falha
    if (ret == ESP_OK) {
        s_spi_bus_ready = true;
        log_info(TAG, "Barramento SPI configurado para o cartão SD.");
    } else {
        log_error(TAG, "Falha ao inicializar o barramento SPI (%s)", esp_err_to_name(ret));
    }

    return ret;
}

// Monta o cartão microSD na VFS usando esp_vfs_fat_sdspi_mount().
// Executado no boot e sempre que um erro crítico obriga a remontar.
static esp_err_t ensure_sd_card_mounted(void)
{
    // Se já está montado, apenas atualiza o estado e retorna sucesso
    if (s_sd_mounted) {
        system_status_set_sd_state(true, get_sd_free_space_bytes());
        return ESP_OK;
    }

    // Garante que o barramento SPI está inicializado antes de prosseguir
    ESP_RETURN_ON_ERROR(ensure_spi_bus_initialized(), TAG, "SPI não disponível para o cartão SD.");

    // Configura o host SPI para comunicação com o cartão SD
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    // Configura os pinos específicos do slot do cartão SD
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cs = PIN_SD_CS;              // Chip Select do SD
    slot_config.gpio_cd = SDSPI_SLOT_NO_CD;       // Sem pino de deteção de cartão
    slot_config.gpio_wp = SDSPI_SLOT_NO_WP;       // Sem pino de proteção contra escrita

    // Configura opções de montagem do sistema de ficheiros FAT
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,          // Não formatar em caso de falha
        .max_files = 5,                           // Máximo de ficheiros abertos simultaneamente
        .allocation_unit_size = 0,                // Usar tamanho padrão do cluster
    };

    // Suprime temporariamente logs internos do driver para evitar mensagens redundantes
    esp_log_level_t prev_vfs_level = esp_log_level_get("vfs_fat_sdmmc");
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    
    // Tenta montar o cartão SD no ponto de montagem especificado
    esp_err_t ret = esp_vfs_fat_sdspi_mount(CONFIG_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    
    // Restaura o nível de log original
    esp_log_level_set("vfs_fat_sdmmc", prev_vfs_level);
    
    // Processa resultado da montagem
    if (ret == ESP_OK) {
        // Montagem bem-sucedida: atualiza flags e estado do sistema
        s_sd_mounted = true;
        s_sd_mount_err_pending = false;
        s_sd_last_mount_err = ESP_OK;
        log_info(TAG, "Cartão SD montado em %s", CONFIG_MOUNT_POINT);
        system_status_set_sd_state(true, get_sd_free_space_bytes());
    } else {
        // Falha na montagem: guarda o erro para reportar ao utilizador
        s_sd_last_mount_err = ret;
        s_sd_mount_err_pending = true;
        system_status_set_sd_state(false, 0);
    }

    return ret;
}

// Desmonta o cartão SD e marca o sistema para reinicializar quando regressar.
// Chamado após erros de IO ou deteções de remoção física.
void mark_sd_card_unavailable(const char *reason)
{
    // Se o SD já não está montado, não há nada a fazer
    if (!s_sd_mounted) {
        return;
    }

    // Regista o motivo da desmontagem para diagnóstico
    log_warn(TAG, "Marcar SD como indisponível (%s).", reason ? reason : "erro desconhecido");
    
    // Tenta desmontar o cartão SD de forma limpa
    esp_err_t unmount_err = esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, s_card);
    if (unmount_err != ESP_OK) {
        log_warn(TAG, "Falha ao desmontar SD (%s).", esp_err_to_name(unmount_err));
    }

    // Atualiza flags internas indicando que o SD não está disponível
    s_sd_mounted = false;
    s_card = NULL;
    
    // Notifica o módulo de estado que o SD foi removido/falhou
    system_status_set_sd_state(false, 0);
    
    // Marca que o sistema deve tentar remontar quando o SD regressar
    s_sd_restart_on_insert = true;
}

// Obtém em bytes o espaço livre reportado pela VFS FAT.
// Usado para expor o estado do SD no dashboard e system_status.
static uint64_t get_sd_free_space_bytes(void)
{
    // Inicializa variáveis para receber informação do sistema de ficheiros
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    
    // Consulta o VFS FAT para obter estatísticas de espaço do cartão
    esp_err_t err = esp_vfs_fat_info(CONFIG_MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        // Em caso de erro (SD removido ou corrompido), retorna zero
        log_warn(TAG, "Falha ao obter espaço livre do SD (%s).", esp_err_to_name(err));
        return 0;
    }

    // Retorna o espaço livre disponível em bytes
    return free_bytes;
}

// Wrapper público que garante que o SD está montado antes de o usar.
// Utilizado por módulos como logging e task_sensor.
esp_err_t config_manager_mount_sd(void)
{
    return ensure_sd_card_mounted();
}

// Indica se o SD está montado e pronto a ser usado.
// Consultado com frequência antes de operações de IO.
bool config_manager_is_sd_available(void)
{
    return s_sd_mounted;
}

// Faz uma leitura rápida ao FS para confirmar que o SD continua acessível.
// Invocado periodicamente pelas tasks que dependem do cartão.
bool config_manager_check_sd_alive(void)
{
    // Se o SD não estava montado, retorna falso imediatamente
    if (!s_sd_mounted) {
        return false;
    }

    // Tenta obter informações do sistema de ficheiros como teste de saúde
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(CONFIG_MOUNT_POINT, &total_bytes, &free_bytes);
    
    // Se a consulta falhar, o cartão foi removido ou está com problemas
    if (err != ESP_OK) {
        log_warn(TAG, "Perdeu acesso ao SD (%s); a desmontar.", esp_err_to_name(err));
        mark_sd_card_unavailable("verificacao periodica falhou");
        return false;
    }

    // SD continua acessível: atualiza estado com espaço livre atual
    system_status_set_sd_state(true, free_bytes);
    return true;
}

// Flag que indica se devemos reiniciar automaticamente quando o SD regressa.
// Usado pelo task_sensor ao gerir a remoção/inserção do cartão.
bool config_manager_should_restart_on_sd_return(void)
{
    return s_sd_restart_on_insert;
}

// Permite comunicar um erro de montagem pendente ao utilizador (one-shot).
// Chamado pelo dashboard para mostrar mensagens claras após falhas.
bool config_manager_pop_pending_sd_error(char *buf, size_t buf_size)
{
    // Verifica se existe um erro de montagem pendente para reportar
    if (!s_sd_mount_err_pending) {
        return false;
    }

    // Limpa a flag do erro (consumo único - padrão "pop")
    s_sd_mount_err_pending = false;
    
    // Se foi fornecido um buffer, formata a mensagem de erro para o utilizador
    if (buf && buf_size > 0) {
        snprintf(buf, buf_size, "Nenhum microSD inserido, verifica o SD (%s)",
                 esp_err_to_name(s_sd_last_mount_err));
    }
    
    // Retorna true indicando que havia um erro pendente
    return true;
}

// Lê config.json do SD e devolve o texto completo com as credenciais de Wi-Fi.
// Utilizado pelos servidores HTTP/HTTPS quando precisam renderizar dashboards.
esp_err_t config_manager_read_wifi_file(char **buffer_out)
{
    // Valida o ponteiro de saída
    if (!buffer_out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Inicializa o buffer de saída como NULL
    *buffer_out = NULL;

    // Garante que o cartão SD está montado antes de tentar ler
    ESP_RETURN_ON_ERROR(config_manager_mount_sd(), TAG, "Não foi possível montar o SD para ler config.json.");

    // Lê o ficheiro completo para memória
    esp_err_t err = read_entire_file(CONFIG_WIFI_FILE_PATH, buffer_out);
    
    // Regista o resultado da operação
    if (err != ESP_OK) {
        log_error(TAG, "Falha ao ler %s", CONFIG_WIFI_FILE_PATH);
    } else if (*buffer_out) {
        log_info(TAG, "config.json aberto com sucesso (%zu bytes).", strlen(*buffer_out));
    }

    return err;
}

// Interpreta o ficheiro JSON do portal e extrai SSID/password validados.
// Usado após upload via portal ou API antes de gravar em ficheiro.
esp_err_t config_manager_parse_wifi_credentials(const char *json_text,
                                                char *ssid_out,
                                                size_t ssid_size,
                                                char *password_out,
                                                size_t password_size)
{
    // Valida todos os parâmetros de entrada
    if (!json_text || !ssid_out || !password_out || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Faz o parsing do texto JSON recebido
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        log_error(TAG, "JSON inválido em %s", CONFIG_WIFI_FILE_PATH);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Navega pela estrutura JSON para encontrar o objeto "wifi"
    cJSON *wifi_obj = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    
    // Extrai os campos "ssid" e "password" dentro do objeto "wifi"
    cJSON *ssid_item = wifi_obj ? cJSON_GetObjectItemCaseSensitive(wifi_obj, "ssid") : NULL;
    cJSON *pass_item = wifi_obj ? cJSON_GetObjectItemCaseSensitive(wifi_obj, "password") : NULL;

    // Verifica se a estrutura JSON está correta e contém os campos necessários
    if (!cJSON_IsObject(wifi_obj) || !cJSON_IsString(ssid_item) || !cJSON_IsString(pass_item)) {
        cJSON_Delete(root);
        log_error(TAG, "config.json sem 'wifi/ssid' ou 'wifi/password' válidos.");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Copia os valores extraídos para os buffers de saída
    strlcpy(ssid_out, ssid_item->valuestring, ssid_size);
    strlcpy(password_out, pass_item->valuestring, password_size);
    
    // Liberta a memória alocada pelo parser JSON
    cJSON_Delete(root);

    // Valida que o SSID não está vazio (password pode ser vazia para redes abertas)
    if (ssid_out[0] == '\0') {
        log_error(TAG, "SSID vazio em config.json.");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Regista sucesso com o SSID extraído
    log_info(TAG, "Credenciais Wi-Fi carregadas (SSID='%s').", ssid_out);
    return ESP_OK;
}

// Lê o UID do fabricante a partir do ficheiro dedicado no SD.
// Chamado no boot e pelos módulos de segurança para assinar dados.
esp_err_t config_manager_get_device_uid(char *uid_out, size_t uid_size)
{
    // Valida os parâmetros de entrada
    if (!uid_out || uid_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Inicializa o buffer de saída como string vazia
    uid_out[0] = '\0';
    
    // Garante que o cartão SD está montado
    esp_err_t mount_err = config_manager_mount_sd();
    if (mount_err != ESP_OK) {
        return mount_err;
    }

    // Lê o ficheiro config_manufacturer.json completo para memória
    char *buffer = NULL;
    esp_err_t err = read_entire_file(CONFIG_MANUFACTURER_FILE_PATH, &buffer);
    if (err != ESP_OK) {
        log_error(TAG, "Falha ao ler %s (%s).", CONFIG_MANUFACTURER_FILE_PATH, esp_err_to_name(err));
        return err;
    }

    // Faz o parsing do conteúdo JSON
    cJSON *root = cJSON_Parse(buffer);
    free(buffer);  // Liberta o buffer de leitura imediatamente após parsing
    
    if (!root) {
        log_error(TAG, "%s contém JSON inválido.", CONFIG_MANUFACTURER_FILE_PATH);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Extrai o campo "device_uid" do JSON
    cJSON *uid_item = cJSON_GetObjectItemCaseSensitive(root, "device_uid");
    esp_err_t result = ESP_OK;
    
    // Valida que o UID existe e não está vazio
    if (!cJSON_IsString(uid_item) || uid_item->valuestring[0] == '\0') {
        log_error(TAG, "%s sem chave 'device_uid' válida.", CONFIG_MANUFACTURER_FILE_PATH);
        result = ESP_ERR_INVALID_RESPONSE;
    } else {
        // Copia o UID para o buffer de saída
        strlcpy(uid_out, uid_item->valuestring, uid_size);
        log_info(TAG, "config_manufacturer.json carregado (UID=%s).", uid_out);
    }

    // Liberta a estrutura JSON
    cJSON_Delete(root);
    return result;
}

// Carrega credenciais da STA a partir do ficheiro config.json.
// Invocado por app_main ao arrancar a interface Wi-Fi.
bool load_wifi_config(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    // Valida os buffers de saída
    if (!ssid_out || !pass_out || ssid_len == 0 || pass_len == 0) {
        ESP_LOGE(JSON_CONSOLE_TAG, "Buffers inválidos fornecidos ao loader Wi-Fi.");
        return false;
    }

    // Inicializa os buffers como strings vazias
    ssid_out[0] = '\0';
    pass_out[0] = '\0';

    // Lê o ficheiro de configuração do utilizador
    char *buffer = NULL;
    esp_err_t err = read_entire_file(CONFIG_USER_FILE_PATH, &buffer);
    if (err != ESP_OK) {
        ESP_LOGE(JSON_CONSOLE_TAG, "Não foi possível abrir %s.", CONFIG_USER_FILE_PATH);
        return false;
    }

    // Faz o parsing do JSON
    ESP_LOGI(JSON_CONSOLE_TAG, "Config carregada.");
    cJSON *root = cJSON_Parse(buffer);
    free(buffer);  // Liberta o buffer após parsing
    
    if (!root) {
        ESP_LOGE(JSON_CONSOLE_TAG, "JSON inválido em %s.", CONFIG_USER_FILE_PATH);
        return false;
    }

    // Extrai os campos SSID e password do JSON
    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, WIFI_JSON_SSID_KEY);
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, WIFI_JSON_PASS_KEY);
    bool ok = false;

    // Valida que o SSID existe e não está vazio
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        ESP_LOGE(JSON_CONSOLE_TAG, "JSON sem SSID Wi-Fi válido.");
        goto cleanup;
    }

    // Tenta desencriptar a password (versão cifrada tem prioridade)
    esp_err_t pass_status = decrypt_json_string(root,
                                                WIFI_JSON_PASS_ENC_KEY,
                                                pass_out,
                                                pass_len,
                                                WIFI_PASS_MIN_LEN,
                                                WIFI_PASS_MAX_LEN,
                                                WIFI_JSON_PASS_ENC_KEY);
    
    // Se a versão cifrada não existe, usa a versão em texto simples
    if (pass_status == ESP_ERR_NOT_FOUND) {
        if (!cJSON_IsString(pass_item)) {
            ESP_LOGE(JSON_CONSOLE_TAG, "JSON sem chaves wifi_pass/wifi_pass_enc válidas.");
            goto cleanup;
        }
        strlcpy(pass_out, pass_item->valuestring, pass_len);
    } else if (pass_status != ESP_OK) {
        // Erro ao desencriptar (ficheiro corrompido ou chave inválida)
        ESP_LOGE(JSON_CONSOLE_TAG, "Falha ao decifrar wifi_pass_enc.");
        goto cleanup;
    }

    // Valida as credenciais extraídas (tamanhos, caracteres ASCII)
    if (config_manager_check_wifi_credentials(ssid_item->valuestring, pass_out) != ESP_OK) {
        ESP_LOGE(WIFI_CONSOLE_TAG, "Credenciais Wi-Fi inválidas em config_user.json.");
        goto cleanup;
    }

    // Copia as credenciais validadas para os buffers de saída e cache global
    strlcpy(ssid_out, ssid_item->valuestring, ssid_len);
    strlcpy(g_config_user.wifi_ssid, ssid_out, sizeof(g_config_user.wifi_ssid));
    strlcpy(g_config_user.wifi_pass, pass_out, sizeof(g_config_user.wifi_pass));
    ok = true;

cleanup:
    // Liberta a estrutura JSON antes de retornar
    cJSON_Delete(root);
    return ok;
}

// Verifica se já existem credenciais Wi-Fi guardadas em memória.
// Usado pelo portal para mostrar estado das credenciais.
bool config_manager_has_saved_wifi_credentials(void)
{
    // Retorna true se o SSID não está vazio (primeira posição diferente de '\0')
    return g_config_user.wifi_ssid[0] != '\0';
}

// Valida SSID e password antes de aceitar o pedido do portal.
// Chamado pelas rotas HTTP para rejeitar entradas inválidas.
esp_err_t config_manager_check_wifi_credentials(const char *ssid, const char *pass)
{
    // Verifica se os ponteiros são válidos
    if (!ssid || !pass) {
        return ESP_ERR_INVALID_ARG;
    }

    // Valida o SSID: mínimo 1 caractere, máximo 32, apenas ASCII imprimível
    if (!validate_ascii_string(ssid, 1, WIFI_SSID_MAX_LEN)) {
        log_warn(TAG, "SSID inválido fornecido.");
        return ESP_ERR_INVALID_ARG;
    }

    // Valida a password: mínimo 8 caracteres, máximo 64, apenas ASCII imprimível
    if (!validate_ascii_string(pass, WIFI_PASS_MIN_LEN, WIFI_PASS_MAX_LEN)) {
        log_warn(TAG, "Password inválida fornecida.");
        return ESP_ERR_INVALID_ARG;
    }

    // Credenciais válidas
    return ESP_OK;
}
// Persiste credenciais Wi-Fi no ficheiro de configuração e na cache em memória.
// Usado após submissão do portal antes de reiniciar Wi-Fi.
esp_err_t config_manager_save_wifi_credentials(const char *ssid, const char *pass)
{
    // Valida as credenciais antes de gravar
    ESP_RETURN_ON_ERROR(config_manager_check_wifi_credentials(ssid, pass),
                        TAG,
                        "Credenciais Wi-Fi rejeitadas.");
    
    // Garante que o SD está montado para poder escrever
    ESP_RETURN_ON_ERROR(ensure_sd_card_mounted(), TAG, "SD não montado; impossível gravar Wi-Fi.");

    // Tenta carregar o ficheiro existente para preservar outros campos
    char *buffer = NULL;
    cJSON *root = NULL;
    if (read_entire_file(CONFIG_USER_FILE_PATH, &buffer) == ESP_OK && buffer) {
        root = cJSON_Parse(buffer);
    }
    free(buffer);

    // Se não existe ou está corrompido, cria um objeto JSON novo
    if (!root) {
        root = cJSON_CreateObject();
    }

    // Verifica se a alocação foi bem-sucedida
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    // Remove campos antigos de SSID e password (se existirem)
    cJSON_DeleteItemFromObjectCaseSensitive(root, WIFI_JSON_SSID_KEY);
    cJSON_DeleteItemFromObjectCaseSensitive(root, WIFI_JSON_PASS_KEY);
    
    // Adiciona o novo SSID ao JSON
    cJSON *ssid_node = cJSON_AddStringToObject(root, WIFI_JSON_SSID_KEY, ssid);
    if (!ssid_node) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    // Atualiza a cache em memória com as novas credenciais
    strlcpy(g_config_user.wifi_ssid, ssid, sizeof(g_config_user.wifi_ssid));
    strlcpy(g_config_user.wifi_pass, pass, sizeof(g_config_user.wifi_pass));

    // Cifra e adiciona os campos sensíveis (password) ao JSON
    esp_err_t enc_err = store_encrypted_fields(root);
    if (enc_err != ESP_OK) {
        cJSON_Delete(root);
        return enc_err;
    }

    // Serializa o JSON para string (formato compacto sem indentação)
    char *json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_text) {
        return ESP_ERR_NO_MEM;
    }

    // Escreve a string JSON no ficheiro do SD
    esp_err_t err = write_text_file(CONFIG_USER_FILE_PATH, json_text);
    cJSON_free(json_text);

    // Regista sucesso da operação
    if (err == ESP_OK) {
        log_info(TAG, "Credenciais Wi-Fi guardadas (SSID=%s).", ssid);
    }

    return err;
}

// Garante que um ficheiro JSON existe e é válido (caso contrário cria defaults).
// Chamado durante init/reload para manter ficheiros do SD consistentes.
static esp_err_t ensure_json_ready(const char *path,
                                  const char *label,
                                  config_create_fn_t create_fn,
                                   config_read_fn_t read_fn,
                                   config_defaults_fn_t defaults_fn,
                                   config_create_fn_t rewrite_fn)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        log_warn(TAG, "%s não encontrado; a criar com valores por defeito.", label);
        ESP_RETURN_ON_ERROR(create_fn(), TAG, "Falha ao criar ficheiro por defeito.");
    }

    esp_err_t err = read_fn();
    if (err != ESP_OK) {
        log_error(TAG, "Erro ao interpretar %s; a reverter para defaults.", label);
        defaults_fn();
        config_create_fn_t writer = rewrite_fn ? rewrite_fn : create_fn;
        ESP_RETURN_ON_ERROR(writer(), TAG, "Falha ao regravar ficheiro por defeito.");
    } else {
        log_info(TAG, "%s válido e carregado.", label);
    }

    return ESP_OK;
}

// Lê todo o conteúdo de um ficheiro texto para memória (terminado a zero).
// Reutilizado pelos leitores de configuração e rotinas auxiliares.
static esp_err_t read_entire_file(const char *path, char **buffer_out)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        int errsv = errno;
        log_error(TAG, "Erro ao abrir %s (errno=%d)", path, errsv);
        if (errsv != ENOENT) {
            mark_sd_card_unavailable("falha ao abrir ficheiro");
        }
        return ESP_FAIL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        int errsv = errno;
        fclose(file);
        log_error(TAG, "Não foi possível obter o tamanho de %s (errno=%d).", path, errsv);
        mark_sd_card_unavailable("falha ao posicionar ficheiro");
        return ESP_FAIL;
    }

    long length = ftell(file);
    if (length <= 0) {
        fclose(file);
        log_error(TAG, "%s vazio ou inválido.", path);
        return ESP_FAIL;
    }
    rewind(file);

    char *buffer = (char *)calloc(1, length + 1);
    if (!buffer) {
        fclose(file);
        log_error(TAG, "Sem memória para carregar %s.", path);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buffer, 1, length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(buffer);
        log_error(TAG, "Leitura incompleta de %s (esperado %ld, obtido %zu).", path, length, read);
        mark_sd_card_unavailable("falha ao ler ficheiro");
        return ESP_FAIL;
    }

    buffer[length] = '\0';
    *buffer_out = buffer;
    ESP_LOGD(TAG, "Leitura completa de %s (%ld bytes).", path, length);     //LOGD- o D serve para só aparecer se estiver no modo DEBUG
    return ESP_OK;
}

// Funcao que vai para escrever texto para um ficheiro
// Utilizado quando atualizamos JSONs e ficheiros auxiliares.
static esp_err_t write_text_file(const char *path, const char *text)
{
    // Abre o ficheiro em modo escrita (cria novo ou sobrescreve existente)
    FILE *file = fopen(path, "w");
    if (!file) {
        int errsv = errno;
        log_error(TAG, "Não foi possível abrir %s para escrita (errno=%d).", path, errsv);
        // Se o erro não é "ficheiro não encontrado", pode ser problema do SD
        if (errsv != ENOENT) {
            mark_sd_card_unavailable("falha ao abrir ficheiro para escrita");
        }
        return ESP_FAIL;
    }

    // Escreve o texto completo no ficheiro
    size_t length = strlen(text);
    size_t written = fwrite(text, 1, length, file);
    
    // Força flush do buffer interno do C para o sistema operativo
    int flush_status = fflush(file);
    
    // Força sincronização do buffer do SO para o hardware (SD físico)
    fsync(fileno(file));
    
    // Fecha o ficheiro
    fclose(file);

    // Verifica se a escrita foi completa e bem-sucedida
    if (written != length || flush_status != 0) {
        log_error(TAG, "Falha ao escrever %s (written=%zu, esperado=%zu).", path, written, length);
        mark_sd_card_unavailable("falha ao escrever ficheiro");
        return ESP_FAIL;
    }

    log_info(TAG, "%s escrito com sucesso (%zu bytes).", path, length);
    return ESP_OK;
}

// Codifica dados binários em base64 alocado dinamicamente.
// Usado antes de persistir strings cifradas no JSON.
static esp_err_t base64_encode_alloc(const uint8_t *data, size_t data_len, char **out_b64)
{
    // Valida os parâmetros de entrada
    if (!data || data_len == 0 || !out_b64) {
        return ESP_ERR_INVALID_ARG;
    }

    // Primeira chamada: determina o tamanho necessário para o buffer
    size_t encoded_len = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &encoded_len, data, data_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    // Aloca buffer com o tamanho calculado (mais 1 para '\0')
    char *buffer = (char *)calloc(1, encoded_len + 1);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    // Segunda chamada: faz a codificação real para o buffer alocado
    ret = mbedtls_base64_encode((unsigned char *)buffer, encoded_len, &encoded_len, data, data_len);
    if (ret != 0) {
        free(buffer);
        ESP_LOGE(TAG, "Falha ao codificar base64 (ret=%d).", ret);
        return ESP_FAIL;
    }

    // Garante terminação nula e retorna o buffer
    buffer[encoded_len] = '\0';
    *out_b64 = buffer;
    return ESP_OK;
}

// Decodifica base64 vindo do JSON e devolve buffer heap.
// Necessário para recuperar credenciais cifradas.
static esp_err_t base64_decode_alloc(const char *b64, uint8_t **out_buf, size_t *out_len)
{
    // Valida os parâmetros de entrada
    if (!b64 || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    // Primeira chamada: determina o tamanho necessário após descodificação
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &decoded_len,
                                    (const unsigned char *)b64, strlen(b64));
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGE(TAG, "Base64 inválido para campo protegido.");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Aloca buffer para os dados descodificados
    uint8_t *buffer = (uint8_t *)calloc(1, decoded_len);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    // Segunda chamada: descodifica para o buffer alocado
    ret = mbedtls_base64_decode(buffer, decoded_len, &decoded_len,
                                (const unsigned char *)b64, strlen(b64));
    if (ret != 0) {
        free(buffer);
        ESP_LOGE(TAG, "Falha ao descodificar base64 (ret=%d).", ret);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Retorna o buffer e o tamanho real dos dados
    *out_buf = buffer;
    *out_len = decoded_len;
    return ESP_OK;
}

static esp_err_t decrypt_json_string(const cJSON *root,
                                     const char *enc_key,
                                     char *out,
                                     size_t out_size,
                                     size_t min_len,
                                     size_t max_len,
                                     const char *label)
{
    if (!root || !enc_key || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Procura o campo cifrado no JSON
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, enc_key);
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        // Campo não existe: retorna NOT_FOUND para permitir fallback para plaintext
        return ESP_ERR_NOT_FOUND;
    }

    // Descodifica o base64 para obter os dados cifrados binários
    uint8_t *cipher = NULL;
    size_t cipher_len = 0;
    esp_err_t err = base64_decode_alloc(item->valuestring, &cipher, &cipher_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Campo %s com base64 inválido.", label ? label : enc_key);
        return err;
    }

    // Desencripta os dados usando AES (chave derivada do hardware EFUSE)
    uint8_t *plaintext = NULL;
    size_t plaintext_len = 0;
    err = security_aes_decrypt(cipher, cipher_len, &plaintext, &plaintext_len);
    free(cipher);  // Liberta dados cifrados imediatamente após uso
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao desencriptar %s.", label ? label : enc_key);
        return err;
    }

    // Valida o tamanho do texto desencriptado
    if (plaintext_len < min_len || plaintext_len > max_len || plaintext_len >= out_size) {
        free(plaintext);
        ESP_LOGE(TAG, "Tamanho inválido para %s (len=%u).",
                 label ? label : enc_key,
                 (unsigned)plaintext_len);
        return ESP_ERR_INVALID_SIZE;
    }

    // Copia o texto desencriptado para o buffer de saída e termina com '\0'
    memcpy(out, plaintext, plaintext_len);
    out[plaintext_len] = '\0';
    
    // Limpa e liberta o buffer temporário com dados sensíveis
    free(plaintext);
    return ESP_OK;
}

static esp_err_t encrypt_and_store_string(cJSON *root,
                                          const char *plain_key,
                                          const char *enc_key,
                                          const char *value)
{
    // Valida os parâmetros obrigatórios
    if (!root || !enc_key) {
        return ESP_ERR_INVALID_ARG;
    }

    // Remove versões antigas do campo cifrado (se existir)
    cJSON_DeleteItemFromObjectCaseSensitive(root, enc_key);
    
    // Remove também a versão plaintext se especificada (migração de segurança)
    if (plain_key) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, plain_key);
    }

    // Se o valor está vazio, não há nada para cifrar
    if (!value || value[0] == '\0') {
        return ESP_OK;
    }

    // Cifra o valor usando AES com chave derivada do hardware
    uint8_t *cipher = NULL;
    size_t cipher_len = 0;
    esp_err_t err = security_aes_encrypt((const uint8_t *)value, strlen(value), &cipher, &cipher_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao cifrar campo %s.", enc_key);
        return err;
    }

    // Codifica os dados cifrados em base64 para armazenamento em JSON
    char *b64 = NULL;
    err = base64_encode_alloc(cipher, cipher_len, &b64);
    free(cipher);  // Liberta dados cifrados após codificação
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao codificar base64 para %s.", enc_key);
        return err;
    }

    // Adiciona o campo cifrado ao objeto JSON
    cJSON *added = cJSON_AddStringToObject(root, enc_key, b64);
    free(b64);  // Liberta string base64 após adicionar ao JSON
    
    if (!added) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

// Atualiza o JSON com as versões cifradas dos campos sensíveis.
// Invocado sempre que vamos persistir config_user.json.
static esp_err_t store_encrypted_fields(cJSON *root)
{
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "JSON inválido para cifrar.");

    ESP_RETURN_ON_ERROR(encrypt_and_store_string(root,
                                                 WIFI_JSON_PASS_KEY,
                                                 WIFI_JSON_PASS_ENC_KEY,
                                                 g_config_user.wifi_pass),
                        TAG,
                        "Falha ao cifrar wifi_pass.");

    // Garante que mqtt_* ficam apenas em plaintext removendo campos *_enc herdados.
    // Evita que versões antigas do ficheiro mantenham dados inconsistentes.
    cJSON_DeleteItemFromObjectCaseSensitive(root, MQTT_PASSWORD_JSON_ENC_KEY);
    cJSON_DeleteItemFromObjectCaseSensitive(root, MQTT_URI_JSON_ENC_KEY);
    ensure_plain_string(root, MQTT_PASSWORD_JSON_KEY, g_config_user.mqtt_password);
    ensure_plain_string(root, MQTT_URI_JSON_KEY, g_config_user.mqtt_uri);
    ensure_plain_string(root, "mqtt_topic", g_config_user.mqtt_topic);
    ensure_plain_string(root, "mqtt_topic_events", g_config_user.mqtt_topic_events);

    return ESP_OK;
}

// Garante que o campo plaintext existe mesmo quando a versão cifrada é usada.
// Necessário para manter compatibilidade com dashboards que só leem texto claro.
static void ensure_plain_string(cJSON *root, const char *key, const char *value)
{
    if (!root || !key) {
        return;
    }

    if (!value || value[0] == '\0') {
        return;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, value) == 0) {
        return;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddStringToObject(root, key, value);
}

// Extrai o host/IP do URI MQTT e atualiza o snapshot para o dashboard HTTPS.
static void update_mqtt_broker_host_status_from_uri(const char *uri)
{
    char host[SYSTEM_STATUS_BROKER_HOST_MAX_LEN] = {0};

    if (uri && uri[0]) {
        const char *start = uri;
        const char *scheme = strstr(uri, "://");
        if (scheme) {
            start = scheme + 3;
        }

        if (*start == '[') {
            start++;
            const char *end = strchr(start, ']');
            size_t len = end ? (size_t)(end - start) : strcspn(start, "]");
            if (len >= sizeof(host)) {
                len = sizeof(host) - 1;
            }
            memcpy(host, start, len);
            host[len] = '\0';
        } else {
            size_t len = strcspn(start, ":/");
            if (len >= sizeof(host)) {
                len = sizeof(host) - 1;
            }
            memcpy(host, start, len);
            host[len] = '\0';
        }
    }

    if (!host[0]) {
        strlcpy(host, "desconhecido", sizeof(host));
    }

    system_status_set_mqtt_broker_host(host);
}

// Carrega o período de amostragem a partir de config_user.json.
// Pode reescrever o ficheiro com defaults caso a validação falhe.
static esp_err_t read_user_config(bool rewrite_if_needed)
{
    // Começa por aplicar valores padrão (serão substituídos se o ficheiro for válido)
    apply_default_user_config();

    // Lê o ficheiro completo para memória
    char *buffer = NULL;
    esp_err_t err = read_entire_file(CONFIG_USER_FILE_PATH, &buffer);
    if (err != ESP_OK) {
        return err;
    }

    // Faz o parsing do JSON
    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        log_error(TAG, "JSON inválido em %s.", CONFIG_USER_FILE_PATH);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Extrai o campo do período de leitura
    cJSON *period_item = cJSON_GetObjectItemCaseSensitive(root, "periodo_leitura_ms");
    
    // Flags para controlar se precisamos reescrever o ficheiro
    bool needs_rewrite = false;
    esp_err_t rewrite_status = ESP_OK;

    // Inicializa com o período padrão
    uint32_t parsed_period = CONFIG_DEFAULT_PERIOD_MS;
    // Tenta extrair o valor numérico do período
    if (cJSON_IsNumber(period_item)) {
        parsed_period = (uint32_t)period_item->valuedouble;
    } else {
        // Campo não existe ou não é numérico: usa default e marca para reescrever
        log_warn(TAG, "config_user.json sem 'periodo_leitura_ms' numérico; a aplicar valor por defeito.");
        needs_rewrite = true;
    }

    // Valida que o período está dentro dos limites permitidos
    if (parsed_period < CONFIG_MANAGER_PERIOD_MIN_MS || parsed_period > CONFIG_MANAGER_PERIOD_MAX_MS) {
        log_warn(TAG,
                 "[CONFIG] Período lido inválido (%lu ms); a usar valor por defeito (%lu ms).",
                 (unsigned long)parsed_period,
                 (unsigned long)CONFIG_DEFAULT_PERIOD_MS);
        parsed_period = CONFIG_DEFAULT_PERIOD_MS;
        needs_rewrite = true;
    }

    // Aplica o período validado à memória partilhada
    set_period_ms(parsed_period);

    // Verifica se o ficheiro ainda contém passwords em texto simples (legacy)
    bool has_plain_wifi_pass = cJSON_GetObjectItemCaseSensitive(root, WIFI_JSON_PASS_KEY) != NULL;
    
    // Flags para controlar migração de campos MQTT para plaintext
    bool rewrite_plain_mqtt_password = false;
    bool rewrite_plain_mqtt_uri = false;

    // Extrai e valida o SSID
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, WIFI_JSON_SSID_KEY);
    if (cJSON_IsString(ssid_item) &&
        validate_ascii_string(ssid_item->valuestring, 1, WIFI_SSID_MAX_LEN)) {
        // SSID válido: copia para a estrutura de configuração
        strlcpy(g_config_user.wifi_ssid, ssid_item->valuestring, sizeof(g_config_user.wifi_ssid));
    } else if (ssid_item) {
        // Campo existe mas é inválido
        log_warn(TAG, "SSID inválido em config_user.json; a ignorar valor.");
    }

    // Tenta carregar a password Wi-Fi cifrada (método preferido)
    esp_err_t wifi_dec = decrypt_json_string(root,
                                             WIFI_JSON_PASS_ENC_KEY,
                                             g_config_user.wifi_pass,
                                             sizeof(g_config_user.wifi_pass),
                                             WIFI_PASS_MIN_LEN,
                                             WIFI_PASS_MAX_LEN,
                                             WIFI_JSON_PASS_ENC_KEY);
    
    // Se não existe versão cifrada, tenta a versão plaintext (legacy)
    if (wifi_dec == ESP_ERR_NOT_FOUND) {
        const cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, WIFI_JSON_PASS_KEY);
        if (cJSON_IsString(pass_item) &&
            validate_ascii_string(pass_item->valuestring, WIFI_PASS_MIN_LEN, WIFI_PASS_MAX_LEN)) {
            // Password plaintext válida: copia e marca para cifrar no próximo save
            strlcpy(g_config_user.wifi_pass, pass_item->valuestring, sizeof(g_config_user.wifi_pass));
        } else if (pass_item) {
            log_warn(TAG, "Password Wi-Fi inválida em config_user.json; a ignorar valor.");
        }
    } else if (wifi_dec != ESP_OK) {
        // Erro ao desencriptar: ficheiro corrompido ou chave inválida
        ESP_LOGE(TAG, "config_user.json corrompido (wifi_pass_enc).");
        cJSON_Delete(root);
        return wifi_dec;
    }

    // Valida a password depois de desencriptar (medida de segurança extra)
    if (wifi_dec == ESP_OK &&
        !validate_ascii_string(g_config_user.wifi_pass, WIFI_PASS_MIN_LEN, WIFI_PASS_MAX_LEN)) {
        log_warn(TAG, "Password Wi-Fi decifrada inválida; a ignorar valor.");
        g_config_user.wifi_pass[0] = '\0';
        needs_rewrite = true;
    }

    const cJSON *uri_item = cJSON_GetObjectItemCaseSensitive(root, MQTT_URI_JSON_KEY);
    esp_err_t uri_dec = decrypt_json_string(root,
                                            MQTT_URI_JSON_ENC_KEY,
                                            g_config_user.mqtt_uri,
                                            sizeof(g_config_user.mqtt_uri),
                                            0,
                                            sizeof(g_config_user.mqtt_uri) - 1,
                                            MQTT_URI_JSON_ENC_KEY);
    if (uri_dec == ESP_ERR_NOT_FOUND) {
        if (cJSON_IsString(uri_item) && uri_item->valuestring) {
            strlcpy(g_config_user.mqtt_uri, uri_item->valuestring, sizeof(g_config_user.mqtt_uri));
        }
    } else if (uri_dec != ESP_OK) {
        ESP_LOGE(TAG, "config_user.json corrompido (mqtt_uri_enc).");
        cJSON_Delete(root);
        return uri_dec;
    } else {
        rewrite_plain_mqtt_uri = true;
    }

    cJSON *topic_item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_topic");
    if (cJSON_IsString(topic_item) && topic_item->valuestring && topic_item->valuestring[0]) {
        strlcpy(g_config_user.mqtt_topic, topic_item->valuestring, sizeof(g_config_user.mqtt_topic));
    }

    cJSON *topic_events_item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_topic_events");
    if (cJSON_IsString(topic_events_item) && topic_events_item->valuestring && topic_events_item->valuestring[0]) {
        strlcpy(g_config_user.mqtt_topic_events, topic_events_item->valuestring, sizeof(g_config_user.mqtt_topic_events));
    }

    cJSON *client_id_item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_client_id");
    if (cJSON_IsString(client_id_item) && client_id_item->valuestring && client_id_item->valuestring[0]) {
        strlcpy(g_config_user.mqtt_client_id, client_id_item->valuestring, sizeof(g_config_user.mqtt_client_id));
    }

    cJSON *username_item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_username");
    if (cJSON_IsString(username_item) && username_item->valuestring) {
        strlcpy(g_config_user.mqtt_username, username_item->valuestring, sizeof(g_config_user.mqtt_username));
    }

    esp_err_t password_dec = decrypt_json_string(root,
                                                 MQTT_PASSWORD_JSON_ENC_KEY,
                                                 g_config_user.mqtt_password,
                                                 sizeof(g_config_user.mqtt_password),
                                                 0,
                                                 sizeof(g_config_user.mqtt_password) - 1,
                                                 MQTT_PASSWORD_JSON_ENC_KEY);
    if (password_dec == ESP_ERR_NOT_FOUND) {
        const cJSON *password_item = cJSON_GetObjectItemCaseSensitive(root, MQTT_PASSWORD_JSON_KEY);
        if (cJSON_IsString(password_item) && password_item->valuestring) {
            strlcpy(g_config_user.mqtt_password, password_item->valuestring, sizeof(g_config_user.mqtt_password));
        }
    } else if (password_dec != ESP_OK) {
        ESP_LOGE(TAG, "config_user.json corrompido (mqtt_password_enc).");
        cJSON_Delete(root);
        return password_dec;
    } else {
        rewrite_plain_mqtt_password = true;
    }

    if (has_plain_wifi_pass) {
        needs_rewrite = true;
    }
    if (rewrite_plain_mqtt_password || rewrite_plain_mqtt_uri) {
        needs_rewrite = true;
    }

    if (rewrite_if_needed && needs_rewrite) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "periodo_leitura_ms");
        cJSON_AddNumberToObject(root, "periodo_leitura_ms", parsed_period);
        esp_err_t enc_err = store_encrypted_fields(root);
        if (enc_err != ESP_OK) {
            cJSON_Delete(root);
            return enc_err;
        }
        char *fixed_json = cJSON_PrintUnformatted(root);
        if (fixed_json) {
            rewrite_status = write_text_file(CONFIG_USER_FILE_PATH, fixed_json);
            cJSON_free(fixed_json);
        } else {
            log_error(TAG, "Sem memória para reescrever config_user.json com período por defeito.");
            rewrite_status = ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(root);
    ESP_LOGD(TAG, "config_user.json carregado (periodo=%lu ms).",
             (unsigned long)parsed_period);
    update_mqtt_broker_host_status_from_uri(g_config_user.mqtt_uri);
    return rewrite_status;
}

// Helper passado para ensure_json_ready que força revalidação do user config.
// Invocado tanto no arranque como durante reloads solicitados.
static esp_err_t read_user_config_wrapper(void)
{
    return read_user_config(true);
}

// Escreve no SD o config_user.json com o novo período solicitado.
// Chamado pelo servidor HTTPS quando o utilizador altera o intervalo.
static esp_err_t persist_user_config(uint32_t period_ms)
{
    char *buffer = NULL;
    cJSON *root = NULL;

    if (read_entire_file(CONFIG_USER_FILE_PATH, &buffer) == ESP_OK && buffer) {
        root = cJSON_Parse(buffer);
    }
    free(buffer);

    if (!root) {
        root = cJSON_CreateObject();
    }

    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *period_item = cJSON_GetObjectItemCaseSensitive(root, "periodo_leitura_ms");
    if (period_item && !cJSON_IsNumber(period_item)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "periodo_leitura_ms");
        period_item = NULL;
    }

    if (!period_item) {
        cJSON_AddNumberToObject(root, "periodo_leitura_ms", period_ms);
    } else {
        cJSON_SetNumberValue(period_item, period_ms);
    }

    esp_err_t enc_err = store_encrypted_fields(root);
    if (enc_err != ESP_OK) {
        cJSON_Delete(root);
        return enc_err;
    }

    char *json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_text) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = write_text_file(CONFIG_USER_FILE_PATH, json_text);
    cJSON_free(json_text);

    if (err == ESP_OK) {
        set_period_ms(period_ms);
        log_info(TAG, "config_user.json atualizado (periodo=%lu ms).", (unsigned long)period_ms);
    }

    return err;
}

// Cria um config_user.json novo com valores seguros e limpa notícias antigas.
// Utilizado quando o ficheiro está ausente ou irrecuperável.
static esp_err_t create_default_user_config(void)
{
    // Reutiliza a mesma rotina de escrita para garantir consistência e logs.
    return persist_user_config(CONFIG_DEFAULT_PERIOD_MS);
}

// Configuração inicial: garante defaults, monta SD e valida ficheiros.
// Chamado pela task_config_loader durante o boot inicial do firmware.
esp_err_t config_manager_init(void)
{
    apply_default_user_config();

    // Monta o cartão SD (esp_vfs_fat_sdspi_mount) antes de mexer em ficheiros.
    esp_err_t err = ensure_sd_card_mounted();
    if (err != ESP_OK) {
        log_error(TAG, "Não foi possível montar o cartão SD; a usar defaults em RAM.");
        return err;
    }

    ESP_RETURN_ON_ERROR(ensure_json_ready(CONFIG_USER_FILE_PATH,
                                          "config_user.json",
                                          create_default_user_config,
                                          read_user_config_wrapper,
                                          apply_default_user_config,
                                          create_default_user_config),
                        TAG,
                        "Falha ao preparar config_user.json.");

    log_info(TAG, "Config aplicada: periodo=%lu ms",
             (unsigned long)config_manager_get_period_ms());

    return ESP_OK;
}

// Reabre config_user.json e aplica alterações de período sem reiniciar.
// Invocado por task_sensor/task_config quando o utilizador grava novos valores.
esp_err_t config_manager_reload_user_config(void)
{
    esp_err_t err = ensure_sd_card_mounted();
    if (err != ESP_OK) {
        log_warn(TAG, "SD não disponível durante reload (%s).", esp_err_to_name(err));
        return err;
    }

    err = read_user_config(true);
    if (err != ESP_OK) {
        log_warn(TAG, "Falha ao recarregar config_user.json (%s).", esp_err_to_name(err));
    }

    return err;
}

// API simples para o HTTPS server atualizar config_user.json.
// Recebe o período em segundos e delega no persist_user_config().
esp_err_t save_config_to_json(uint32_t period_ms)
{
    // Interface pública para as tarefas que ajustam o período de leitura em tempo real.
    ESP_RETURN_ON_ERROR(ensure_sd_card_mounted(), TAG, "SD não montado; não é possível gravar a configuração.");

    if (period_ms < CONFIG_MANAGER_PERIOD_MIN_MS || period_ms > CONFIG_MANAGER_PERIOD_MAX_MS) {
        log_error(TAG, "Período fora do intervalo permitido (%lu ms).", (unsigned long)period_ms);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = persist_user_config(period_ms);
    return err;
}

// Atualiza o período via segundos e notifica os consumidores do novo valor.
// Chamado pela UI web quando o slider de intervalo é alterado.
esp_err_t config_manager_set_period_seconds(uint32_t period_seconds)
{
    if (period_seconds < CONFIG_MANAGER_PERIOD_MIN_S || period_seconds > CONFIG_MANAGER_PERIOD_MAX_S) {
        log_error(TAG, "Período rejeitado (%lu s). Intervalo permitido: [%u; %u] s.",
                  (unsigned long)period_seconds,
                  CONFIG_MANAGER_PERIOD_MIN_S,
                  CONFIG_MANAGER_PERIOD_MAX_S);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t new_period_ms = period_seconds * 1000U;
    return save_config_to_json(new_period_ms);
}

// Garante que o loop de eventos padrão do ESP-IDF está criado.
// Necessário antes de registar handlers Wi-Fi e SNTP.
static esp_err_t ensure_event_loop_ready(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        log_error(TAG, "Falha ao inicializar esp_netif (%s)", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        log_error(TAG, "Falha ao criar event loop (%s)", esp_err_to_name(err));
        return err;
    }

    if (!s_wifi_sta_netif) {
        s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_wifi_sta_netif) {
            log_error(TAG, "Não foi possível criar o netif STA");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_wifi_ap_netif) {
        s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_wifi_ap_netif) {
            log_error(TAG, "Não foi possível criar o netif SoftAP");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_netif_logs_suppressed) {
        esp_log_level_set("esp_netif_handlers", ESP_LOG_NONE);
        s_netif_logs_suppressed = true;
    }

    if (!s_wifi_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                            TAG,
                            "Falha ao registar handler de eventos Wi-Fi.");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                            TAG,
                            "Falha ao registar handler de eventos IP.");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_event_handler, NULL),
                            TAG,
                            "Falha ao registar handler de IP do SoftAP.");
        s_wifi_handlers_registered = true;
        log_info(TAG, "Handlers de eventos Wi-Fi registados.");
    }

    return ESP_OK;
}

// Configura as interfaces de rede STA e SoftAP com endereços e handlers.
// Executado uma única vez durante o init do Wi-Fi.
static esp_err_t configure_softap_netif(void)
{
    if (!s_wifi_ap_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .gw.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    };

    esp_err_t err = esp_netif_dhcps_stop(s_wifi_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        log_error(TAG, "Falha ao parar DHCP do SoftAP (%s)", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_set_ip_info(s_wifi_ap_netif, &ip_info);
    if (err != ESP_OK) {
        log_error(TAG, "Não foi possível configurar IP do SoftAP (%s)", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_dhcps_start(s_wifi_ap_netif);
    if (err != ESP_OK) {
        log_error(TAG, "Falha ao iniciar DHCP server do SoftAP (%s)", esp_err_to_name(err));
        return err;
    }

    log_info(TAG, "SoftAP configurado com IP " IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

// Procura na tabela interna um cliente SoftAP pelo MAC.
// Usado pelos handlers Wi-Fi para atualizar entradas existentes.
static softap_client_entry_t *softap_client_find(const uint8_t *mac)
{
    if (!mac) {
        return NULL;
    }

    for (size_t i = 0; i < SOFTAP_MAX_CLIENTS; ++i) {
        if (s_softap_clients[i].valid &&
            memcmp(s_softap_clients[i].mac, mac, sizeof(s_softap_clients[i].mac)) == 0) {
            return &s_softap_clients[i];
        }
    }
    return NULL;
}

// Reserva um slot para um cliente SoftAP recém-conectado.
// Chamado quando o evento WIFI_EVENT_AP_STACONNECTED chega.
static softap_client_entry_t *softap_client_alloc(const uint8_t *mac)
{
    if (!mac) {
        return NULL;
    }

    softap_client_entry_t *entry = softap_client_find(mac);
    if (entry) {
        return entry;
    }

    for (size_t i = 0; i < SOFTAP_MAX_CLIENTS; ++i) {
        if (!s_softap_clients[i].valid) {
            s_softap_clients[i].valid = true;
            memcpy(s_softap_clients[i].mac, mac, sizeof(s_softap_clients[i].mac));
            s_softap_clients[i].aid = 0;
            s_softap_clients[i].ip.addr = 0;
            return &s_softap_clients[i];
        }
    }

    // Sem espaço livre; reutilizar a primeira entrada.
    s_softap_clients[0].valid = true;
    memcpy(s_softap_clients[0].mac, mac, sizeof(s_softap_clients[0].mac));
    s_softap_clients[0].aid = 0;
    s_softap_clients[0].ip.addr = 0;
    return &s_softap_clients[0];
}

// Recupera o IP associado a um cliente conhecido.
// Usado ao construir respostas JSON com a lista de clientes.
static bool softap_client_get_ip(const uint8_t *mac, esp_ip4_addr_t *ip_out)
{
    if (!mac || !ip_out) {
        return false;
    }

    softap_client_entry_t *entry = softap_client_find(mac);
    if (entry && entry->ip.addr != 0) {
        *ip_out = entry->ip;
        return true;
    }
    return false;
}

// Remove um cliente SoftAP da tabela quando se desconecta.
// Invocado pelos eventos WIFI_EVENT_AP_STADISCONNECTED.
static void softap_client_remove(const uint8_t *mac)
{
    softap_client_entry_t *entry = softap_client_find(mac);
    if (entry) {
        memset(entry, 0, sizeof(*entry));
    }
}

// Copia o snapshot atual de clientes SoftAP para exportação via dashboard.
// Chamado pelo servidor HTTPS quando lista os dispositivos ligados.
size_t config_manager_get_softap_clients(config_softap_client_info_t *out_array, size_t max_entries)
{
    size_t total = 0;
    size_t written = 0;

    for (size_t i = 0; i < SOFTAP_MAX_CLIENTS; ++i) {
        if (!s_softap_clients[i].valid) {
            continue;
        }
        total++;
        if (!out_array || written >= max_entries) {
            continue;
        }

        config_softap_client_info_t *dst = &out_array[written++];
        snprintf(dst->mac,
                 sizeof(dst->mac),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_softap_clients[i].mac[0], s_softap_clients[i].mac[1],
                 s_softap_clients[i].mac[2], s_softap_clients[i].mac[3],
                 s_softap_clients[i].mac[4], s_softap_clients[i].mac[5]);
        if (s_softap_clients[i].ip.addr != 0) {
            snprintf(dst->ip,
                     sizeof(dst->ip),
                     IPSTR,
                     IP2STR(&s_softap_clients[i].ip));
            dst->ip_assigned = true;
        } else {
            strlcpy(dst->ip, "—", sizeof(dst->ip));
            dst->ip_assigned = false;
        }
        dst->aid = s_softap_clients[i].aid;
    }

    return total;
}

// Tenta ligar ao Wi-Fi STA com as credenciais fornecidas e atualiza estado.
// Usado pelo portal após guardar novas credenciais sem reiniciar a board.
esp_err_t config_manager_apply_sta_credentials(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_ERROR(config_manager_check_wifi_credentials(ssid, pass),
                        TAG,
                        "Credenciais rejeitadas antes de aplicar na STA.");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        log_error(TAG, "Falha ao aplicar config STA (%s).", esp_err_to_name(err));
        return err;
    }

    strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE && err != ESP_ERR_WIFI_CONN) {
        log_error(TAG, "Falha ao iniciar reconexão Wi-Fi (%s).", esp_err_to_name(err));
        return err;
    }

    system_status_set_wifi_connected(false);
    log_info(TAG, "STA a ligar ao SSID %s.", ssid);
    return ESP_OK;
}

// Garantia de que o Non-Volatile Storage está pronto, requisito para guardar parâmetros Wi-Fi.
// Chamado antes de manipular netifs nas rotinas de init.
static esp_err_t ensure_nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Falha ao limpar NVS.");
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        log_error(TAG, "NVS não disponível (%s)", esp_err_to_name(err));
    }
    return err;
}

// Handler central para eventos Wi-Fi/IP (arranque, ligação, desconexão e IP obtido).
// Registado durante o init para manter o system_status e listas atualizadas.
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            system_status_set_wifi_connected(false);
            if (s_wifi_ssid[0]) {
                ESP_LOGI(WIFI_CONSOLE_TAG, "A ligar a %s...", s_wifi_ssid);
            } else {
                ESP_LOGI(WIFI_CONSOLE_TAG, "A ligar ao Wi-Fi configurado...");
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            system_status_set_wifi_connected(true);
            update_wifi_rssi();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            const char *reason = wifi_reason_to_string(disc ? disc->reason : 0);
            ESP_LOGW(WIFI_CONSOLE_TAG, "Ligação falhou (%s). A tentar novamente...", reason);
            esp_wifi_connect();
            system_status_set_wifi_connected(false);
            break;
        }
        case WIFI_EVENT_AP_START:
            log_info(TAG, "SoftAP ativo; clientes podem ligar-se com o SSID %s.", SOFTAP_DEFAULT_SSID);
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *conn = (wifi_event_ap_staconnected_t *)event_data;
            if (conn) {
                softap_client_entry_t *entry = softap_client_alloc(conn->mac);
                if (entry) {
                    memcpy(entry->mac, conn->mac, sizeof(entry->mac));
                    entry->aid = conn->aid;
                    entry->ip.addr = 0;
                }
                ESP_LOGW(WIFI_CONSOLE_TAG,
                         "\033[33mSoftAP: dispositivo ligado (MAC=%02X:%02X:%02X:%02X:%02X:%02X AID=%d).\033[0m",
                         conn->mac[0], conn->mac[1], conn->mac[2],
                         conn->mac[3], conn->mac[4], conn->mac[5],
                         conn->aid);
            }
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *disc = (wifi_event_ap_stadisconnected_t *)event_data;
            if (disc) {
                char mac_buf[18];
                snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                         disc->mac[0], disc->mac[1], disc->mac[2],
                         disc->mac[3], disc->mac[4], disc->mac[5]);
                esp_ip4_addr_t last_ip = {0};
                bool has_ip = softap_client_get_ip(disc->mac, &last_ip);
                softap_client_remove(disc->mac);
                char ip_str[16];
                if (has_ip) {
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&last_ip));
                } else {
                    strlcpy(ip_str, "desconhecido", sizeof(ip_str));
                }
                ESP_LOGW(WIFI_CONSOLE_TAG,
                         "\033[33mSoftAP: dispositivo saiu (MAC=%s IP=%s AID=%d).\033[0m",
                         mac_buf, ip_str, disc->aid);
            } else {
                ESP_LOGW(WIFI_CONSOLE_TAG, "\033[33mSoftAP: cliente desligado (dados indisponíveis).\033[0m");
            }
            break;
        }
        case WIFI_EVENT_AP_STOP:
            log_warn(TAG, "SoftAP foi desligado.");
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(WIFI_CONSOLE_TAG, "Wi-Fi ligado - IP: %s", ip_str);
            start_time_sync();
            system_status_set_wifi_connected(true);
            system_status_set_wifi_ip(ip_str);
            update_wifi_rssi();
        } else if (event_id == IP_EVENT_AP_STAIPASSIGNED) {
            ip_event_ap_staipassigned_t *assigned = (ip_event_ap_staipassigned_t *)event_data;
            if (assigned) {
                char mac_buf[18];
                snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                         assigned->mac[0], assigned->mac[1], assigned->mac[2],
                         assigned->mac[3], assigned->mac[4], assigned->mac[5]);
                softap_client_entry_t *entry = softap_client_alloc(assigned->mac);
                uint16_t aid = 0;
                if (entry) {
                    entry->ip = assigned->ip;
                    aid = entry->aid;
                }
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&assigned->ip));
                log_info(TAG, "SoftAP: dispositivo entrou (MAC=%s IP=%s AID=%d).", mac_buf, ip_str, aid);
            }
        }
    }
}

// Inicia o cliente SNTP assim que existe conectividade STA.
// Chamado pelo handler Wi-Fi para sincronizar o RTC.
static void start_time_sync(void)
{
    if (s_time_sync_started) {
        return;
    }

    setenv("TZ", "WET0WEST,M3.5.0/1,M10.5.0", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = time_sync_notification_cb;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_time_sync_started = true;
        log_info(TAG, "SNTP iniciado; a sincronizar hora.");
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_time_sync_started = true;
        log_warn(TAG, "SNTP já se encontra inicializado.");
    } else {
        log_error(TAG, "Falha ao iniciar SNTP (%s)", esp_err_to_name(err));
    }
}

// Callback do SNTP que marca o system_status como sincronizado.
// Chamada pelo IDF quando o relógio é ajustado.
static void time_sync_notification_cb(struct timeval *tv)
{
    s_time_synced = true;

    time_t now = tv ? tv->tv_sec : 0;
    struct tm timeinfo;
    char buffer[32];

    if (now == 0) {
        time(&now);
    }

    if (localtime_r(&now, &timeinfo)) {
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        log_info(TAG, "Hora sincronizada: %s", buffer);
    } else {
        log_warn(TAG, "Hora sincronizada mas não foi possível formatar data.");
    }
}

// Configura o Wi-Fi STA/AP com as credenciais dadas e arranca o driver.
// Utilizado tanto no boot como quando o SD regressa após uma falha.
esp_err_t wifi_connect_with_credentials(const char *ssid, const char *pass)
{
    // Verifica se temos credenciais válidas para ativar modo Station (cliente)
    bool have_sta = (ssid && pass && ssid[0] != '\0' && pass[0] != '\0');

    // Garante que o NVS está inicializado (necessário para guardar configuração Wi-Fi)
    ESP_RETURN_ON_ERROR(ensure_nvs_ready(), TAG, "NVS necessário para Wi-Fi.");
    
    // Garante que o event loop e netifs estão prontos
    ESP_RETURN_ON_ERROR(ensure_event_loop_ready(), TAG, "Falha ao preparar stack de rede.");
    
    // Configura a interface de rede do SoftAP (IP, DHCP server, etc.)
    ESP_RETURN_ON_ERROR(configure_softap_netif(), TAG, "Falha ao configurar o SoftAP.");

    // Inicializa o driver Wi-Fi do ESP32 com configuração padrão
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        // Wi-Fi já estava inicializado (reinicialização), não é erro
        err = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "Falha ao inicializar driver Wi-Fi.");

    // Define o modo de operação: APSTA (cliente+AP) ou apenas AP
    wifi_mode_t desired_mode = have_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(desired_mode), TAG, "Não foi possível definir modo Wi-Fi.");

    // Prepara a configuração do Access Point (permite que outros dispositivos se liguem)
    wifi_config_t ap_config = { 0 };
    strlcpy((char *)ap_config.ap.ssid, SOFTAP_DEFAULT_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(SOFTAP_DEFAULT_SSID);
    strlcpy((char *)ap_config.ap.password, SOFTAP_DEFAULT_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.channel = SOFTAP_DEFAULT_CHANNEL;      // Canal Wi-Fi 6
    ap_config.ap.max_connection = 4;                    // Máximo 4 clientes simultâneos
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;         // Segurança WPA2
    ap_config.ap.pmf_cfg.required = false;              // PMF opcional
    ap_config.ap.pmf_cfg.capable = true;

    // Aplica a configuração do SoftAP ao driver
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "Falha ao aplicar config SoftAP.");
    log_info(TAG, "SoftAP \"%s\" configurado com password predefinida.", SOFTAP_DEFAULT_SSID);

    // Se temos credenciais válidas, configura também o modo Station para ligar à rede
    if (have_sta) {
        wifi_config_t wifi_config = { 0 };
        strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;  // Mínimo WPA2
        
        // Guarda o SSID para logging posterior
        strlcpy(s_wifi_ssid, (const char *)wifi_config.sta.ssid, sizeof(s_wifi_ssid));
        
        // Aplica a configuração da Station ao driver
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "Falha ao aplicar config STA.");
    } else {
        // Sem credenciais: limpa o SSID guardado
        s_wifi_ssid[0] = '\0';
    }

    // Inicia o driver Wi-Fi (ativa as interfaces configuradas)
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Não foi possível arrancar o Wi-Fi.");
    err = ESP_OK;

    // Se o modo Station está ativo, tenta ligar à rede configurada
    if (have_sta) {
        err = esp_wifi_connect();
        if (err == ESP_ERR_WIFI_CONN) {
            // Erro esperado durante tentativa inicial, será retentado automaticamente
            err = ESP_OK;
        } else if (err != ESP_OK) {
            // Outro erro inesperado ao tentar conectar
            ESP_LOGE(WIFI_CONSOLE_TAG, "Wi-Fi não iniciou (%s).", esp_err_to_name(err));
        }
    } else {
        // Sem credenciais: apenas SoftAP está disponível (sem internet)
        system_status_set_wifi_connected(false);
        log_warn(TAG, "STA desativada; SoftAP disponível apenas na rede local.");
    }

    return err;
}

// Verifica limites de tamanho e caracteres ASCII antes de aceitar strings externas.
// Reutilizado na validação de SSID, password e URIs.
static bool validate_ascii_string(const char *value, size_t min_len, size_t max_len)
{
    // Verifica se o ponteiro é válido
    if (!value) {
        return false;
    }

    // Calcula o comprimento da string
    size_t len = strlen(value);
    
    // Verifica se está dentro dos limites permitidos
    if (len < min_len || len > max_len) {
        return false;
    }

    // Verifica se todos os caracteres são ASCII imprimível (sem controlo)
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (!isprint(ch)) {
            // Encontrou caractere não-imprimível (controlo, null, etc.)
            return false;
        }
    }

    // String válida: tamanho correto e apenas caracteres seguros
    return true;
}
// assim conseguimos dar debug mais facilmente
// Converte códigos de desconexão Wi-Fi em texto legível para logging.
// Chamado dentro do handler de eventos STA.
static const char *wifi_reason_to_string(uint8_t reason)
{
    // Mapeia códigos numéricos de erro do ESP-IDF para mensagens compreensíveis
    switch (reason) {
    // Erros relacionados com autenticação (password incorreta)
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "WRONG_PASSWORD";
    
    // Access Point não encontrado (SSID não existe ou está fora de alcance)
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    
    // Falha na associação (AP pode estar cheio ou a rejeitar ligações)
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "ASSOC_FAIL";
    
    // Perda de sinal (AP deixou de responder)
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    
    // Timeout durante handshake de ligação
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    
    // Código de erro desconhecido ou não mapeado
    default:
        return "UNKNOWN";
    }
}

// Obtém o RSSI atual da STA e atualiza o system_status.
// Chamado durante eventos de ligação para alimentar o dashboard.
static void update_wifi_rssi(void)
{
    // Estrutura para receber informações do Access Point conectado
    wifi_ap_record_t ap_info;
    
    // Consulta informações do AP (inclui RSSI - força do sinal)
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        // Atualiza o módulo de estado com o RSSI atual (em dBm)
        // Valores típicos: -30 dBm (excelente) a -90 dBm (fraco)
        system_status_set_wifi_rssi(ap_info.rssi);
    }
}
