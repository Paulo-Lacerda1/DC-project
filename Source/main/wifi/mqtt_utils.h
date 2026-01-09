#pragma once

#include <stddef.h>

char *mqtt_load_ca_cert_from_sd(const char *path, size_t *out_len);
