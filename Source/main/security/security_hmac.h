#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
data -dados a processar (pode ser NULL apenas se len for 0).
len- tamanho dos dados em bytes.
out_digest- buffer de 32 bytes onde o digest será escrito.
*/
esp_err_t security_hmac_compute(const uint8_t *data, size_t len, uint8_t out_digest[32]);
