// Implementa o servidor HTTPS e as APIs usadas pelo dashboard seguro.
// Pertence ao módulo de conectividade que expõe estado e controlo remoto.
#include "https_server_app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "sensor_data_store.h"
#include "config_manager.h"
#include "task_sensor.h"
#include "logging.h"
#include "system_status.h"
#include "cJSON.h"
#include "config_runtime.h"

static const char *TAG = "https_server_app";

static httpd_handle_t s_server = NULL;
static bool s_handlers_registered = false;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static void maybe_start_server(void);
static void log_period_change(long new_period_s, httpd_req_t *req);
static esp_err_t send_index_page(httpd_req_t *req);

// Callback de erros do HTTPS server (TLS/socket).
// Registado no init para que falhas TLS sejam reportadas no log.
static void https_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base != ESP_HTTPS_SERVER_EVENT || event_id != HTTPS_SERVER_EVENT_ERROR) {
        return;
    }

    esp_https_server_last_error_t *last_error = (esp_https_server_last_error_t *)event_data;
    ESP_LOGE(TAG, "Erro no HTTPS server: last_error=%s tls=%d flags=%d",
             esp_err_to_name(last_error->last_error),
             last_error->esp_tls_error_code,
             last_error->esp_tls_flags);
}

// Extrai IP do cliente a partir do socket da request.
// Necessário para restringir endpoints a STA ou SoftAP.
static bool get_request_ip(httpd_req_t *req, char *ip_out, size_t ip_len)
{
    if (!req || !ip_out || ip_len == 0) {
        return false;
    }

    int sock = httpd_req_to_sockfd(req);
    if (sock < 0) {
        return false;
    }

    struct sockaddr_storage peer = {0};
    socklen_t addr_len = sizeof(peer);
    if (getpeername(sock, (struct sockaddr *)&peer, &addr_len) != 0) {
        return false;
    }

    if (peer.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&peer;
        return inet_ntop(AF_INET, &in->sin_addr, ip_out, ip_len) != NULL;
    }
#if defined(AF_INET6)
    if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&peer;
        return inet_ntop(AF_INET6, &in6->sin6_addr, ip_out, ip_len) != NULL;
    }
#endif
    return false;
}

// Permite filtrar pedidos vindos do SoftAP (restrições de acesso).
// Usado por handlers que só devem servir clientes autenticados via STA.
static bool request_from_softap(httpd_req_t *req)
{
    char ip_buf[48] = {0};
    if (!get_request_ip(req, ip_buf, sizeof(ip_buf))) {
        return false;
    }
    return strncmp(ip_buf, "192.168.4.", 10) == 0;
}

// Resposta 403 para endpoints que só devem servir a STA.
// Evita que clientes ligados ao SoftAP alterem configuração sensível.
static esp_err_t send_sta_only_response(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "Dashboard disponível apenas através da interface STA.");
}

// Renderiza index.html e injeta o estado do toggle SD antes de enviar.
// Chamado apenas quando o cliente STA acede ao dashboard principal.
static esp_err_t send_index_page(httpd_req_t *req)
{
    const char *placeholder = "{{ENABLE_SD_LOGGING_CHECKED}}";
    const size_t placeholder_len = strlen(placeholder);
    const char *html = (const char *)index_html_start;
    const size_t html_size = (size_t)(index_html_end - index_html_start);
    size_t match_offset = 0;
    bool found = false;

    if (placeholder_len > 0 && html_size >= placeholder_len) {
        for (size_t i = 0; i + placeholder_len <= html_size; ++i) {
            if (memcmp(html + i, placeholder, placeholder_len) == 0) {
                match_offset = i;
                found = true;
                break;
            }
        }
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!found) {
        return httpd_resp_send(req, html, html_size);
    }

    const char *checked = config_runtime_get_sd_logging_enabled() ? "checked" : "";
    const size_t checked_len = strlen(checked);
    const size_t out_len = html_size - placeholder_len + checked_len;

    char *buffer = (char *)malloc(out_len + 1);
    if (!buffer) {
        return httpd_resp_send(req, html, html_size);
    }

    const size_t prefix_len = match_offset;
    const size_t suffix_offset = match_offset + placeholder_len;
    const size_t suffix_len = html_size - suffix_offset;

    memcpy(buffer, html, prefix_len);
    if (checked_len > 0) {
        memcpy(buffer + prefix_len, checked, checked_len);
    }
    memcpy(buffer + prefix_len + checked_len, html + suffix_offset, suffix_len);
    buffer[out_len] = '\0';

    esp_err_t res = httpd_resp_send(req, buffer, out_len);
    free(buffer);
    return res;
}

// GET "/" → dashboard principal que só deve ser acedido via STA.
// Handler registado como URI raiz do servidor HTTPS.
static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (request_from_softap(req)) {
        return send_sta_only_response(req);
    }

    return send_index_page(req);
}

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
};

// API de leituras atuais do sensor e período configurado.
// Invocada pelo dashboard para atualizar gráficos em tempo real.
static esp_err_t sensor_data_get_handler(httpd_req_t *req)
{
    if (request_from_softap(req)) {
        return send_sta_only_response(req);
    }

    sensor_data_t data = {0};
    bool has_data = sensor_data_store_get(&data);
    uint32_t period_ms = config_manager_get_period_ms();
    char json[160];
    int len;

    if (has_data) {
        len = snprintf(json, sizeof(json),
                       "{\"temperature\":%.2f,\"humidity\":%.2f,"
                       "\"hasData\":true,\"periodMs\":%lu}",
                       data.temperature, data.humidity,
                       (unsigned long)period_ms);
    } else {
        len = snprintf(json, sizeof(json),
                       "{\"hasData\":false,\"periodMs\":%lu}",
                       (unsigned long)period_ms);
    }
    if (len < 0 || len >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Expõe snapshot completo do estado (Wi-Fi, MQTT, SD, SoftAP clients).
// Endpoint consumido pelo dashboard para preencher cartões de estado.
static esp_err_t system_status_get_handler(httpd_req_t *req)
{
    if (request_from_softap(req)) {
        return send_sta_only_response(req);
    }

    system_status_snapshot_t snapshot = {0};
    system_status_snapshot(&snapshot);
    config_softap_client_info_t softap_clients[CONFIG_MANAGER_SOFTAP_MAX_CLIENTS] = {0};
    size_t softap_total = config_manager_get_softap_clients(softap_clients,
                                                            CONFIG_MANAGER_SOFTAP_MAX_CLIENTS);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    if (wifi) {
        cJSON_AddBoolToObject(wifi, "sta_connected", snapshot.wifi.sta_connected);
        cJSON_AddStringToObject(wifi, "ip_addr", snapshot.wifi.ip_addr);
        cJSON_AddNumberToObject(wifi, "rssi", snapshot.wifi.rssi);
    }

    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    if (mqtt) {
        cJSON_AddBoolToObject(mqtt, "connected", snapshot.mqtt.connected);
        cJSON_AddNumberToObject(mqtt, "last_attempt_ts", (double)snapshot.mqtt.last_attempt_ts);
        cJSON_AddNumberToObject(mqtt, "fail_count", snapshot.mqtt.fail_count);
        cJSON_AddNumberToObject(mqtt, "messages_sent", snapshot.mqtt.messages_sent);
        cJSON_AddNumberToObject(mqtt, "messages_buffered", snapshot.mqtt.messages_buffered);
        cJSON_AddStringToObject(mqtt, "broker_host", snapshot.mqtt.broker_host);
    }

    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    if (sd) {
        cJSON_AddBoolToObject(sd, "mounted", snapshot.sd.mounted);
        cJSON_AddNumberToObject(sd, "free_space_bytes", (double)snapshot.sd.free_space_bytes);
        cJSON_AddNumberToObject(sd, "log_entries", snapshot.sd.log_entries);
        cJSON_AddNumberToObject(sd, "last_log_ts", (double)snapshot.sd.last_log_ts);
        cJSON_AddNumberToObject(sd, "data_entries", snapshot.sd.data_entries);
        cJSON_AddNumberToObject(sd, "data_last_log_ts", (double)snapshot.sd.data_last_log_ts);
        cJSON_AddBoolToObject(sd, "data_logging_enabled", snapshot.sd.data_logging_enabled);
    }

    cJSON *general = cJSON_AddObjectToObject(root, "general");
    if (general) {
        cJSON_AddNumberToObject(general, "uptime_seconds", (double)snapshot.general.uptime_seconds);
        cJSON_AddStringToObject(general, "firmware_version", snapshot.general.firmware_version);
        cJSON_AddStringToObject(general, "device_uid", snapshot.general.device_uid);
    }

    cJSON *softap = cJSON_AddObjectToObject(root, "softap");
    if (softap) {
        cJSON_AddNumberToObject(softap, "client_count", (double)softap_total);
        cJSON *arr = cJSON_AddArrayToObject(softap, "clients");
        if (arr) {
            size_t emit = softap_total;
            if (emit > CONFIG_MANAGER_SOFTAP_MAX_CLIENTS) {
                emit = CONFIG_MANAGER_SOFTAP_MAX_CLIENTS;
            }
            for (size_t i = 0; i < emit; ++i) {
                cJSON *item = cJSON_CreateObject();
                if (!item) {
                    continue;
                }
                cJSON_AddStringToObject(item, "ip", softap_clients[i].ip);
                cJSON_AddNumberToObject(item, "aid", (double)softap_clients[i].aid);
                cJSON_AddBoolToObject(item, "ip_assigned", softap_clients[i].ip_assigned);
                cJSON_AddItemToArray(arr, item);
            }
        }
    }

    char *json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_text) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, json_text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_text);
    return res;
}

static const httpd_uri_t sensor_data_uri = {
    .uri = "/api/sensor",
    .method = HTTP_GET,
    .handler = sensor_data_get_handler,
};

static const httpd_uri_t status_uri = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = system_status_get_handler,
};

// Resolve IP/MAC do cliente para logging de alterações.
// Usado pelos endpoints que alteram configuração para auditoria.
static void resolve_client_info(httpd_req_t *req,
                                char *ip_out,
                                size_t ip_len,
                                char *mac_out,
                                size_t mac_len)
{
    if (ip_out && ip_len > 0) {
        strlcpy(ip_out, "desconhecido", ip_len);
    }
    if (mac_out && mac_len > 0) {
        strlcpy(mac_out, "desconhecido", mac_len);
    }

    if (ip_out && ip_len > 0) {
        get_request_ip(req, ip_out, ip_len);
    }

    wifi_sta_list_t wifi_list = {0};
    if (esp_wifi_ap_get_sta_list(&wifi_list) == ESP_OK && wifi_list.num >= 1) {
        const uint8_t *mac = wifi_list.sta[0].mac;
        if (mac_out && mac_len >= 18) {
            snprintf(mac_out,
                     mac_len,
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
}

// Determina se o cliente espera JSON (fetch/AJAX) ou fallback em HTML.
// Permite reutilizar handlers para API e UI tradicional.
static bool request_wants_json(httpd_req_t *req)
{
    char header[32];
    if (httpd_req_get_hdr_value_str(req, "X-Requested-With", header, sizeof(header)) == ESP_OK) {
        if (strcasecmp(header, "fetch") == 0) {
            return true;
        }
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", header, sizeof(header)) == ESP_OK) {
        if (strstr(header, "application/json")) {
            return true;
        }
    }
    return false;
}

// Renderiza uma resposta HTML simples para feedback dos POSTs.
// Chamado quando o cliente não indicou interesse em JSON.
static esp_err_t send_feedback_page(httpd_req_t *req,
                                    const char *status,
                                    const char *title,
                                    const char *message);

// Resposta padrão JSON para sucesso/erro.
// Usada pelos POST handlers quando o pedido vem de fetch().
static esp_err_t send_feedback_json(httpd_req_t *req,
                                    const char *status,
                                    const char *message,
                                    bool is_error)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char body[192];
    int len = 0;
    if (is_error) {
        len = snprintf(body, sizeof(body),
                       "{\"ok\":false,\"error\":\"%s\"}",
                       message ? message : "Erro");
    } else {
        len = snprintf(body, sizeof(body),
                       "{\"ok\":true,\"message\":\"%s\"}",
                       message ? message : "OK");
    }
    if (len < 0 || len >= (int)sizeof(body)) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Erro interno\"}");
    }
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// Gera uma entrada de log com IP/MAC quando alguém altera o período.
// Permite rastrear alterações feitas via dashboard seguro.
static void log_period_change(long new_period_s, httpd_req_t *req)
{
    char ip_str[48];
    char mac_str[32];
    resolve_client_info(req, ip_str, sizeof(ip_str), mac_str, sizeof(mac_str));

    log_info("CONFIG",
             "[CONFIG] Periodo de leitura alterado para %ld segundos via HTTPS (IP %s MAC %s)",
             new_period_s,
             ip_str,
             mac_str);
}

// Regista quem ativou/desativou o logging no SD.
// Usado no handler update_config_post_handler para auditoria.
static void log_sd_toggle_change(bool enabled, httpd_req_t *req)
{
    char ip_str[48];
    char mac_str[32];
    resolve_client_info(req, ip_str, sizeof(ip_str), mac_str, sizeof(mac_str));

    log_info("CONFIG",
             "SD logging %s (via HTTPS IP %s MAC %s)",
             enabled ? "ON" : "OFF",
             ip_str,
             mac_str);
}

static esp_err_t send_feedback_page(httpd_req_t *req,
                                    const char *status,
                                    const char *title,
                                    const char *message)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char body[256];
    int len = snprintf(body, sizeof(body),
                       "<!DOCTYPE html><html lang=\"pt\"><body>"
                       "<h3>%s</h3><p>%s</p>"
                       "<p><a href=\"/\">Voltar</a></p>"
                       "</body></html>",
                       title ? title : "",
                       message ? message : "");
    if (len < 0 || len >= (int)sizeof(body)) {
        return httpd_resp_sendstr(req, "Erro interno");
    }
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// POST /update_config → ativa/desativa logging no SD.
// Handler acionado pelo dashboard ao alternar o toggle de armazenamento.
static esp_err_t update_config_post_handler(httpd_req_t *req)
{
    if (request_from_softap(req)) {
        return send_sta_only_response(req);
    }

    if (req->content_len > 256) {
        if (request_wants_json(req)) {
            return send_feedback_json(req,
                                      "400 Bad Request",
                                      "Conteúdo demasiado grande.",
                                      true);
        }
        return send_feedback_page(req,
                                  "400 Bad Request",
                                  "Pedido inválido",
                                  "Conteúdo demasiado grande.");
    }

    char buffer[257] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (request_wants_json(req)) {
                return send_feedback_json(req,
                                          "500 Internal Server Error",
                                          "Erro ao ler pedido.",
                                          true);
            }
            return send_feedback_page(req,
                                      "500 Internal Server Error",
                                      "Erro ao ler pedido",
                                      "Não foi possível ler o corpo do POST.");
        }
        received += ret;
    }
    buffer[received] = '\0';

    bool enable_sd = false;
    if (strstr(buffer, "enable_sd_logging=") != NULL) {
        enable_sd = true;
    }

    config_runtime_set_sd_logging_enabled(enable_sd);
    system_status_set_data_logging_enabled(enable_sd);
    log_sd_toggle_change(enable_sd, req);

    const char *msg = enable_sd ? "Registo no SD ativado." : "Registo no SD desativado.";
    if (request_wants_json(req)) {
        return send_feedback_json(req,
                                  "200 OK",
                                  msg,
                                  false);
    }
    return send_feedback_page(req,
                              "200 OK",
                              "Configuração atualizada",
                              msg);
}

// POST /set_period → altera período de leitura do sensor.
// Invocado quando o utilizador submete novos intervalos via UI.
static esp_err_t set_period_post_handler(httpd_req_t *req)
{
    // Bloqueia acesso de clientes do SoftAP (só STA pode alterar período)
    if (request_from_softap(req)) {
        return send_sta_only_response(req);
    }

    // Valida tamanho do conteúdo (deve ter dados mas não ser excessivo)
    if (req->content_len == 0 || req->content_len >= 128) {
        if (request_wants_json(req)) {
            return send_feedback_json(req,
                                      "400 Bad Request",
                                      "Conteúdo ausente ou demasiado grande.",
                                      true);
        }
        return send_feedback_page(req,
                                  "400 Bad Request",
                                  "Pedido inválido",
                                  "Conteúdo ausente ou demasiado grande.");
    }

    // Buffer para receber o corpo do POST
    char buffer[128] = {0};
    size_t received = 0;

    // Lê o corpo completo do POST em chunks
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                // Timeout, tenta novamente
                continue;
            }
            // Erro ao receber corpo do POST via socket
            if (request_wants_json(req)) {
                return send_feedback_json(req,
                                          "500 Internal Server Error",
                                          "Não foi possível ler o corpo do POST.",
                                          true);
            }
            return send_feedback_page(req,
                                      "500 Internal Server Error",
                                      "Erro a ler pedido",
                                      "Não foi possível ler o corpo do POST.");
        }
        received += ret;
    }
    buffer[received] = '\0';

    // Procura o campo "period=" no corpo URL-encoded
    char *period_field = strstr(buffer, "period=");
    if (!period_field) {
        // Campo obrigatório em falta
        if (request_wants_json(req)) {
            return send_feedback_json(req,
                                      "400 Bad Request",
                                      "Campo 'period' em falta.",
                                      true);
        }
        return send_feedback_page(req,
                                  "400 Bad Request",
                                  "Pedido inválido",
                                  "Campo 'period' em falta.");
    }
    
    // Avança para o valor após "period="
    period_field += strlen("period=");
    
    // Termina a string no próximo parâmetro (se existir)
    char *amp = strchr(period_field, '&');
    if (amp) {
        *amp = '\0';
    }

    // Decodifica caracteres '+' para espaços (URL-encoding)
    for (char *p = period_field; *p; ++p) {
        if (*p == '+') {
            *p = ' ';
        }
    }

    // Converte string para número inteiro
    char *endptr = NULL;
    long new_period_s = strtol(period_field, &endptr, 10);
    if (period_field == endptr || (endptr && *endptr != '\0')) {
        // Conversão falhou ou caracteres inválidos
        if (request_wants_json(req)) {
            return send_feedback_json(req,
                                      "400 Bad Request",
                                      "Período não reconhecido.",
                                      true);
        }
        return send_feedback_page(req,
                                  "400 Bad Request",
                                  "Valor inválido",
                                  "Período não reconhecido.");
    }

    if (new_period_s < CONFIG_MANAGER_PERIOD_MIN_S || new_period_s > CONFIG_MANAGER_PERIOD_MAX_S) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "O período deve estar entre %u e %u segundos.",
                 CONFIG_MANAGER_PERIOD_MIN_S,
                 CONFIG_MANAGER_PERIOD_MAX_S);
        if (request_wants_json(req)) {
            return send_feedback_json(req,
                                      "400 Bad Request",
                                      msg,
                                      true);
        }
        return send_feedback_page(req,
                                  "400 Bad Request",
                                  "Valor fora do intervalo",
                                  msg);
    }

    esp_err_t save_status = config_manager_set_period_seconds((uint32_t)new_period_s);
    if (save_status != ESP_OK) {
        if (request_wants_json(req)) {
            return send_feedback_json(req,
                                      "500 Internal Server Error",
                                      "Não foi possível guardar o novo período.",
                                      true);
        }
        return send_feedback_page(req,
                                  "500 Internal Server Error",
                                  "Erro ao gravar",
                                  "Não foi possível guardar o novo período.");
    }

    log_period_change(new_period_s, req);
    esp_err_t notify_status = task_sensor_notify_period_update();
    if (notify_status != ESP_OK) {
        log_warn("CONFIG", "Não foi possível notificar a task do sensor (status=%d).", (int)notify_status);
    }

    char success_msg[160];
    snprintf(success_msg, sizeof(success_msg),
             "Período atualizado com sucesso para %ld s.", new_period_s);
    if (request_wants_json(req)) {
        return send_feedback_json(req,
                                  "200 OK",
                                  success_msg,
                                  false);
    }
    return send_feedback_page(req,
                              "200 OK",
                              "Configuração atualizada",
                              success_msg);
}

static const httpd_uri_t set_period_uri = {
    .uri = "/set_period",
    .method = HTTP_POST,
    .handler = set_period_post_handler,
};

static const httpd_uri_t update_config_uri = {
    .uri = "/update_config",
    .method = HTTP_POST,
    .handler = update_config_post_handler,
};

// Arranca o servidor HTTPS com certificados embebidos e regista endpoints.
// Chamado internamente quando a STA obtém IP para servir o dashboard.
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    extern const unsigned char servercert_pem_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_pem_end[] asm("_binary_servercert_pem_end");
    conf.servercert = servercert_pem_start;
    conf.servercert_len = servercert_pem_end - servercert_pem_start;

    extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS server não iniciou (%s)", esp_err_to_name(ret));
        return NULL;
    }

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &sensor_data_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &set_period_uri);
    httpd_register_uri_handler(server, &update_config_uri);
    return server;
}

// Garante que apenas uma instância do servidor HTTPS é criada.
// Invocado tanto pelo evento de IP como pelo init caso o STA já esteja ligado.
static void maybe_start_server(void)
{
    if (s_server) {
        return;
    }

    s_server = start_webserver();
    if (!s_server) {
        ESP_LOGE(TAG, "Arranque do servidor HTTPS falhou; será tentado novamente quando possível.");
    }
}

// Handler do evento IP_EVENT_STA_GOT_IP que dispara o arranque do servidor.
// Mantido separado para só arrancar o HTTPS quando há conectividade real.
static void sta_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        maybe_start_server();
    }
}

// Para o servidor HTTPS e liberta recursos TLS.
// Usado quando o SD é removido ou o sistema entra em modo OFF.
void https_server_app_stop(void)
{
    if (!s_server) {
        return;
    }

    httpd_ssl_stop(s_server);
    s_server = NULL;
}

// Regista handlers de eventos e lança o servidor se a STA já possui IP.
// Chamado durante o boot principal depois de configurar Wi-Fi.
void https_server_app_init(void)
{
    if (!s_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_SERVER_EVENT, ESP_EVENT_ANY_ID,
                                                   &https_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &sta_ip_event_handler, NULL));
        s_handlers_registered = true;
    }

    system_status_snapshot_t snapshot = {0};
    system_status_snapshot(&snapshot);
    if (snapshot.wifi.sta_connected) {
        maybe_start_server();
    }
}
