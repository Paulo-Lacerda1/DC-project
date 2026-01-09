#pragma once

#include <stdbool.h>

// Flag em RAM que controla se as leituras são gravadas no SD.
extern bool g_enable_sd_logging;

void config_runtime_init(void);
bool config_runtime_get_sd_logging_enabled(void);
void config_runtime_set_sd_logging_enabled(bool enabled);
