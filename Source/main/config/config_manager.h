// Interface pública do gestor de configuração/SD/Wi-Fi.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define CONFIG_MANAGER_PERIOD_MIN_S     1U
#define CONFIG_MANAGER_PERIOD_MAX_S     3600U
#define CONFIG_MANAGER_PERIOD_MIN_MS    (CONFIG_MANAGER_PERIOD_MIN_S * 1000U)
#define CONFIG_MANAGER_PERIOD_MAX_MS    (CONFIG_MANAGER_PERIOD_MAX_S * 1000U)
#define CONFIG_MANAGER_SOFTAP_MAX_CLIENTS   5U
#define CONFIG_MANAGER_WIFI_SSID_MAX_LEN    32U
#define CONFIG_MANAGER_WIFI_PASS_MAX_LEN    64U
#define CONFIG_MANAGER_WIFI_PASS_MIN_LEN    8U

extern uint32_t leitura_periodo_ms;

typedef struct {
    uint32_t periodo_leitura_ms;
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_uri[128];
    char mqtt_topic[64];
    char mqtt_topic_events[64];
    char mqtt_client_id[64];
    char mqtt_username[64];
    char mqtt_password[64];
} config_user_t;

extern config_user_t g_config_user;

typedef struct {
    char mac[18];
    char ip[16];
    uint16_t aid;
    bool ip_assigned;
} config_softap_client_info_t;

/* Inicializa o cartão SD e garante que config_user.json está acessível. */
esp_err_t config_manager_init(void);

/* Grava config_user.json com o novo período. */
esp_err_t save_config_to_json(uint32_t period_ms);

/* Recarrega config_user.json do SD durante a execução. */
esp_err_t config_manager_reload_user_config(void);

/* Obtém o período atual em ms de forma segura. */
uint32_t config_manager_get_period_ms(void);

/* Atualiza o período (segundos) em RAM+SD com validação. */
esp_err_t config_manager_set_period_seconds(uint32_t period_seconds);

/* Monta o cartão microSD */
esp_err_t config_manager_mount_sd(void);

/* Indica se o cartão microSD está atualmente montado/disponível. */
bool config_manager_is_sd_available(void);

/* Valida periodicamente se o SD ainda responde; desmonta-o e devolve false em falha. */
bool config_manager_check_sd_alive(void);

/* Indica se o sistema deve reiniciar quando o SD voltar a ficar disponível. */
bool config_manager_should_restart_on_sd_return(void);

/* Devolve e limpa um erro pendente de montagem do SD (se existir). */
bool config_manager_pop_pending_sd_error(char *buf, size_t buf_size);

/* Marca o SD como indisponível e desmonta (usar em erros fatais de I/O). */
void mark_sd_card_unavailable(const char *reason);

/* Lê config.json para memória (aloca buffer dinamicamente). */
esp_err_t config_manager_read_wifi_file(char **buffer_out);

/* Obtém o UID do dispositivo a partir de config_manufacturer.json. */
esp_err_t config_manager_get_device_uid(char *uid_out, size_t uid_size);

/* Carrega os campos wifi_ssid/wifi_pass de config_user.json. */
bool load_wifi_config(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len);

/* Configura o Wi-Fi STA e liga com as credenciais lidas do SD. */
esp_err_t wifi_connect_with_credentials(const char *ssid, const char *pass);

/* Copia até max_entries clientes SoftAP ativos; retorna o número total encontrado. */
size_t config_manager_get_softap_clients(config_softap_client_info_t *out_array, size_t max_entries);

/* Valida as credenciais fornecidas (ASCII + tamanhos). */
esp_err_t config_manager_check_wifi_credentials(const char *ssid, const char *pass);

/* Grava novas credenciais Wi-Fi em config_user.json e atualiza g_config_user. */
esp_err_t config_manager_save_wifi_credentials(const char *ssid, const char *pass);

/* Aplica credenciais na interface STA e força reconexão. */
esp_err_t config_manager_apply_sta_credentials(const char *ssid, const char *pass);

/* Indica se existem credenciais gravadas (SSID não vazio). */
bool config_manager_has_saved_wifi_credentials(void);
