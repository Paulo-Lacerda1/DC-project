// Controla o LED RGB do painel via periférico RMT com padrões de estado.
// Parte do módulo core de feedback visual.
#include <stdbool.h>
#include "led.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "RGB_LED"

#define LED_BLINK_ON_MS   100    // tempo com o  LED ligado em cada ciclo
#define LED_BLINK_OFF_MS  900    // tempo com o LED desligado em cada ciclo

#define LED_STRIP_RESOLUTION_HZ (10 * 1000 * 1000) // 10 MHz -> 0.1 us
#define LED_T0H_TICKS 4
#define LED_T0L_TICKS 8
#define LED_T1H_TICKS 8
#define LED_T1L_TICKS 4

#define LED_BRIGHTNESS 0x20                         //regular para depois ter atencao ao consumo
#define LED_TX_TIMEOUT_MS 20

static rmt_channel_handle_t led_channel;
static rmt_encoder_handle_t led_encoder;
static led_state_t current_state = LED_STATE_OFF;
static rmt_transmit_config_t tx_config = {
    .loop_count = 0,
};

static void led_task_blink(void *arg); // tarefa responsável por piscar o LED

// Envia a combinação GRB para o WS2812 através do periférico RMT.
// Usado tanto pelas APIs públicas como pela task de blink.
static void rgb_led_show(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!led_channel || !led_encoder) {
        ESP_LOGW(TAG, "RGB LED ainda não foi inicializado");
        return;
    }

    const uint8_t grb[3] = { green, red, blue };
    esp_err_t err = rmt_transmit(led_channel, led_encoder, grb, sizeof(grb), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao enviar dados para o LED (%s)", esp_err_to_name(err));
        return;
    }
    err = rmt_tx_wait_all_done(led_channel, LED_TX_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Timeout à espera do LED (%s)", esp_err_to_name(err));
    }
}

// Configura o canal RMT e o encoder para controlar o LED RGB.
// Chamado no arranque antes de qualquer alteração de estado.
void led_init(void)
{
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = LED_RGB_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &led_channel));
    ESP_ERROR_CHECK(rmt_enable(led_channel));

    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = LED_T0H_TICKS,
            .level1 = 0,
            .duration1 = LED_T0L_TICKS,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = LED_T1H_TICKS,
            .level1 = 0,
            .duration1 = LED_T1L_TICKS,
        },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&encoder_config, &led_encoder));

    led_off();
    // tarefa fica a tomar conta dos padrões de piscar
    if (xTaskCreate(led_task_blink, "led_blink", 2048, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar a tarefa de blink do LED");
    }
}

// Atalho para fixar o LED no estado normal (verde).
// Usado por módulos que querem apenas indicar operação nominal.
void led_on(void)
{
    led_set_state_with_reason(LED_STATE_NORMAL, NULL);
}

// Desliga o LED e cancela qualquer padrão ativo.
// Usado quando o sistema entra em standby ou modo furtivo.
void led_off(void)
{
    led_set_state_with_reason(LED_STATE_OFF, NULL);
}

// Atualiza o estado alvo do LED e opcionalmente regista o motivo.
// Chamado por todas as tasks interessadas em sinalizar erros ou TX.
void led_set_state_with_reason(led_state_t state, const char *reason)
{
    current_state = state; // tarefa irá ler este estado e atualizar o LED
    (void)reason; // Mantido para eventual logging de motivos sem alterar a API.
}


// Wrapper que ignora o motivo quando apenas precisamos trocar de estado.
// Mantém compatibilidade com chamadas antigas.
void led_set_state(led_state_t state)
{
    led_set_state_with_reason(state, NULL);
}

// Devolve o estado registado para que outras tasks saibam o padrão atual.
// Usado pela task de blink e por código que precisa saber o estado base.
led_state_t led_get_state(void)
{
    return current_state;
}

// Task responsável por piscar o LED conforme o estado selecionado.
// Corre em loop infinito e aplica duty-cycle definido pelas constantes.
static void led_task_blink(void *arg)
{
    (void)arg;

    // Loop infinito para controlar o LED
    while (true) {
        // Lê o estado atual do LED (pode ser alterado por outras tasks)
        led_state_t state = current_state;
        
        // Inicializa cores RGB a zero
        uint8_t red = 0, green = 0, blue = 0;

        // Define a cor conforme o estado
        switch (state) {
        case LED_STATE_NORMAL:
            // Verde = operação normal
            green = LED_BRIGHTNESS;
            break;
        case LED_STATE_ERROR:
            // Vermelho = erro
            red = LED_BRIGHTNESS;
            break;
        case LED_STATE_TRANSMISSION:
            // Amarelo (vermelho + verde) = transmissão MQTT
            red = LED_BRIGHTNESS;
            green = LED_BRIGHTNESS;
            break;
        case LED_STATE_OFF:
        default:
            // Cores permanecem a zero (LED desligado)
            break;
        }

        // Se o estado é OFF, mantém LED desligado sem piscar
        if (state == LED_STATE_OFF) {
            rgb_led_show(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_OFF_MS));
            continue;
        }
        
        // Período ON: acende o LED com a cor definida
        rgb_led_show(red, green, blue);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_ON_MS));
        
        // Período OFF: apaga o LED (cria efeito de piscar)
        rgb_led_show(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_OFF_MS));
    }
}
