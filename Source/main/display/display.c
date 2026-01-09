// Todo o rendering de UI do TFT ST7735: dashboards, gráficos e logs.
// Pertence ao módulo de interface local do sistema.
#include "display.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "st7735.h"
#include "board_pins.h"

#define DASHBOARD_MARGIN_X 4
#define DASHBOARD_MARGIN_Y 4
#define BORDER_THICKNESS   2
#define LABEL_COLUMN_WIDTH      80
#define TEXT_PADDING            6
#define ROW_VALUE_TEXT_SIZE     1
#define ROW_LABEL_TEXT_SIZE     1
#define ROW_SENSOR_TEXT_SIZE    1
#define SENSOR_TEXT_SPACING     5
#define DEGREE_SYMBOL_GAP       2
#define DISPLAY_ROW_COUNT       3

#define LABEL_BG_COLOR ST7735_RED      
#define SENSOR_OFF_COLOR ST7735_BLUE   

#define TABLE_WIDTH  (ST7735_WIDTH - 2 * DASHBOARD_MARGIN_X)
#define TABLE_HEIGHT (ST7735_HEIGHT - 2 * DASHBOARD_MARGIN_Y)
#define ROW_HEIGHT   (TABLE_HEIGHT / DISPLAY_ROW_COUNT)
#define TABLE_X      DASHBOARD_MARGIN_X
#define TABLE_Y      DASHBOARD_MARGIN_Y
#define SEPARATOR_X  (TABLE_X + BORDER_THICKNESS + LABEL_COLUMN_WIDTH)
#define VALUE_AREA_X (SEPARATOR_X + BORDER_THICKNESS)

#define LOG_BUFFER_SIZE       50
#define LOG_LINE_MAX_LEN      DISPLAY_LOG_LINE_MAX_LEN
#define LOG_VISIBLE_LINES     5
#define LOG_LINE_HEIGHT       12

#define GRAPH_MAX_POINTS      30        //os x pontos mais recentes
#define GRAPH_MARGIN_LEFT     18
#define GRAPH_MARGIN_RIGHT    14
#define GRAPH_MARGIN_TOP      18
#define GRAPH_MARGIN_BOTTOM   14
#define GRAPH_AXIS_COLOR      ST7735_GRAY
#define GRAPH_BG_COLOR        ST7735_BLACK
#define GRAPH_TEMP_COLOR      ST7735_RED
#define GRAPH_HUM_COLOR       ST7735_CYAN
#define GRAPH_TEXT_COLOR      ST7735_WHITE
#define GRAPH_TEMP_MIN        0.0f
#define GRAPH_TEMP_MAX        50.0f
#define GRAPH_HUM_MIN         0.0f
#define GRAPH_HUM_MAX         100.0f

static const char *TAG = "display";

static bool s_display_ready = false;
static bool s_table_layout_ready = false;
static display_screen_t s_current_screen = SCREEN_MAIN;

static char s_log_buffer[LOG_BUFFER_SIZE][LOG_LINE_MAX_LEN];
static int s_log_write_index = 0;
static int s_log_count = 0;

static float s_graph_temp[GRAPH_MAX_POINTS];
static float s_graph_hum[GRAPH_MAX_POINTS];
static int s_graph_write_index = 0;
static int s_graph_count = 0;

static void draw_table_layout(void);
static void draw_row_label(uint8_t row, const char *label);
static void draw_row_value(uint8_t row, const char *value, uint16_t text_color,
                           uint16_t bg_color, uint8_t text_size, bool condensed,
                           bool with_degree_symbol, const char *suffix);
static void clear_row_value_area(uint8_t row, uint16_t bg_color);
static uint16_t draw_regular_string(uint16_t x, uint16_t y, const char *str,
                                    uint16_t color, uint16_t bg, uint8_t size);
static uint16_t draw_condensed_string(uint16_t x, uint16_t y, const char *str,
                                      uint16_t color, uint16_t bg, uint8_t size);
static void draw_degree_symbol(uint16_t x, uint16_t y, uint8_t size,
                               uint16_t color, uint16_t bg);
static void draw_line_segment(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                              uint16_t color);
static void draw_graph_axes(bool dual_axis);
static void draw_graph_series(const float *buffer, int start_index, int points,
                              uint16_t color, float min_val, float max_val);
static void draw_graph_latest_values(bool has_data, float temp, float hum);
static uint16_t graph_map_value_to_y(float value, float min_val, float max_val);
static int graph_points_to_draw(void);
static int graph_start_index(int points_to_draw);
static void sanitize_log_string(char *dst, const char *src, size_t max_len);


// Mostra uma animação de boas-vindas para contextualizar o utilizador.
// Chamado quando o display é inicializado antes de mostrar dados reais.
void display_show_welcome_message(void)
{
    const char *title = "BEM VINDO !";
    const char *line  = "Usa o botao para navegares";
    const char *arrow = "<----------|";

    uint8_t title_size = 2;
    uint8_t text_size  = 1;

    const int char_w = 6;
    const int char_h = 8;

    //Largura da frase principal
    int line_width = strlen(line) * char_w * text_size;
    int line_x = (ST7735_WIDTH - line_width) / 2;

    // X do meio da frase
    int mid_x = line_x + (line_width / 2);
    //Ecra limpo
    st7735_fill_rect(0, 0, ST7735_WIDTH, ST7735_HEIGHT, ST7735_BLACK);

    // TÍTULO 
    int title_width = strlen(title) * char_w * title_size;
    int title_x = (ST7735_WIDTH - title_width) / 2;

    st7735_draw_string(title_x, 15, title,
                       ST7735_WHITE, ST7735_BLACK, title_size);

    // FRASE PRINCIPAL 
    int y_line = 40;

    st7735_draw_string(line_x, y_line, line,
                       ST7735_WHITE, ST7735_BLACK, text_size);

    //BARRAS VERTICAIS 
    int bar_x  = mid_x - char_w / 2;

    int bar1_y = y_line + 13;   // 2 px extra de gap
    int bar2_y = bar1_y + char_h;     // 2ª barra logo por baixo

    st7735_draw_string(bar_x, bar1_y, "|",
                       ST7735_WHITE, ST7735_BLACK, text_size);

    st7735_draw_string(bar_x, bar2_y, "|",
                       ST7735_WHITE, ST7735_BLACK, text_size);

    // SETA 
    int arrow_len   = strlen(arrow);
    int arrow_end_x = bar_x;                          // posição do '|'
    int arrow_x     = arrow_end_x - (arrow_len - 1) * char_w;

    int arrow_y = bar2_y + 3;     // seta 3 px abaixo da 2ª barra

    st7735_draw_string(arrow_x, arrow_y, arrow,
                       ST7735_WHITE, ST7735_BLACK, text_size);
}

// Inicializa o driver ST7735 e desenha o layout base.
// Chamado uma vez no boot para preparar o ecrã antes das atualizações.
esp_err_t display_init(void)
{
    if (s_display_ready) {
        return ESP_OK;
    }

    st7735_config_t cfg = {
        .mosi_io_num = PIN_TFT_MOSI,
        .sclk_io_num = PIN_TFT_SCLK,
        .cs_io_num = PIN_TFT_CS,
        .dc_io_num = PIN_TFT_DC,
        .rst_io_num = PIN_TFT_RST,
        .bl_io_num = -1, // brilho controlado via PWM em task_brightness
        .host_id = SPI2_HOST,
        .reuse_spi_bus = true, // Reutiliza o barramento SPI já inicializado para o cartão SD
    };

    esp_err_t err = st7735_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar o ST7735: %s", esp_err_to_name(err));
        return err;
    }

    st7735_set_rotation(3);
    st7735_fill_screen(ST7735_BLACK);
    s_display_ready = true;
    s_table_layout_ready = false;
    draw_table_layout();
    return ESP_OK;
}

// Limpa o ecrã e força redesenho do layout na próxima atualização.
// Usado quando mudamos de ecrã ou precisamos eliminar artefactos.
void display_clear(void)
{
    if (!s_display_ready) {
        return;
    }
    st7735_fill_screen(ST7735_BLACK);
    s_table_layout_ready = false;
}

// Mostra as últimas linhas de log num layout dedicado.
// Chamado quando a task_display alterna para o ecrã de logs.
void display_draw_logs(void)
{
    if (!s_display_ready) {
        return;
    }

    st7735_fill_screen(ST7735_BLACK);
    st7735_draw_string(4, 2, "Logs recentes", ST7735_YELLOW, ST7735_BLACK, 1);
    st7735_fill_rect(2, 14, ST7735_WIDTH - 4, 1, ST7735_WHITE);

    int lines = (s_log_count < LOG_VISIBLE_LINES) ? s_log_count : LOG_VISIBLE_LINES;
    if (lines == 0) {
        st7735_draw_string(4, 26, "Sem logs", ST7735_WHITE, ST7735_BLACK, 1);
        return;
    }

    int start = s_log_write_index - lines;
    while (start < 0) {
        start += LOG_BUFFER_SIZE;
    }

    for (int i = 0; i < lines; ++i) {
        int idx = (start + i) % LOG_BUFFER_SIZE;
        uint16_t y = 20 + i * LOG_LINE_HEIGHT;
        st7735_draw_string(4, y, s_log_buffer[idx], ST7735_WHITE, ST7735_BLACK, 1);
    }
}

// Adiciona uma linha ao buffer circular usado pelo ecrã de logs.
// Chamado pelo logger sempre que ocorre um novo evento.
void display_add_log(const char *msg)
{
    if (!msg || msg[0] == '\0') {
        return;
    }

    char clean[LOG_LINE_MAX_LEN];
    sanitize_log_string(clean, msg, LOG_LINE_MAX_LEN);

    snprintf(s_log_buffer[s_log_write_index], LOG_LINE_MAX_LEN, "%s", clean);

    s_log_write_index = (s_log_write_index + 1) % LOG_BUFFER_SIZE;

    if (s_log_count < LOG_BUFFER_SIZE) {
        s_log_count++;
    }
}


// Remove caracteres não imprimíveis para evitar corromper o TFT.
// Utilizado antes de colocar textos no buffer de logs.
static void sanitize_log_string(char *dst, const char *src, size_t max_len)
{
    size_t i = 0;
    
    // Processa cada caractere da string de origem
    while (*src && i < max_len - 1) {
        unsigned char c = (unsigned char)*src;

        // Se for ASCII imprimível (espaço até ~), copia diretamente
        if (c >= 32 && c <= 126) {
            dst[i++] = c;
        }
        else {
            // Caractere especial ou acentuado, precisa substituir
            switch (c) {
                case 0xC3:  // UTF-8: prefixo para caracteres acentuados latinos
                    // Avança para o próximo byte que define o caractere específico
                    src++;
                    switch ((unsigned char)*src) {
                        case 0xA7: dst[i++] = 'c'; break;  // ç
                        case 0xA3: dst[i++] = 'a'; break;  // ã
                        case 0xB5: dst[i++] = 'o'; break;  // õ
                        case 0xA1: dst[i++] = 'a'; break;  // á
                        case 0xA9: dst[i++] = 'e'; break;  // é
                        case 0xAA: dst[i++] = 'e'; break;  // ê
                        case 0xB3: dst[i++] = 'o'; break;  // ó
                        case 0xAD: dst[i++] = 'i'; break;  // í
                        default:
                            // Caractere acentuado não reconhecido
                            dst[i++] = '?';
                            break;
                    }
                    break;

                default:
                    // Outro caractere não suportado
                    dst[i++] = '?';
                    break;
            }
        }

        // Avança para o próximo caractere de origem
        src++;
    }

    // Termina a string com null
    dst[i] = '\0';
}


// Armazena os últimos pontos usados para o gráfico rolling.
// Chamado pela task_display logo após receber nova leitura.
void display_store_measurement(float temp, float hum)
{
    s_graph_temp[s_graph_write_index] = temp;
    s_graph_hum[s_graph_write_index] = hum;
    s_graph_write_index = (s_graph_write_index + 1) % GRAPH_MAX_POINTS;
    if (s_graph_count < GRAPH_MAX_POINTS) {
        s_graph_count++;
    }
}

// Desenha o gráfico combinado de temperatura e humidade no ecrã gráfico.
// Usa os dados acumulados por display_store_measurement.
void display_draw_graph_temp_hum(void)
{
    if (!s_display_ready) {
        return;
    }

    st7735_fill_screen(GRAPH_BG_COLOR);
    draw_graph_axes(true);

    int points = graph_points_to_draw();
    if (points <= 0) {
        draw_graph_latest_values(false, 0.0f, 0.0f);
        st7735_draw_string(20, ST7735_HEIGHT / 2, "Sem dados", GRAPH_TEXT_COLOR, GRAPH_BG_COLOR, 1);
        return;
    }

    int start_index = graph_start_index(points);
    draw_graph_series(s_graph_temp, start_index, points, GRAPH_TEMP_COLOR, GRAPH_TEMP_MIN, GRAPH_TEMP_MAX);
    draw_graph_series(s_graph_hum, start_index, points, GRAPH_HUM_COLOR, GRAPH_HUM_MIN, GRAPH_HUM_MAX);

    int latest_index = s_graph_write_index - 1;
    if (latest_index < 0) {
        latest_index += GRAPH_MAX_POINTS;
    }
    float latest_temp = s_graph_temp[latest_index];
    float latest_hum = s_graph_hum[latest_index];
    draw_graph_latest_values(true, latest_temp, latest_hum);
}

// Mostra um aviso claro de que o cartão SD foi removido.
// Invocado quando a task_sensor entra no modo de espera por SD.
void display_show_sd_removed(void)
{
    if (!s_display_ready) {
        return;
    }

    const char *msg = "MicroSD removido";
    const uint8_t text_size = 1;
    const uint16_t char_width = 8 * text_size;
    uint16_t msg_width = (uint16_t)(strlen(msg) * char_width);
    uint16_t x = (ST7735_WIDTH > msg_width) ? ((ST7735_WIDTH - msg_width) / 2) : 2;
    uint16_t y = (ST7735_HEIGHT / 2) - (text_size * 4);

    st7735_fill_screen(ST7735_BLACK);
    st7735_draw_string(x, y, msg, ST7735_BLUE, ST7735_BLACK, text_size);
    s_table_layout_ready = false;
}

// Atualiza o ecrã selecionado (principal, gráfico, logs).
// Usado pela task_button_screen e pela task_display.
void display_set_screen(display_screen_t screen)
{
    if (screen >= SCREEN_MAX) {
        return;
    }

    if (s_current_screen == screen) {
        return;
    }

    s_current_screen = screen;
    if (screen == SCREEN_MAIN) {
        s_table_layout_ready = false;
    }
}

// Devolve o ecrã ativo para que outras tasks saibam o contexto atual.
// Principalmente usado para sincronizar botões e atualizações.
display_screen_t display_get_screen(void)
{
    return s_current_screen;
}

// Atualiza os valores das três linhas (temperatura, humidade, estado).
// Chamado sempre que há nova leitura ou mudança de standby.
void display_draw_table(float temp, float hum, bool sensor_ok, bool standby_active)
{
    if (!s_display_ready) {
        return;
    }

    if (!s_table_layout_ready) {
        draw_table_layout();
    }

    char sensor_text[16];
    uint16_t sensor_color = ST7735_GREEN;
    if (standby_active) {
        snprintf(sensor_text, sizeof(sensor_text), "%s", "STANDBY");
        sensor_color = ST7735_BLUE; //(cores trocadas)
    } else {
        snprintf(sensor_text, sizeof(sensor_text), "%s", sensor_ok ? "SENSOR ON" : "SENSOR OFF");
        sensor_color = sensor_ok ? ST7735_GREEN : SENSOR_OFF_COLOR;
    }

    if (standby_active) {
        draw_row_value(0, "STOP", ST7735_BLUE, ST7735_BLACK, ROW_VALUE_TEXT_SIZE, false, false, NULL);
        draw_row_value(1, "STOP", ST7735_BLUE, ST7735_BLACK, ROW_VALUE_TEXT_SIZE, false, false, NULL);
    } else {
        char temp_text[16];
        char hum_text[16];
        snprintf(temp_text, sizeof(temp_text), "%5.2f", temp);
        snprintf(hum_text, sizeof(hum_text), "%5.2f", hum);
        draw_row_value(0, temp_text, ST7735_WHITE, ST7735_BLACK, ROW_VALUE_TEXT_SIZE, false, true, "C");
        draw_row_value(1, hum_text, ST7735_WHITE, ST7735_BLACK, ROW_VALUE_TEXT_SIZE, false, false, "%");
    }

    draw_row_value(2, sensor_text, sensor_color,
                   ST7735_BLACK, ROW_SENSOR_TEXT_SIZE, true, false, NULL);
}

// Desenha estrutura da tabela (bordas, separadores e labels).
// Executado na inicialização e sempre que um clear forçado ocorre.
static void draw_table_layout(void)
{
    if (!s_display_ready) {
        return;
    }

    st7735_fill_screen(ST7735_BLACK);

    // Contorno exterior da tabela
    st7735_fill_rect(TABLE_X, TABLE_Y, TABLE_WIDTH, BORDER_THICKNESS, ST7735_GRAY);
    st7735_fill_rect(TABLE_X, TABLE_Y + TABLE_HEIGHT - BORDER_THICKNESS, TABLE_WIDTH, BORDER_THICKNESS, ST7735_GRAY);
    st7735_fill_rect(TABLE_X, TABLE_Y, BORDER_THICKNESS, TABLE_HEIGHT, ST7735_GRAY);
    st7735_fill_rect(TABLE_X + TABLE_WIDTH - BORDER_THICKNESS, TABLE_Y, BORDER_THICKNESS, TABLE_HEIGHT, ST7735_GRAY);

    // Coluna dos rótulos
    st7735_fill_rect(TABLE_X + BORDER_THICKNESS,
                     TABLE_Y + BORDER_THICKNESS,
                     LABEL_COLUMN_WIDTH,
                     TABLE_HEIGHT - 2 * BORDER_THICKNESS,
                     LABEL_BG_COLOR);

    // Separador vertical entre os rótulos e valores
    st7735_fill_rect(SEPARATOR_X, TABLE_Y, BORDER_THICKNESS, TABLE_HEIGHT, ST7735_GRAY);

    // Separadores horizontais das linhas
    for (uint8_t row = 1; row < DISPLAY_ROW_COUNT; ++row) {
        uint16_t sep_y = TABLE_Y + row * ROW_HEIGHT;
        st7735_fill_rect(TABLE_X + BORDER_THICKNESS, sep_y, TABLE_WIDTH - 2 * BORDER_THICKNESS, BORDER_THICKNESS, ST7735_GRAY);
    }

    // Zona dos valores limpa
    st7735_fill_rect(VALUE_AREA_X,
                     TABLE_Y + BORDER_THICKNESS,
                     TABLE_WIDTH - LABEL_COLUMN_WIDTH - 3 * BORDER_THICKNESS,
                     TABLE_HEIGHT - 2 * BORDER_THICKNESS,
                     ST7735_BLACK);

    draw_row_label(0, "Temperatura");
    draw_row_label(1, "Humidade");
    draw_row_label(2, "Sensor");

    s_table_layout_ready = true;
}

// Escreve o texto fixo da coluna esquerda na linha indicada.
// Mantém consistência visual nos diferentes ecrãs.
static void draw_row_label(uint8_t row, const char *label)
{
    const uint16_t char_height = 9 * ROW_LABEL_TEXT_SIZE;
    const uint16_t base_y = TABLE_Y + row * ROW_HEIGHT;
    uint16_t y = base_y + (ROW_HEIGHT - char_height) / 2;
    st7735_draw_string(TABLE_X + BORDER_THICKNESS + TEXT_PADDING,
                       y,
                       label,
                       ST7735_WHITE,
                       LABEL_BG_COLOR,
                       ROW_LABEL_TEXT_SIZE);
}

// Limpa a área de valores de uma linha mantendo o fundo configurado.
// Usado antes de redesenhar textos variáveis para evitar ghosting.
static void clear_row_value_area(uint8_t row, uint16_t bg_color)
{
    const uint16_t row_y = TABLE_Y + row * ROW_HEIGHT + BORDER_THICKNESS + 1;
    uint16_t area_height = ROW_HEIGHT - BORDER_THICKNESS - 2;
    int16_t vertical_offset = 0;

    if (row == DISPLAY_ROW_COUNT - 1) {
        // A linha inferior partilha espaço com a moldura inferior, ajusta o preenchimento.
        area_height -= 1;
        vertical_offset = -1;
    }

    const uint16_t start_x = VALUE_AREA_X + TEXT_PADDING / 2;
    const uint16_t end_x = TABLE_X + TABLE_WIDTH - BORDER_THICKNESS;
    const uint16_t area_width = (end_x > start_x) ? (end_x - start_x) : 0;
    st7735_fill_rect(start_x,
                     row_y + vertical_offset,
                     area_width,
                     area_height,
                     bg_color);
}

// Escreve o valor e suffix numa linha, com opção de adicionar símbolo de graus.
// Suporta modo condensado e símbolo de grau conforme necessário.
static void draw_row_value(uint8_t row, const char *value, uint16_t text_color,
                           uint16_t bg_color, uint8_t text_size, bool condensed,
                           bool with_degree_symbol, const char *suffix)
{
    clear_row_value_area(row, bg_color);
    const uint16_t char_height = 8 * text_size;
    const uint16_t base_y = TABLE_Y + row * ROW_HEIGHT;
    const uint16_t y = base_y + (ROW_HEIGHT - char_height) / 2;
    uint16_t cursor_x = VALUE_AREA_X + TEXT_PADDING;

    if (condensed) {
        cursor_x = draw_condensed_string(cursor_x, y, value, text_color, bg_color, text_size);
    } else {
        cursor_x = draw_regular_string(cursor_x, y, value, text_color, bg_color, text_size);
    }

    if (with_degree_symbol) {
        cursor_x += DEGREE_SYMBOL_GAP;
        uint16_t symbol_y = (y > (2 * text_size)) ? (y - 2 * text_size) : y;
        draw_degree_symbol(cursor_x, symbol_y, text_size, text_color, bg_color);
        cursor_x += (text_size * 3);
    }

    if (suffix && suffix[0] != '\0') {
        cursor_x += TEXT_PADDING / 2;
        draw_regular_string(cursor_x, y, suffix, text_color, bg_color, text_size);
    }
}

// Helper para desenhar texto monoespaçado regular e devolver largura usada.
// Utilizado pelas rotinas de label/valor quando há espaço suficiente.
static uint16_t draw_regular_string(uint16_t x, uint16_t y, const char *str,
                                    uint16_t color, uint16_t bg, uint8_t size)
{
    uint16_t cx = x;
    while (*str) {
        if (*str == '\n') {
            y += 8 * size;
            cx = x;
        } else {
            st7735_draw_char(cx, y, *str, color, bg, size);
            cx += 6 * size;
        }
        str++;
    }
    return cx;
}

// Variante condensada que reduz espaçamento entre caracteres.
// Útil quando os valores excedem a largura padrão.
static uint16_t draw_condensed_string(uint16_t x, uint16_t y, const char *str,
                                      uint16_t color, uint16_t bg, uint8_t size)
{
    uint16_t cx = x;
    const uint16_t advance = SENSOR_TEXT_SPACING * size;

    while (*str) {
        if (*str == '\n') {
            y += 8 * size;
            cx = x;
        } else {
            st7735_draw_char(cx, y, *str, color, bg, size);
            cx += advance;
        }
        str++;
    }
    return cx;
}

//Esta é a função para tentar desenhar o º, porque o driver nao dá para certos caracteres ASCII

// Desenha um pequeno círculo para representar o símbolo de grau.
// Usado após valores de temperatura na tabela.
static void draw_degree_symbol(uint16_t x, uint16_t y, uint8_t size,
                               uint16_t color, uint16_t bg)
{
    const uint16_t radius = size;
    const uint16_t diameter = radius * 2;
    st7735_fill_rect(x, y, diameter + 1, diameter + 1, bg);

    for (uint16_t dy = 0; dy <= diameter; ++dy) {
        for (uint16_t dx = 0; dx <= diameter; ++dx) {
            int16_t dist_x = (int16_t)dx - (int16_t)radius;
            int16_t dist_y = (int16_t)dy - (int16_t)radius;
            if ((dist_x * dist_x + dist_y * dist_y) <= (radius * radius + 1)) {
                st7735_draw_pixel(x + dx, y + dy, color);
            }
        }
    }
}

// Desenha uma linha pixel a pixel controlando cor e limites.
// Base usado por draw_graph_axes e outros elementos gráficos.
static void draw_line_segment(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                              uint16_t color)
{
    int ix0 = (int)x0;
    int iy0 = (int)y0;
    int ix1 = (int)x1;
    int iy1 = (int)y1;
    int dx = abs(ix1 - ix0);
    int sx = (ix0 < ix1) ? 1 : -1;
    int dy = -abs(iy1 - iy0);
    int sy = (iy0 < iy1) ? 1 : -1;
    int err = dx + dy;

    while (true) {
        st7735_draw_pixel((uint16_t)ix0, (uint16_t)iy0, color);
        if (ix0 == ix1 && iy0 == iy1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            ix0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            iy0 += sy;
        }
    }
}

// Desenha os eixos principais do gráfico com ou sem eixo secundário.
// Chamado antes de renderizar as séries de temperatura/humidade.
static void draw_graph_axes(bool dual_axis)
{
    const uint16_t origin_x = GRAPH_MARGIN_LEFT;
    const uint16_t origin_y = ST7735_HEIGHT - GRAPH_MARGIN_BOTTOM;
    const uint16_t graph_width = ST7735_WIDTH - GRAPH_MARGIN_LEFT - GRAPH_MARGIN_RIGHT;
    const uint16_t graph_height = ST7735_HEIGHT - GRAPH_MARGIN_TOP - GRAPH_MARGIN_BOTTOM;

    st7735_fill_rect(origin_x, origin_y - graph_height, graph_width, graph_height, GRAPH_BG_COLOR);
    st7735_fill_rect(origin_x, origin_y - graph_height, 1, graph_height, GRAPH_AXIS_COLOR);
    st7735_fill_rect(origin_x, origin_y, graph_width, 1, GRAPH_AXIS_COLOR);
    if (dual_axis) {
        st7735_fill_rect(origin_x + graph_width, origin_y - graph_height, 1, graph_height, GRAPH_AXIS_COLOR);
    }

    const char *title = dual_axis ? "Temperatura & Humidade" : "Temperatura";
    uint16_t title_width = (uint16_t)(strlen(title) * 6);
    uint16_t title_x = (ST7735_WIDTH > title_width) ? ((ST7735_WIDTH - title_width) / 2) : 2;
    if (dual_axis) {
        const char *temp_word = "Temperatura";
        uint16_t temp_width = (uint16_t)(strlen(temp_word) * 6);
        uint16_t cursor_x = title_x;
        st7735_draw_string(cursor_x, 2, temp_word, GRAPH_TEMP_COLOR, GRAPH_BG_COLOR, 1);
        cursor_x += temp_width;
        const char *sep = " & ";
        st7735_draw_string(cursor_x, 2, sep, GRAPH_TEXT_COLOR, GRAPH_BG_COLOR, 1);
        cursor_x += (uint16_t)(strlen(sep) * 6);
        st7735_draw_string(cursor_x, 2, "Humidade", GRAPH_HUM_COLOR, GRAPH_BG_COLOR, 1);
    } else {
        st7735_draw_string(title_x, 2, title, GRAPH_TEMP_COLOR, GRAPH_BG_COLOR, 1);
    }

    char label[16];
    uint16_t temp_label_x_max = (origin_x > 16) ? (origin_x - 16) : 0;
    uint16_t temp_label_x_min = (origin_x > 9) ? (origin_x - 9) : 0;
    snprintf(label, sizeof(label), "%.0f", GRAPH_TEMP_MAX);
    st7735_draw_string(temp_label_x_max, GRAPH_MARGIN_TOP, label, GRAPH_TEMP_COLOR, GRAPH_BG_COLOR, 1);
    snprintf(label, sizeof(label), "%.0f", GRAPH_TEMP_MIN);
    st7735_draw_string(temp_label_x_min, origin_y - 6, label, GRAPH_TEMP_COLOR, GRAPH_BG_COLOR, 1);

    if (dual_axis) {
        uint16_t axis_right = origin_x + graph_width;
        uint16_t right_label_x = (axis_right > 20) ? (axis_right - 19) : axis_right;
        snprintf(label, sizeof(label), "%.0f", GRAPH_HUM_MAX);
        st7735_draw_string(right_label_x, GRAPH_MARGIN_TOP, label, GRAPH_HUM_COLOR, GRAPH_BG_COLOR, 1);
        snprintf(label, sizeof(label), "%.0f", GRAPH_HUM_MIN);
        uint16_t hum_min_x = (axis_right + 3 < ST7735_WIDTH) ? (axis_right + 3) : (ST7735_WIDTH - 6);
        st7735_draw_string(hum_min_x, origin_y - 6, label, GRAPH_HUM_COLOR, GRAPH_BG_COLOR, 1);
    }
}

// Percorre o buffer circular e liga os pontos via line segments.
// É chamado para cada série (temperatura e humidade) com cor distinta.
static void draw_graph_series(const float *buffer, int start_index, int points,
                              uint16_t color, float min_val, float max_val)
{
    if (points <= 0) {
        return;
    }

    const uint16_t origin_x = GRAPH_MARGIN_LEFT;
    const uint16_t graph_width = ST7735_WIDTH - GRAPH_MARGIN_LEFT - GRAPH_MARGIN_RIGHT;

    uint16_t prev_x = 0;
    uint16_t prev_y = 0;
    bool has_prev = false;

    for (int i = 0; i < points; ++i) {
        int idx = (start_index + i) % GRAPH_MAX_POINTS;
        float value = buffer[idx];
        uint16_t x = origin_x;
        if (points > 1) {
            x = origin_x + (uint16_t)((i * (graph_width - 1)) / (points - 1));
        }
        uint16_t y = graph_map_value_to_y(value, min_val, max_val);

        if (has_prev) {
            draw_line_segment(prev_x, prev_y, x, y, color);
        } else {
            st7735_draw_pixel(x, y, color);
        }
        has_prev = true;
        prev_x = x;
        prev_y = y;
    }
}

// Mostra os valores mais recentes abaixo do gráfico com as respetivas cores.
static void draw_graph_latest_values(bool has_data, float temp, float hum)
{
    char temp_text[24];
    char hum_text[24];

    if (has_data) {
        snprintf(temp_text, sizeof(temp_text), "%.2f C", temp);
        snprintf(hum_text, sizeof(hum_text), "%.2f %%", hum);
    } else {
        snprintf(temp_text, sizeof(temp_text), "-- C");
        snprintf(hum_text, sizeof(hum_text), "-- %%");
    }

    uint16_t text_y = ST7735_HEIGHT - GRAPH_MARGIN_BOTTOM + 4;
    if (text_y + 8 > ST7735_HEIGHT) {
        text_y = (ST7735_HEIGHT > 10) ? (ST7735_HEIGHT - 10) : (ST7735_HEIGHT - 8);
    }
    const uint16_t graph_width = ST7735_WIDTH - GRAPH_MARGIN_LEFT - GRAPH_MARGIN_RIGHT;
    uint16_t temp_x = GRAPH_MARGIN_LEFT;

    st7735_draw_string(temp_x, text_y, temp_text, GRAPH_TEMP_COLOR, GRAPH_BG_COLOR, 1);

    uint16_t hum_width = (uint16_t)(strlen(hum_text) * 6);
    uint16_t hum_x;
    if (hum_width < graph_width) {
        hum_x = GRAPH_MARGIN_LEFT + graph_width - hum_width;
    } else {
        hum_x = GRAPH_MARGIN_LEFT;
    }

    st7735_draw_string(hum_x, text_y, hum_text, GRAPH_HUM_COLOR, GRAPH_BG_COLOR, 1);
}

// Converte um valor físico para coordenada Y no espaço do gráfico.
// Normaliza considerando margens e limites configurados.
static uint16_t graph_map_value_to_y(float value, float min_val, float max_val)
{
    if (max_val <= min_val) {
        return ST7735_HEIGHT - GRAPH_MARGIN_BOTTOM;
    }

    if (value < min_val) {
        value = min_val;
    } else if (value > max_val) {
        value = max_val;
    }

    const uint16_t graph_height = ST7735_HEIGHT - GRAPH_MARGIN_TOP - GRAPH_MARGIN_BOTTOM;
    const uint16_t origin_y = ST7735_HEIGHT - GRAPH_MARGIN_BOTTOM;
    float ratio = (value - min_val) / (max_val - min_val);
    uint16_t delta = (uint16_t)(ratio * graph_height);
    if (delta > graph_height) {
        delta = graph_height;
    }
    return origin_y - delta;
}

// Determina quantos pontos devem ser desenhados (limitado à capacidade).
// Baseia-se no número de valores válidos já armazenados.
static int graph_points_to_draw(void)
{
    if (s_graph_count == 0) {
        return 0;
    }

    int limit = ST7735_WIDTH - GRAPH_MARGIN_LEFT - GRAPH_MARGIN_RIGHT;
    if (limit > GRAPH_MAX_POINTS) {
        limit = GRAPH_MAX_POINTS;
    }
    if (limit <= 0) {
        limit = GRAPH_MAX_POINTS;
    }

    return (s_graph_count < limit) ? s_graph_count : limit;
}

// Calcula o índice inicial no buffer circular para desenhar os últimos pontos.
// Evita percorrer mais dados do que o necessário.
static int graph_start_index(int points_to_draw)
{
    if (points_to_draw <= 0) {
        return 0;
    }

    int start = s_graph_write_index - points_to_draw;
    while (start < 0) {
        start += GRAPH_MAX_POINTS;
    }
    return start % GRAPH_MAX_POINTS;
}
