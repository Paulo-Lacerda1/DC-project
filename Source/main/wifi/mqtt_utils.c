// Funções auxiliares para obter recursos MQTT a partir do cartão SD.
// Integra o módulo de conectividade Wi-Fi/MQTT.
#include "mqtt_utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "logging.h"

static const char *TAG = "mqtt_utils";

// Lê o certificado CA armazenado no SD para memória heap.
// Chamado por task_mqtt antes de iniciar o cliente TLS.
char *mqtt_load_ca_cert_from_sd(const char *path, size_t *out_len)
{
    // Valida os parâmetros de entrada
    if (!path || !out_len) {
        log_error(TAG, "Parâmetros inválidos ao carregar certificado CA.");
        return NULL;
    }

    // Inicializa o tamanho de saída
    *out_len = 0;

    // Abre o ficheiro do certificado em modo binário
    FILE *file = fopen(path, "rb");
    if (!file) {
        log_error(TAG, "Não foi possível abrir %s (errno=%d).", path, errno);
        return NULL;
    }

    // Move o ponteiro do ficheiro para o fim para obter o tamanho
    if (fseek(file, 0, SEEK_END) != 0) {
        log_error(TAG, "Falha ao posicionar no ficheiro %s.", path);
        fclose(file);
        return NULL;
    }

    // Obtém a posição atual (tamanho do ficheiro)
    long file_size = ftell(file);
    if (file_size <= 0) {
        log_error(TAG, "Tamanho inesperado do certificado (%ld).", file_size);
        fclose(file);
        return NULL;
    }

    // Volta ao início do ficheiro para ler o conteúdo
    if (fseek(file, 0, SEEK_SET) != 0) {
        log_error(TAG, "Falha ao reiniciar leitura do certificado.");
        fclose(file);
        return NULL;
    }

    // Aloca memória para o certificado (+ 1 byte para terminador nulo)
    char *buffer = (char *)malloc((size_t)file_size + 1U);
    if (!buffer) {
        log_error(TAG, "Sem memória para ler certificado (%ld bytes).", file_size);
        fclose(file);
        return NULL;
    }

    // Lê o conteúdo completo do ficheiro
    size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);

    // Verifica se leu todos os bytes esperados
    if (bytes_read != (size_t)file_size) {
        log_error(TAG, "Leitura incompleta do certificado (lidos=%zu, esperado=%ld).",
                  bytes_read, file_size);
        free(buffer);
        return NULL;
    }

    // Adiciona terminador nulo ao buffer
    buffer[file_size] = '\0';
    
    // Define o tamanho de saída e retorna o buffer
    *out_len = (size_t)file_size;
    return buffer;
}
