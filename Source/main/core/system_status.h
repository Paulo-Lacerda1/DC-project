#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SYSTEM_STATUS_BROKER_HOST_MAX_LEN   64U

typedef struct {
    bool sta_connected;
    char ip_addr[48];
    int rssi;
} system_status_wifi_t;

typedef struct {
    bool connected;
    uint64_t last_attempt_ts;
    uint32_t fail_count;
    uint32_t messages_sent;
    uint32_t messages_buffered;
    char broker_host[SYSTEM_STATUS_BROKER_HOST_MAX_LEN];
} system_status_mqtt_t;

typedef struct {
    bool mounted;
    uint64_t free_space_bytes;
    uint32_t log_entries;
    uint64_t last_log_ts;
    uint32_t data_entries;
    uint64_t data_last_log_ts;
    bool data_logging_enabled;
} system_status_sd_t;

typedef struct {
    uint64_t uptime_seconds;
    char firmware_version[64];
    char device_uid[64];
} system_status_general_t;

typedef struct {
    system_status_wifi_t wifi;
    system_status_mqtt_t mqtt;
    system_status_sd_t sd;
    system_status_general_t general;
    bool standby_active;
} system_status_snapshot_t;

void system_status_init(void);

void system_status_set_firmware_info(const char *version, const char *device_uid);

void system_status_set_wifi_connected(bool connected);
void system_status_set_wifi_ip(const char *ip_addr);
void system_status_set_wifi_rssi(int rssi_dbm);

void system_status_mark_mqtt_attempt(void);
void system_status_set_mqtt_connected(bool connected);
bool system_status_is_mqtt_connected(void);
void system_status_increment_mqtt_fail(void);
void system_status_increment_mqtt_sent(void);
void system_status_set_mqtt_buffered(uint32_t buffered);
void system_status_set_mqtt_broker_host(const char *host);

void system_status_set_sd_state(bool mounted, uint64_t free_space_bytes);
void system_status_note_log_write(void);
void system_status_note_data_log_write(void);
void system_status_reset_data_log_stats(void);
void system_status_set_data_logging_enabled(bool enabled);

void system_status_snapshot(system_status_snapshot_t *out);

void system_status_set_standby_active(bool active);
bool system_status_is_standby_active(void);
