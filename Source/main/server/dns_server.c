#include "dns_server.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_netif_ip_addr.h"

#define DNS_SERVER_PORT 53
#define DNS_MAX_PACKET_SIZE 512
#define DNS_TASK_STACK 4096
#define DNS_TASK_PRIORITY 4

typedef struct {
    const char *name;
    uint32_t ip_addr; // network byte order
} dns_mapping_t;

static const char *TAG = "dns_server";

static const dns_mapping_t s_dns_mappings[] = {
    { "dcdashboard.pt", ESP_IP4TOADDR(192, 168, 4, 1) },
    { "dcconfiguracao.pt", ESP_IP4TOADDR(10, 101, 122, 21) },
};

static TaskHandle_t s_dns_task = NULL;
static int s_dns_sock = -1;
static volatile bool s_dns_running = false;

static bool dns_parse_qname(const uint8_t *packet, size_t len, size_t offset,
                            char *out, size_t out_len, size_t *out_next)
{
    size_t pos = offset;
    size_t out_pos = 0;

    while (pos < len) {
        uint8_t label_len = packet[pos++];
        if (label_len == 0) {
            if (out_pos < out_len) {
                out[out_pos] = '\0';
            }
            if (out_next) {
                *out_next = pos;
            }
            return true;
        }

        if ((label_len & 0xC0) != 0) {
            // Não suportamos compressão no QNAME (pouco comum em queries).
            return false;
        }

        if (pos + label_len > len) {
            return false;
        }

        if (out_pos && out_pos < out_len - 1) {
            out[out_pos++] = '.';
        }

        for (uint8_t i = 0; i < label_len; ++i) {
            if (out_pos < out_len - 1) {
                out[out_pos++] = (char)tolower((unsigned char)packet[pos]);
            }
            pos++;
        }
    }

    return false;
}

static const dns_mapping_t *dns_find_mapping(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(s_dns_mappings) / sizeof(s_dns_mappings[0]); ++i) {
        if (strcasecmp(name, s_dns_mappings[i].name) == 0) {
            return &s_dns_mappings[i];
        }
    }
    return NULL;
}

static size_t dns_build_response(const uint8_t *query, size_t query_len,
                                 size_t question_end, uint16_t req_flags,
                                 bool answer_found, uint32_t ip_addr,
                                 bool nxdomain, uint8_t *out, size_t out_len)
{
    if (question_end > query_len || question_end > out_len) {
        return 0;
    }

    memcpy(out, query, question_end);

    uint16_t resp_flags = 0x8000 | 0x0400; // QR + AA
    if (req_flags & 0x0100) {
        resp_flags |= 0x0100; // RD
    }
    if (nxdomain) {
        resp_flags |= 0x0003; // RCODE = NXDOMAIN
    }

    out[2] = (uint8_t)(resp_flags >> 8);
    out[3] = (uint8_t)(resp_flags & 0xFF);

    // QDCOUNT = 1
    out[4] = 0x00;
    out[5] = 0x01;

    // ANCOUNT
    if (answer_found && !nxdomain) {
        out[6] = 0x00;
        out[7] = 0x01;
    } else {
        out[6] = 0x00;
        out[7] = 0x00;
    }

    // NSCOUNT, ARCOUNT = 0
    out[8] = 0x00;
    out[9] = 0x00;
    out[10] = 0x00;
    out[11] = 0x00;

    size_t resp_len = question_end;

    if (answer_found && !nxdomain) {
        if (resp_len + 16 > out_len) {
            return 0;
        }

        // NAME: pointer para o QNAME (offset 12)
        out[resp_len++] = 0xC0;
        out[resp_len++] = 0x0C;

        // TYPE A
        out[resp_len++] = 0x00;
        out[resp_len++] = 0x01;

        // CLASS IN
        out[resp_len++] = 0x00;
        out[resp_len++] = 0x01;

        // TTL = 60s
        out[resp_len++] = 0x00;
        out[resp_len++] = 0x00;
        out[resp_len++] = 0x00;
        out[resp_len++] = 0x3C;

        // RDLENGTH = 4
        out[resp_len++] = 0x00;
        out[resp_len++] = 0x04;

        // RDATA = IP
        memcpy(&out[resp_len], &ip_addr, sizeof(ip_addr));
        resp_len += sizeof(ip_addr);
    }

    return resp_len;
}

static void dns_server_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket DNS (%d).", errno);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Falha ao fazer bind do DNS (%d).", errno);
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_dns_sock = sock;
    s_dns_running = true;
    ESP_LOGI(TAG, "DNS iniciado (porta %d).", DNS_SERVER_PORT);

    uint8_t rx_buf[DNS_MAX_PACKET_SIZE];
    uint8_t tx_buf[DNS_MAX_PACKET_SIZE];

    while (s_dns_running) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        ssize_t len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            if (!s_dns_running) {
                break;
            }
            continue;
        }

        if (len < 12) {
            continue;
        }

        uint16_t qdcount = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];
        if (qdcount == 0) {
            continue;
        }

        uint16_t req_flags = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];

        char qname[256];
        size_t qname_end = 0;
        if (!dns_parse_qname(rx_buf, (size_t)len, 12, qname, sizeof(qname), &qname_end)) {
            continue;
        }

        if (qname_end + 4 > (size_t)len) {
            continue;
        }

        uint16_t qtype = ((uint16_t)rx_buf[qname_end] << 8) | rx_buf[qname_end + 1];
        uint16_t qclass = ((uint16_t)rx_buf[qname_end + 2] << 8) | rx_buf[qname_end + 3];
        size_t question_end = qname_end + 4;

        bool supported = (qtype == 1 && qclass == 1);
        const dns_mapping_t *mapping = supported ? dns_find_mapping(qname) : NULL;

        bool answer_found = mapping != NULL;
        bool nxdomain = supported && !answer_found;
        uint32_t ip_addr = answer_found ? mapping->ip_addr : 0;

        size_t resp_len = dns_build_response(rx_buf, (size_t)len, question_end, req_flags,
                                             answer_found, ip_addr, nxdomain, tx_buf, sizeof(tx_buf));
        if (resp_len == 0) {
            continue;
        }

        sendto(sock, tx_buf, resp_len, 0, (struct sockaddr *)&from, from_len);
    }

    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }

    s_dns_running = false;
    s_dns_task = NULL;
    ESP_LOGI(TAG, "DNS parado.");
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    if (s_dns_task) {
        return;
    }

    BaseType_t created = xTaskCreate(dns_server_task, "dns_server", DNS_TASK_STACK,
                                     NULL, DNS_TASK_PRIORITY, &s_dns_task);
    if (created != pdPASS) {
        s_dns_task = NULL;
        ESP_LOGE(TAG, "Falha ao criar task DNS.");
    }
}

void dns_server_stop(void)
{
    if (!s_dns_task) {
        return;
    }

    s_dns_running = false;

    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);
        close(s_dns_sock);
        s_dns_sock = -1;
    }
}
