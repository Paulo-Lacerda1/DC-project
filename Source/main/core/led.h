#pragma once
#include "board_pins.h"

// Estados do LED partilhados peo sistema
typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_NORMAL,
    LED_STATE_ERROR,
    LED_STATE_TRANSMISSION,
} led_state_t;

void led_init(void);
void led_on(void);
void led_off(void);
void led_set_state_with_reason(led_state_t state, const char *reason);
void led_set_state(led_state_t state);
led_state_t led_get_state(void);
