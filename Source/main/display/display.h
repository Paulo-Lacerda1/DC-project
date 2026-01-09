#pragma once

#include <stdbool.h>
#include "esp_err.h"


typedef enum {
    SCREEN_MAIN = 0,        // Tabela principal
    SCREEN_LOGS,            // Logs recentes
    SCREEN_GRAPH_TEMP_HUM,  // Gráfico temp/hum
    SCREEN_MAX              // Total de ecrãs
} display_screen_t;

#define DISPLAY_LOG_LINE_MAX_LEN 64

esp_err_t display_init(void);

// Desenha a tabela principal; em standby, mostra "STOP" nas duas primeiras linhas.
void display_draw_table(float temp, float hum, bool sensor_ok, bool standby_active);

void display_draw_logs(void);
void display_draw_graph_temp_hum(void);

void display_clear(void);
void display_show_sd_removed(void);
void display_show_welcome_message(void);

void display_set_screen(display_screen_t screen);
display_screen_t display_get_screen(void);

void display_add_log(const char *msg);
void display_store_measurement(float temp, float hum);
