#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sensor_dht20.h"

/* Inicializa o mecanismo de partilha dos dados do sensor. */
void sensor_data_store_init(void);

/* Atualiza o valor de temperatura/humidade disponível para o resto do sistema. */
void sensor_data_store_set(float temperature, float humidity);

/* Marca os dados como indisponíveis (por exemplo, em caso de erro de leitura). */
void sensor_data_store_invalidate(void);

/* Copia a leitura mais recente para o caller.
 * @param[out] out Estrutura que recebe temperatura/humidade.
 * @return true se os dados são válidos, false caso contrário. */
bool sensor_data_store_get(sensor_data_t *out);

typedef struct {
    float temperatura_c;
    float humidade_rh;
    uint64_t timestamp_ms;
} sensor_reading_t;

bool sensor_get_last_reading(sensor_reading_t *out);
