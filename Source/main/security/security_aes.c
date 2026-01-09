// Implementa cifragem/decifragem AES-256-CBC com chave da eFuse.
// Parte do módulo de segurança usado para proteger credenciais.
#include "security_aes.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_check.h"
#include "mbedtls/aes.h"

static const char *TAG = "security_aes";

// AES-256 em modo CBC com padding PKCS#7; IV aleatório é prefixado ao ciphertext.
static uint8_t s_key[32];
// Cache da chave lida do eFuse para evitar leituras repetidas.
static bool s_key_loaded = false;

// Lê a chave AES-256 armazenada no eFuse KEY_BLOCK5.
// Usado antes de cifrar/decifrar para evitar leituras repetidas.
static esp_err_t security_aes_load_key(void)
{
    // Se a chave já foi carregada anteriormente, reutiliza o cache
    if (s_key_loaded) {
        return ESP_OK;
    }

    // Limpa o buffer da chave
    memset(s_key, 0, sizeof(s_key));
    
    // Lê a chave de 256 bits (32 bytes) do bloco KEY5 da eFuse
    esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_KEY5, s_key, sizeof(s_key) * 8U);
    if (err == ESP_OK) {
        // Leitura bem-sucedida, marca como carregada
        s_key_loaded = true;
    } else {
        // Falha ao ler a chave
        ESP_LOGE(TAG, "Falha ao ler chave AES do eFuse KEY_BLOCK5 (%s).", esp_err_to_name(err));
    }

    return err;
}

// Cifra dados com AES-256-CBC, prefixando IV e aplicando padding PKCS#7.
// Usado para guardar credenciais cifradas antes de escrever JSON no SD.
esp_err_t security_aes_encrypt(const uint8_t *plaintext, size_t len,
                               uint8_t **out_ciphertext, size_t *out_len)
{
    // Valida os parâmetros de entrada
    if (!out_ciphertext || !out_len || (len > 0 && !plaintext)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Inicializa os ponteiros de saída
    *out_ciphertext = NULL;
    *out_len = 0;

    // Garante que a chave foi carregada antes de cifrar
    ESP_RETURN_ON_ERROR(security_aes_load_key(), TAG, "Chave AES não disponível.");

    // Calcula padding PKCS#7 para alinhar aos blocos de 16 bytes
    size_t pad_len = 16U - (len % 16U);
    size_t padded_len = len + pad_len;
    size_t total_len = 16U + padded_len;   // IV + ciphertext

    // Aloca buffer para dados com padding
    uint8_t *padded = (uint8_t *)calloc(1, padded_len);
    if (!padded) {
        return ESP_ERR_NO_MEM;
    }

    // Copia dados originais e adiciona padding PKCS#7
    if (len > 0) {
        memcpy(padded, plaintext, len);
    }
    memset(padded + len, (int)pad_len, pad_len);

    // Aloca buffer de saída (IV + ciphertext)
    uint8_t *output = (uint8_t *)calloc(1, total_len);
    if (!output) {
        free(padded);
        return ESP_ERR_NO_MEM;
    }

    // Gera IV aleatório nos primeiros 16 bytes
    esp_fill_random(output, 16U);

    // Inicializa contexto AES
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    // Configura chave AES-256 para encriptação
    int ret = mbedtls_aes_setkey_enc(&ctx, s_key, 256);
    
    // Copia IV para buffer temporário (será modificado pela operação CBC)
    uint8_t iv_copy[16];
    memcpy(iv_copy, output, sizeof(iv_copy));
    
    if (ret == 0) {
        // Cifra os dados em modo CBC
        ret = mbedtls_aes_crypt_cbc(&ctx,
                                    MBEDTLS_AES_ENCRYPT,
                                    padded_len,
                                    iv_copy,
                                    padded,
                                    output + 16U);
    }

    // Limpa contexto e dados temporários
    mbedtls_aes_free(&ctx);
    free(padded);

    if (ret != 0) {
        // Erro na cifragem
        free(output);
        ESP_LOGE(TAG, "mbedTLS AES encrypt falhou (ret=%d).", ret);
        return ESP_FAIL;
    }

    // Retorna o ciphertext (IV + dados cifrados)
    *out_ciphertext = output;
    *out_len = total_len;
    return ESP_OK;
}

// Decifra dados AES-256-CBC prefixados com IV e valida padding PKCS#7.
// Utilizado ao carregar strings cifradas dos ficheiros JSON.
esp_err_t security_aes_decrypt(const uint8_t *ciphertext, size_t len,
                               uint8_t **out_plaintext, size_t *out_len)
{
    if (!ciphertext || !out_plaintext || !out_len || len <= 16U) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_plaintext = NULL;
    *out_len = 0;

    size_t data_len = len - 16U;
    if (data_len == 0 || (data_len % 16U) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Garante que a chave foi carregada antes de decifrar.
    ESP_RETURN_ON_ERROR(security_aes_load_key(), TAG, "Chave AES não disponível.");

    uint8_t *plaintext = (uint8_t *)calloc(1, data_len);
    if (!plaintext) {
        return ESP_ERR_NO_MEM;
    }

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    int ret = mbedtls_aes_setkey_dec(&ctx, s_key, 256);
    uint8_t iv[16];
    memcpy(iv, ciphertext, sizeof(iv));
    if (ret == 0) {
        ret = mbedtls_aes_crypt_cbc(&ctx,
                                    MBEDTLS_AES_DECRYPT,
                                    data_len,
                                    iv,
                                    ciphertext + 16U,
                                    plaintext);
    }

    mbedtls_aes_free(&ctx);

    if (ret != 0) {
        free(plaintext);
        ESP_LOGE(TAG, "mbedTLS AES decrypt falhou (ret=%d).", ret);
        return ESP_FAIL;
    }

    // Valida padding PKCS#7 para detetar corrupção ou chave errada.
    uint8_t pad = plaintext[data_len - 1];
    if (pad == 0 || pad > 16) {
        free(plaintext);
        ESP_LOGE(TAG, "Padding PKCS#7 inválido (valor=%u).", (unsigned)pad);
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (size_t i = 0; i < pad; ++i) {
        if (plaintext[data_len - 1 - i] != pad) {
            free(plaintext);
            ESP_LOGE(TAG, "Padding PKCS#7 corrompido.");
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    size_t plain_len = data_len - pad;
    memset(plaintext + plain_len, 0, pad);

    *out_plaintext = plaintext;
    *out_len = plain_len;
    return ESP_OK;
}
