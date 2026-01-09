#pragma once

#include "driver/gpio.h"

// Barramento I2C partilhado pelos sensores
#define PIN_I2C_SDA   GPIO_NUM_6        //GPIO6
#define PIN_I2C_SCL   GPIO_NUM_7        //GPIO7

// Ligações do cartão microSD (SPI)
#define PIN_SD_SCK    GPIO_NUM_21
#define PIN_SD_MOSI   GPIO_NUM_19
#define PIN_SD_MISO   GPIO_NUM_20
#define PIN_SD_CS     GPIO_NUM_18

// LED RGB de estado
#define LED_RGB_GPIO  GPIO_NUM_8

// Display ST7735 + potenciometro
#define PIN_TFT_MOSI  GPIO_NUM_19
#define PIN_TFT_SCLK  GPIO_NUM_21
#define PIN_TFT_CS    GPIO_NUM_22
#define PIN_TFT_DC    GPIO_NUM_2
#define PIN_TFT_RST   GPIO_NUM_3
#define PIN_TFT_LIT   GPIO_NUM_15
#define PIN_POT_ADC   GPIO_NUM_0

// Botão físico (StandBY)
#define PIN_BUTTON      GPIO_NUM_10

// Botão para alternar o ecrã do display
#define SCREEN_BUTTON_GPIO GPIO_NUM_23
