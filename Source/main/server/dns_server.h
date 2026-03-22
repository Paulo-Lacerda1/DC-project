#pragma once

// DNS server usado pelo SoftAP para resolver domínios locais.
// Iniciar quando o AP está ativo e parar quando o AP é desligado.
void dns_server_start(void);
void dns_server_stop(void);
