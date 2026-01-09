// Calcula HMAC-SHA256 com chave secreta guardada no eFuse.
// Parte do módulo de segurança usado para assinar mensagens MQTT.
#include "security_hmac.h"

#include <stdbool.h>
#include <string.h>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "mbedtls/md.h"

static const char *TAG = "security_hmac";
static uint8_t s_key[32];
static bool s_key_loaded = false;

// Lê a chave de 256 bits do KEY_BLOCK5 e mantém-na em RAM sem expor.
// Executado apenas na primeira utilização para evitar acessos repetidos ao eFuse.
static esp_err_t security_hmac_load_key(void)
{
    // Verifica se a chave já foi carregada anteriormente
    if (s_key_loaded) {
        return ESP_OK;
    }

    // Limpa o buffer da chave antes de ler
    memset(s_key, 0, sizeof(s_key));
    
    // Lê a chave de 256 bits (32 bytes × 8 bits) do bloco de eFuse KEY5
    esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_KEY5, s_key, sizeof(s_key) * 8U);
    if (err == ESP_OK) {
        // Marca a chave como carregada para evitar releituras
        s_key_loaded = true;
    } else {
        // Regista erro se a leitura do eFuse falhar
        ESP_LOGE(TAG, "Falha ao ler chave HMAC do eFuse KEY_BLOCK5 (%s).", esp_err_to_name(err));
    }

    return err;
}

// Calcula o HMAC-SHA256 de um buffer usando a chave carregada do eFuse.
// Usado para assinar leituras publicadas ou strings sensíveis.
esp_err_t security_hmac_compute(const uint8_t *data, size_t len, uint8_t out_digest[32])
{
    // Valida os parâmetros de entrada
    if (!out_digest || (len > 0 && !data)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Garante que a chave HMAC está carregada do eFuse
    esp_err_t err = security_hmac_load_key();
    if (err != ESP_OK) {
        return err;
    }

    // Obtém as informações do algoritmo SHA-256 do mbedTLS
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) {
        return ESP_FAIL;
    }

    // Inicializa o contexto de digest
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    // Configura o contexto para modo HMAC (último parâmetro = 1)
    int ret = mbedtls_md_setup(&ctx, md_info, 1);
    if (ret == 0) {
        // Inicia o cálculo HMAC com a chave secreta
        ret = mbedtls_md_hmac_starts(&ctx, s_key, sizeof(s_key));
    }
    if (ret == 0 && data && len > 0) {
        // Processa os dados de entrada
        ret = mbedtls_md_hmac_update(&ctx, data, len);
    }
    if (ret == 0) {
        // Finaliza o cálculo e obtém o digest
        ret = mbedtls_md_hmac_finish(&ctx, out_digest);
    } else {
        // Em caso de erro, limpa o buffer de saída
        memset(out_digest, 0, 32);
    }

    // Liberta os recursos alocados pelo contexto
    mbedtls_md_free(&ctx);

    // Verifica se houve erro no processo
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedTLS HMAC falhou (ret=%d).", ret);
        err = ESP_FAIL;
    }

    return err;
}
