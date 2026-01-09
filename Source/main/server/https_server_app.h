#pragma once

/*Inicia o servidor HTTPS e regista handlers de eventos.
O servidor é arrancado automaticamente quando o SoftAP estiver a dar*/
void https_server_app_init(void);
void https_server_app_stop(void);
