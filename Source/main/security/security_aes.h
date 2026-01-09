#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Criptografa plaintext usando AES-256-CBC com IV prefixado no resultado. */
esp_err_t security_aes_encrypt(const uint8_t *plaintext, size_t len,
                               uint8_t **out_ciphertext, size_t *out_len);

/* Descriptografa buffers no formato IV || ciphertext AES-256-CBC. */
esp_err_t security_aes_decrypt(const uint8_t *ciphertext, size_t len,
                               uint8_t **out_plaintext, size_t *out_len);
