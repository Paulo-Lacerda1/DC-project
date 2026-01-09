// Ajusta o brilho do TFT com base no potenciometro analógico.
// Faz parte do módulo de interface gráfica responsável pelo backlight.
#include "task_brightness.h"

#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/ledc.h"
#include "board_pins.h"
#include "logging.h"

#define BRIGHTNESS_TASK_STACK              4096
#define BRIGHTNESS_TASK_PRIORITY           4
#define BRIGHTNESS_TASK_DELAY_MS           250
#define BRIGHTNESS_VARIATION_PERCENT       5          // variacao necessaria para sair do modo poupanca
#define BRIGHTNESS_IDLE_STAGE1_TIMEOUT_MS  30000      // tempo até reduzir para 20%
#define BRIGHTNESS_IDLE_STAGE1_PERCENT     20         // brilho após 30s sem variacao
#define BRIGHTNESS_IDLE_STAGE2_TIMEOUT_MS  15000      // tempo adicional sem variacao ate desligar (15s)
#define BRIGHTNESS_IDLE_STAGE2_PERCENT     0          // brilho sem mais variacao
#define BRIGHTNESS_ADC_MAX                 4095
#define BRIGHTNESS_MAX_VOLTAGE_MV    3300
#define BRIGHTNESS_ADC_ATTEN         ADC_ATTEN_DB_12

#define BRIGHTNESS_PWM_TIMER         LEDC_TIMER_0
#define BRIGHTNESS_PWM_MODE          LEDC_LOW_SPEED_MODE
#define BRIGHTNESS_PWM_CHANNEL       LEDC_CHANNEL_0
#define BRIGHTNESS_PWM_FREQUENCY_HZ  5000
#define BRIGHTNESS_PWM_RESOLUTION    LEDC_TIMER_10_BIT
#define BRIGHTNESS_PWM_MAX_DUTY      1023
#define BRIGHTNESS_NOTIFY_USER_ACTIVITY      (1U << 0)

static const char *TAG = "task_brightness";
static TaskHandle_t s_task_brightness_handle = NULL;
static bool s_brightness_pwm_ready = false;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_adc_cali_ready = false;

typedef enum {
    BRIGHTNESS_CALI_NONE = 0,
    BRIGHTNESS_CALI_CURVE,
    BRIGHTNESS_CALI_LINE,
} brightness_cali_scheme_t;

static brightness_cali_scheme_t s_adc_cali_scheme = BRIGHTNESS_CALI_NONE;

typedef enum {
    BRIGHTNESS_IDLE_STATE_NONE = 0,
    BRIGHTNESS_IDLE_STATE_REDUCED,
    BRIGHTNESS_IDLE_STATE_OFF,
} brightness_idle_state_t;

// Converte a leitura RAW do ADC (0-4095) em duty-cycle PWM válido.
// Invocado quando a calibração em mV não está disponível.
static inline int brightness_raw_to_duty(int raw_value)
{
    if (raw_value < 0) {
        raw_value = 0;
    } else if (raw_value > BRIGHTNESS_ADC_MAX) {
        raw_value = BRIGHTNESS_ADC_MAX;
    }

    return (raw_value * BRIGHTNESS_PWM_MAX_DUTY) / BRIGHTNESS_ADC_MAX;
}

// Converte o valor em mV calibrados para duty-cycle PWM.
// Usado quando a calibração ADC consegue fornecer tensão real.
static inline int brightness_voltage_to_duty(int voltage_mv)
{
    if (voltage_mv < 0) {
        voltage_mv = 0;
    } else if (voltage_mv > BRIGHTNESS_MAX_VOLTAGE_MV) {
        voltage_mv = BRIGHTNESS_MAX_VOLTAGE_MV;
    }

    return (voltage_mv * BRIGHTNESS_PWM_MAX_DUTY) / BRIGHTNESS_MAX_VOLTAGE_MV;
}

// Converte percentagem (0-100%) em duty-cycle PWM.
static inline int brightness_percent_to_duty(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    return (percent * BRIGHTNESS_PWM_MAX_DUTY) / 100;
}

// Converte o valor RAW do ADC em percentagem (0-100%).
static inline int brightness_raw_to_percent(int raw_value)
{
    if (raw_value < 0) {
        raw_value = 0;
    } else if (raw_value > BRIGHTNESS_ADC_MAX) {
        raw_value = BRIGHTNESS_ADC_MAX;
    }

    return (raw_value * 100) / BRIGHTNESS_ADC_MAX;
}

// Tenta habilitar a calibração ADC necessária para obter leituras estáveis.
// Chamado ao iniciar a task ou quando o ADC é reconfigurado.
static bool brightness_adc_calibration_init(adc_unit_t unit, adc_channel_t channel)
{
    if (s_adc_cali_ready) {
        return true;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = BRIGHTNESS_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali_handle) == ESP_OK) {
        s_adc_cali_ready = true;
        s_adc_cali_scheme = BRIGHTNESS_CALI_CURVE;
        ESP_LOGI(TAG, "Calibração ADC pronta (curve fitting).");
        return true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_config = {
        .unit_id = unit,
        .atten = BRIGHTNESS_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
#if CONFIG_IDF_TARGET_ESP32
        .default_vref = 1100,
#endif
    };
    if (adc_cali_create_scheme_line_fitting(&line_config, &s_adc_cali_handle) == ESP_OK) {
        s_adc_cali_ready = true;
        s_adc_cali_scheme = BRIGHTNESS_CALI_LINE;
        ESP_LOGI(TAG, "Calibração ADC pronta (line fitting).");
        return true;
    }
#endif

    s_adc_cali_handle = NULL;
    s_adc_cali_scheme = BRIGHTNESS_CALI_NONE;
    s_adc_cali_ready = false;
    ESP_LOGW(TAG, "Calibração ADC indisponível; a usar valores RAW.");
    return false;
}

// Liberta recursos da calibração quando já não são necessários.
// Executado ao terminar a task ou ao fechar o ADC.
static void brightness_adc_calibration_deinit(void)
{
    if (!s_adc_cali_ready || !s_adc_cali_handle) {
        return;
    }

    switch (s_adc_cali_scheme) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        case BRIGHTNESS_CALI_CURVE:
            adc_cali_delete_scheme_curve_fitting(s_adc_cali_handle);
            break;
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        case BRIGHTNESS_CALI_LINE:
            adc_cali_delete_scheme_line_fitting(s_adc_cali_handle);
            break;
#endif
        default:
            break;
    }

    s_adc_cali_handle = NULL;
    s_adc_cali_ready = false;
    s_adc_cali_scheme = BRIGHTNESS_CALI_NONE;
}

// Configura o timer e o canal LEDC que alimentam o backlight.
// Chamado no arranque e antes de criar a task de brilho.
esp_err_t brightness_pwm_init(void)
{
    if (s_brightness_pwm_ready) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode       = BRIGHTNESS_PWM_MODE,
        .duty_resolution  = BRIGHTNESS_PWM_RESOLUTION,
        .timer_num        = BRIGHTNESS_PWM_TIMER,
        .freq_hz          = BRIGHTNESS_PWM_FREQUENCY_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };

    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar o timer LEDC (%s)", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num   = PIN_TFT_LIT,
        .speed_mode = BRIGHTNESS_PWM_MODE,
        .channel    = BRIGHTNESS_PWM_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BRIGHTNESS_PWM_TIMER,
        .duty       = BRIGHTNESS_PWM_MAX_DUTY, // arranca no máximo brilho
        .hpoint     = 0,
    };

    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar o canal LEDC (%s)", esp_err_to_name(err));
        return err;
    }

    err = ledc_set_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL, BRIGHTNESS_PWM_MAX_DUTY);
    if (err == ESP_OK) {
        err = ledc_update_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao aplicar duty-cycle inicial (%s)", esp_err_to_name(err));
        return err;
    }

    s_brightness_pwm_ready = true;
    ESP_LOGI(TAG, "PWM de brilho inicializado (max duty %d)", BRIGHTNESS_PWM_MAX_DUTY);
    return ESP_OK;
}

// Task FreeRTOS que lê o potenciometro e ajusta o PWM se o valor mudar.
// Criada por task_brightness_start e corre continuamente em background.
static void task_brightness(void *pvParameters)
{
    // Converte o GPIO do potenciómetro para unidade e canal ADC
    adc_unit_t adc_unit;
    adc_channel_t adc_channel;
    if (adc_oneshot_io_to_channel(PIN_POT_ADC, &adc_unit, &adc_channel) != ESP_OK || adc_unit != ADC_UNIT_1) {
        // Falha ao mapear GPIO, termina a task
        ESP_LOGE(TAG, "Falha ao mapear o GPIO%d para o ADC1", (int)PIN_POT_ADC);
        vTaskDelete(NULL);
        return;
    }

    // Cria e inicializa o handle do ADC One-Shot
    adc_oneshot_unit_handle_t adc_handle;
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = adc_unit,
    };
    if (adc_oneshot_new_unit(&unit_config, &adc_handle) != ESP_OK) {
        // Falha ao criar unidade ADC
        ESP_LOGE(TAG, "Falha ao inicializar o ADC One-Shot");
        vTaskDelete(NULL);
        return;
    }

    // Configura o canal ADC com atenuação e resolução
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten    = BRIGHTNESS_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc_handle, adc_channel, &channel_config) != ESP_OK) {
        // Falha ao configurar canal, limpa recursos
        ESP_LOGE(TAG, "Falha ao configurar o canal do ADC");
        adc_oneshot_del_unit(adc_handle);
        brightness_adc_calibration_deinit();
        vTaskDelete(NULL);
        return;
    }

    // Inicializa o PWM para controlar o backlight do LCD
    if (brightness_pwm_init() != ESP_OK) {
        // Falha ao inicializar PWM, limpa recursos
        adc_oneshot_del_unit(adc_handle);
        brightness_adc_calibration_deinit();
        vTaskDelete(NULL);
        return;
    }

    // Tenta inicializar calibração do ADC (opcional)
    brightness_adc_calibration_init(adc_unit, adc_channel);

    // Guarda a última percentagem aplicada para detetar mudanças significativas
    int last_percent = -1;
    const TickType_t idle_stage1_timeout_ticks = pdMS_TO_TICKS(BRIGHTNESS_IDLE_STAGE1_TIMEOUT_MS);
    const TickType_t idle_stage2_timeout_ticks = pdMS_TO_TICKS(BRIGHTNESS_IDLE_STAGE2_TIMEOUT_MS);
    TickType_t last_variation_tick = xTaskGetTickCount();
    brightness_idle_state_t idle_state = BRIGHTNESS_IDLE_STATE_NONE;

    // Loop principal de monitorização do potenciómetro
    while (true) {
        // Lê valor RAW do ADC (0-4095)
        int adc_raw = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, adc_channel, &adc_raw);
        if (err != ESP_OK) {
            // Erro na leitura, aguarda e tenta novamente
            ESP_LOGE(TAG, "Erro ao ler ADC (%s)", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(BRIGHTNESS_TASK_DELAY_MS));
            continue;
        }

        const int current_percent = brightness_raw_to_percent(adc_raw);
        bool has_variation = false;
        if (last_percent < 0) {
            // Primeira leitura, sempre atualiza
            has_variation = true;
        } else if (abs(current_percent - last_percent) >= BRIGHTNESS_VARIATION_PERCENT) {
            // Mudança significativa detetada (>= 5%)
            has_variation = true;
        }

        const TickType_t now = xTaskGetTickCount(); //momento (em ticks) que a leitura aconteceu
        
        // Verifica notificações pendentes para saber se houve atividade externa desde o último ciclo
        uint32_t notify_value = 0;
        bool user_activity = false;
        if (xTaskNotifyWait(0, BRIGHTNESS_NOTIFY_USER_ACTIVITY, &notify_value, 0) == pdPASS) {
            if (notify_value & BRIGHTNESS_NOTIFY_USER_ACTIVITY) {
                user_activity = true;
            }
        }

        if (has_variation || user_activity) {
            last_percent = current_percent;
            last_variation_tick = now;

            if (idle_state != BRIGHTNESS_IDLE_STATE_NONE) {
                idle_state = BRIGHTNESS_IDLE_STATE_NONE;
                if (has_variation) {
                    ESP_LOGI(TAG, "Variação >=%d%% detetada; a retomar o brilho do potenciómetro.",
                             BRIGHTNESS_VARIATION_PERCENT);
                } else {
                    ESP_LOGI(TAG, "Atividade externa detetada; a retomar brilho normal.");
                }
                log_info(TAG, "Backlight retomado após atividade do utilizador.");
            }

            int voltage_mv = 0;
            bool used_calibration = false;
            int duty = 0;

            // Tenta usar calibração se disponível
            if (s_adc_cali_ready && s_adc_cali_handle) {
                if (adc_cali_raw_to_voltage(s_adc_cali_handle, adc_raw, &voltage_mv) == ESP_OK) {
                    // Converte tensão calibrada para duty-cycle
                    duty = brightness_voltage_to_duty(voltage_mv);
                    used_calibration = true;
                } else {
                    // Falha na conversão, usa valor RAW
                    ESP_LOGW(TAG, "Falha ao converter ADC para mV; a usar RAW.");
                }
            }

            // Se calibração falhou ou não está disponível, usa valor RAW
            if (!used_calibration) {
                duty = brightness_raw_to_duty(adc_raw);
            }

            // Aplica o novo duty-cycle ao PWM
            err = ledc_set_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL, duty);
            if (err == ESP_OK) {
                // Confirma a atualização do duty-cycle
                err = ledc_update_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL);
            }

            // Regista o resultado
            if (err == ESP_OK) {
                // Calcula percentagem de brilho
                const int percent = (duty * 100) / BRIGHTNESS_PWM_MAX_DUTY;
                if (used_calibration) {
                    ESP_LOGI(TAG, "ADC=%d (%dmV) => brilho=%d%% (duty=%d)", adc_raw, voltage_mv, percent, duty);
                } else {
                    ESP_LOGI(TAG, "ADC=%d => brilho=%d%% (duty=%d)", adc_raw, percent, duty);
                }
            } else {
                // Erro ao atualizar PWM
                ESP_LOGE(TAG, "Falha ao actualizar o duty-cycle (%s)", esp_err_to_name(err));
            }
        } else {
            const TickType_t idle_duration = now - last_variation_tick;

            if (idle_state == BRIGHTNESS_IDLE_STATE_NONE &&
                last_percent != 0 &&
                idle_duration >= idle_stage1_timeout_ticks) {
                // Primeiro patamar de poupança: força brilho reduzido após 10s sem variação
                const int duty = brightness_percent_to_duty(BRIGHTNESS_IDLE_STAGE1_PERCENT);
                esp_err_t err = ledc_set_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL, duty);
                if (err == ESP_OK) {
                    err = ledc_update_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL);
                }

                if (err == ESP_OK) {
                    idle_state = BRIGHTNESS_IDLE_STATE_REDUCED;
                    ESP_LOGI(TAG, "Sem variação >=%d%% por %ds; brilho forçado a %d%% (duty=%d)",
                             BRIGHTNESS_VARIATION_PERCENT,
                             BRIGHTNESS_IDLE_STAGE1_TIMEOUT_MS / 1000,
                             BRIGHTNESS_IDLE_STAGE1_PERCENT,
                             duty);
                    log_info(TAG,
                             "Backlight reduzido automaticamente para %d%% devido a inatividade.",
                             BRIGHTNESS_IDLE_STAGE1_PERCENT);
                } else {
                    ESP_LOGE(TAG, "Falha ao forçar o duty-cycle (%s)", esp_err_to_name(err));
                }
            } else if (idle_state == BRIGHTNESS_IDLE_STATE_REDUCED &&
                       idle_duration >= (idle_stage1_timeout_ticks + idle_stage2_timeout_ticks)) {
                // Segundo patamar: após mais 5s sem variação, desliga o backlight (0%)
                const int duty = brightness_percent_to_duty(BRIGHTNESS_IDLE_STAGE2_PERCENT);
                esp_err_t err = ledc_set_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL, duty);
                if (err == ESP_OK) {
                    err = ledc_update_duty(BRIGHTNESS_PWM_MODE, BRIGHTNESS_PWM_CHANNEL);
                }

                if (err == ESP_OK) {
                    idle_state = BRIGHTNESS_IDLE_STATE_OFF;
                    ESP_LOGI(TAG, "Mais %ds sem variação; brilho forçado a %d%% (duty=%d)",
                             BRIGHTNESS_IDLE_STAGE2_TIMEOUT_MS / 1000,
                             BRIGHTNESS_IDLE_STAGE2_PERCENT,
                             duty);
                    log_info(TAG, "Backlight desligado automaticamente por inatividade prolongada.");
                } else {
                    ESP_LOGE(TAG, "Falha ao forçar o duty-cycle (%s)", esp_err_to_name(err));
                }
            }
        }

        // Aguarda antes da próxima leitura
        vTaskDelay(pdMS_TO_TICKS(BRIGHTNESS_TASK_DELAY_MS));
    }
}


// Inicializa o PWM (se necessário) e cria a task de monitorização de brilho.
// Invocado pelo app_main logo após o display estar disponível.
void task_brightness_start(void)
{
    if (s_task_brightness_handle != NULL) {
        return;
    }

    if (brightness_pwm_init() != ESP_OK) {
        ESP_LOGE(TAG, "Não foi possível inicializar o PWM de brilho.");
        return;
    }

    const BaseType_t result = xTaskCreate(
        task_brightness,
        "task_brightness",
        BRIGHTNESS_TASK_STACK,
        NULL,
        BRIGHTNESS_TASK_PRIORITY,
        &s_task_brightness_handle);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task_brightness (erro %ld)", (long)result);
        s_task_brightness_handle = NULL;
    }
}

// Permite que outras tasks sinalizem atividade do utilizador (e.g., botões).
// Reset ao temporizador de poupança assim que a navegação interage com o display.
void task_brightness_notify_user_activity(void)
{
    TaskHandle_t handle = s_task_brightness_handle;
    if (handle == NULL) {
        return;
    }

    xTaskNotify(handle, BRIGHTNESS_NOTIFY_USER_ACTIVITY, eSetBits);
}
