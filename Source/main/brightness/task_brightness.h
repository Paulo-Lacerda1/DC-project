#pragma once

#include "esp_err.h"

//Inicializa o canal PWM responsável pelo brilho.

esp_err_t brightness_pwm_init(void);

//Cria e arranca a tarefa de controlo de brilho.
void task_brightness_start(void);

//Notifica atividade do utilizador para reiniciar o temporizador de inatividade.
void task_brightness_notify_user_activity(void);
