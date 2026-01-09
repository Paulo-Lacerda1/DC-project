# Estado atual (funcionalidades implementadas)

## Visão geral
- Firmware para ESP32 que reúne leitura de ambiente (DHT20), interface local via TFT ST7735, registo em microSD, configuração Wi‑Fi/MQTT e dashboards web seguros.
- A aplicação funciona sempre em modo AP+STA: liga‑se à rede configurada e mantém um SoftAP “ESP_Config” para provisão local.
- Todos os serviços (portal, HTTPS, MQTT, display, logging) partilham o mesmo pipeline de dados e são coordenados pelas tasks FreeRTOS descritas abaixo.

## Arquitetura e fluxo de tarefas
- `app_main` inicializa `system_status`, `config_runtime`, logging assinado no SD, LED RGB, Wi‑Fi/SoftAP, servidores HTTP/HTTPS e cria as tasks (`task_config_loader`, `task_sensor`, `task_display`, `task_brightness`, `task_button`, `task_button_screen`, `task_mqtt`).
- `task_config_loader` serializa o acesso ao SD via `config_manager`: monta o cartão, garante que `config_user.json` e `config_manufacturer.json` existem, lê/valida período do sensor e aplica alterações sem reiniciar.
- `sensor_data_store` guarda a última leitura (temp/hum + timestamp) com mutex; `task_sensor`, `task_mqtt`, `task_display` e `/api/sensor` consomem estes dados sem mexer em globais.
- `system_status` mantém um snapshot único (Wi‑Fi STA + SoftAP, MQTT, SD, uptime, UID, standby) que alimenta o dashboard HTTPS, o portal e o display.
- `config_runtime` guarda estados dinâmicos não persistentes, como o toggle “Registo no SD”, usados por `task_sensor` e pelo dashboard.

## Sensores e interface local

### Sensor DHT20
- A `task_sensor` lê temperatura e humidade conforme o período definido em `config_user.json`; o valor é atualizado dinamicamente via `/set_period` ou pela task de configuração sem reiniciar.
- A cada ciclo valida se o cartão SD continua montado (`config_manager_check_sd_alive`) e suspende leituras/logging caso haja erro ou período inválido, colocando o LED em vermelho até haver configuração válida.
- Após uma leitura válida, atualiza o `sensor_data_store`, imprime no terminal, escreve opcionalmente no `data.txt` (CSV com timestamp, temperatura, humidade) e notifica a `task_display` e a `task_mqtt`.
- Se o SD for removido, para o HTTPS server, desliga Wi‑Fi STA, mostra aviso no TFT (“MicroSD removido”) e bloqueia até o cartão voltar a ficar disponível ou o sistema reiniciar segundo as flags do `config_manager`.

### Display TFT e botões de navegação
- `task_display` inicializa o ST7735 partilhando o bus SPI com o SD, mostra um ecrã de boas‑vindas e passa a atualizar três vistas: tabela principal (temp/hum/estado), histórico de logs e gráfico das 10 últimas amostras.
- Os logs aparecem no ecrã após sanitização para ASCII (`display_add_log`), permitindo ver eventos críticos mesmo sem consola.
- A `task_button_screen` lê o botão dedicado ao TFT, com debounce, e alterna pela sequência de ecrãs (principal → logs → gráfico), notificando o display sempre que o utilizador pressiona o botão.
- Quando o SD é removido a UI troca para o aviso dedicado; quando o sistema entra em standby, as linhas principais mostram “STOP” e o estado passa para “STANDBY”.

### Medição de brilho (potenciómetro)
- `task_brightness` usa o ADC One‑Shot no `PIN_POT_ADC` com calibração (curve fitting ou line fitting consoante o que o IDF suportar) para obter leitura estável em mV.
- Sempre que o valor varia acima do `BRIGHTNESS_CHANGE_THRESHOLD`, imprime no terminal a leitura RAW, tensão aproximada e percentagem calculada e ajusta o PWM do backlight (`PIN_TFT_LIT`) via LEDC (5 kHz, 10 bits). Se não houver alterações ≥5% durante 10 s (`BRIGHTNESS_IDLE_STAGE1_TIMEOUT_MS`), força um modo de poupança a 20% e, após mais 5 s de inatividade, desliga o backlight; qualquer variação volta a aplicar o duty‑cycle proporcional ao potenciómetro.
- Se a calibração não estiver disponível, cai para um modo proporcional simples e mantém os logs para diagnóstico.

### LED RGB e botão de standby
- O LED endereçável é controlado por RMT e indica o estado: verde = operação normal, amarelo = transmissão MQTT, vermelho = erro (período inválido, SD ausente, standby forçado ou falha no sensor).
- `task_button` monitoriza o botão físico em `GPIO10`; ao pressionar envia notificação para `task_sensor` entrar em standby (suspende DHT20, display e MQTT, mantém logs) e ao libertar retoma imediatamente, restaurando o LED ao estado anterior.
- O estado de standby fica disponível em `system_status` e é mostrado tanto no display como no dashboard.

## Cartão microSD, configuração e logging
- `config_user.json` (periodo, Wi‑Fi, MQTT) e `config_manufacturer.json` (UID usado nas assinaturas) vivem em `/sdcard`; se não existirem são criados com valores seguros e placeholders.
- As credenciais sensíveis (`wifi_pass`, `mqtt_password`, `mqtt_uri`) são guardadas cifradas (AES‑256‑CBC + Base64); sempre que um campo plaintext é lido é imediatamente removido/regravado cifrado.
- `config.json` é lido pelo portal HTTP ao receber novas credenciais; `save_config_to_json()` e `config_manager_set_period_seconds()` persistem alterações pedidas pelo dashboard.
- `system.log` é gravado em `/sdcard/system.log`; cada linha tem `timestamp | level | tag | mensagem | HMAC=<sha256>` onde o HMAC é calculado com a chave secreta no eFuse KEY_BLOCK5. O ficheiro é criado automaticamente, truncado no arranque e o `system_status` acompanha nº de entradas, último timestamp e espaço livre.
- O ficheiro de dados do sensor (`/sdcard/data.txt`) é limpo no arranque de `task_sensor` e só recebe amostras quando o toggle “Registo no SD” está ativo; falhas consecutivas ao abrir/escrever fazem `mark_sd_card_unavailable()` para obrigar à intervenção do utilizador.
- `task_config_loader` garante que apenas uma task mexe no SD de cada vez e responde via notificação ao chamador (`task_sensor`, portal, dashboard), evitando corridas mesmo quando o cartão é removido e inserido em runtime.

## Wi‑Fi (STA + SoftAP)
- O dispositivo liga‑se como STA usando `wifi_ssid`/`wifi_pass` definidos no SD e simultaneamente inicia o SoftAP `ESP_Config` com password `dispositivos` e IP fixo `192.168.4.1`.
- O `config_manager` registra handlers para todos os eventos Wi‑Fi/IP: quando a STA liga/desliga atualiza `system_status` (RSSI, IP, estado) e imprime na consola o motivo (`WRONG_PASSWORD`, `NO_AP_FOUND`, etc.); quando clientes entram/saiem do SoftAP o log mostra MAC, IP atribuído e AID.
- A lista de clientes do SoftAP é mantida numa tabela interna e exportada via `config_manager_get_softap_clients()` para o dashboard HTTPS.
- Assim que a STA obtém IP o SNTP é iniciado (`pool.ntp.org`), garantindo timestamps válidos para logs e assinaturas.

## Portal HTTP de configuração (SoftAP)
- Disponível apenas através do SoftAP em `http://192.168.4.1/`, serve `config_portal.html` e três rotas AJAX: `GET /status` devolve `state` (`idle/connecting/connected/error`), IP atual, SSID guardado e mensagens de estado; `POST /wifi` recebe SSID/password, valida ASCII e guarda em `config_user.json`; `POST /reset` agenda um restart remoto controlado.
- `POST /reset` permite pedir remotamente um restart seguro da placa; o handler agenda o `esp_restart()` numa task dedicada para que a resposta HTTP seja enviada antes do reboot físico.
- Após cada POST o portal marca o estado “connecting”, força a STA a aplicar as novas credenciais (`config_manager_apply_sta_credentials`) e atualiza o texto com sucesso/erro. Estes estados também são logados para auditoria.
- O servidor liga/desliga automaticamente quando o SoftAP é iniciado/parado e ignora pedidos vindos de fora da rede AP.

## Servidor HTTPS / Dashboard
- Arranca quando a STA obtém IP válido; usa certificados embebidos em `server/certs` e corre em `https://<ip_da_STA>/`. Todos os pedidos provenientes do SoftAP recebem `403` para garantir que apenas clientes autenticados via STA acedem ao dashboard seguro.
- Páginas e APIs expostas:
  - `GET /` → Dashboard em tempo real com cartões de Wi‑Fi (STA + lista de SoftAP), MQTT (tentativas, falhas, mensagens enviadas e em buffer), SD (estado, espaço livre, nº de logs e dados), uptime, versão do firmware (retirada de `esp_app_desc`) e UID do dispositivo. Inclui um toggle “Registo no SD” que reflete `config_runtime_get_sd_logging_enabled()` e botões para atualizar período.
  - `GET /api/sensor` → JSON com última leitura e período atual.
  - `GET /api/status` → snapshot completo (`system_status` + clientes SoftAP).
  - `POST /set_period` → recebe `period=<segundos>` (1–3600), valida, grava no SD, loga o IP/MAC do cliente e notifica a `task_sensor`.
  - `POST /update_config` → liga/desliga o registo de `data.txt`, atualiza `config_runtime` + `system_status` e regista no log quem alterou.
- Todos os handlers suportam respostas JSON (quando o cliente envia `fetch`/`Accept: application/json`) ou HTML simples de feedback para submits tradicionais. Se o SD for removido o servidor é parado para evitar acessos inseguros.

## Partilha e consumo dos dados
- `sensor_data_store_set()` guarda temperatura, humidade e timestamp; `sensor_data_store_get()` devolve a última amostra para o HTTPS server e display, enquanto `sensor_get_last_reading()` fornece à MQTT task valores com tempo em ms para assinatura.
- `system_status_snapshot()` agrega, além das métricas de Wi‑Fi/MQTT/SD, informação geral (uptime em segundos, versão do firmware, UID) e o estado de standby ativo; o dashboard HTTPS e o portal utilizam este snapshot para mostrar dados consistentes.
- Como cada consumidor trabalha sobre estas APIs thread-safe, nenhuma task toca diretamente em variáveis globais do sensor ou do Wi‑Fi.

## Broker MQTT por TLS
- `task_mqtt_start()` lê `mqtt_uri`, `mqtt_topic`, `mqtt_topic_events`, `client_id`, `username` e `password` de `config_user.json`, carrega a CA do broker de `/sdcard/certs/mosquitto.crt` e inicia o `esp_mqtt_client` com TLS mutual trust.
- As leituras são publicadas no tópico configurado (por defeito `data`) com QoS 1. Antes de enviar, `task_mqtt` carrega o UID (`config_manager_get_device_uid`), monta a string `uid|temperatura|humidade`, calcula o HMAC‑SHA256 com a chave do eFuse e inclui o digest em hexadecimal no payload.
- Enquanto aguarda PUBACK, o LED fica amarelo (`LED_STATE_TRANSMISSION`); ao receber confirmação regressa ao estado estável anterior. Falhas incrementam `system_status.mqtt.fail_count`.
- Eventos de log (`log_enqueue_event`) também são enviados para `mqtt_topic_events`: cada item contém timestamp (sincronizado via SNTP), nível, módulo e mensagem escapada, além de um HMAC‑SHA256 calculado sobre o payload canónico para garantir integridade no tópico `eventos`.
- Reconexões seguem backoff exponencial (1–8 s) e o estado `connected/buffered messages` fica visível no dashboard e no portal.

## Logging, auditoria e partilha remota
- O subsistema `logging` escreve INFO/WARN/ERROR em `/sdcard/system.log` com HMAC e faz mirror no display (ecrã de logs) e na fila de eventos para MQTT. Se o SD falhar definitivamente, mostra um banner no terminal e mantém logs apenas na RAM/consola.
- `system_status` acompanha quantidade de logs e dados gravados, último timestamp e se o SD está montado. Estes valores são mostrados no dashboard HTTPS para que o utilizador saiba quando o cartão precisa de manutenção.
- O toggle “Registo no SD” pode ser controlado via dashboard (`/update_config`); quando ativo, `task_sensor` acrescenta linhas no `data.txt`. Em caso de duas falhas consecutivas ao abrir/escrever, o SD é marcado como indisponível para evitar corrupção.
- Alterações críticas (período, toggle do logging, tentativas de ligação Wi‑Fi) registam IP/MAC do cliente onde é possível, garantindo rastreabilidade completa.

## Segurança e integridade
- As chaves AES‑256 e HMAC residem no eFuse KEY_BLOCK5. O firmware nunca grava segredos em claro no SD: credenciais e URIs são cifradas e apenas desencriptadas na RAM.
- Toda a comunicação remota sensível é feita sobre TLS: MQTT usa CA lida do SD e o dashboard usa o certificado embebido; pedidos oriundos do SoftAP são bloqueados no HTTPS.
- As linhas do log e as mensagens MQTT levam assinaturas HMAC verificáveis fora do dispositivo; qualquer alteração é detetável.
- Os handlers validam entradas (ASCII, tamanho mínimo/máximo, intervalo de período), evitam injecções JSON e registam tentativas inválidas. O SNTP mantém o relógio sincronizado para que logs e assinaturas tenham timestamps fiáveis.

## Gestão de standby e falhas do microSD
- O botão em `GPIO10` ativa standby físico: `task_sensor` pausa loops, o display mostra “STOP”, o LED passa para “erro/mode standby” e `system_status_set_standby_active(true)` atualiza portal/dashboard. Ao libertar, a notificação reativa sensores, MQTT e display de imediato.
- `task_sensor` monitoriza continuamente o SD; se for removido, invalida leituras, pára o HTTPS server, tenta parar o Wi‑Fi para poupar energia e entra num loop de remontagem com supressão de logs ruidosos. Assim que o cartão regressa, o ficheiro de dados é limpo, as stats do dashboard são reiniciadas e, se necessário, força `esp_restart()`.
- Este mecanismo evita corrupção, garante que apenas dados consistentes chegam ao MQTT/HTTPS e dá feedback claro (LED vermelho + mensagem no TFT + linhas no log) para o utilizador corrigir a falha.
