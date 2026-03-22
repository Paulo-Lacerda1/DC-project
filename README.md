# Projeto Final de DC
Paulo Lacerda - 120202  
Gabriel Marta - 120155

Todas as comprovações de funcionamento encontram-se na pasta `Evidencias/...`.

## Semana: 5

**Contribuições:**  
120202: 50% | 120155: 50%

### Progresso:

**Display TFT com múltiplos ecrãs** — (concluído)  
Foi desenvolvida toda a interface gráfica do dispositivo, baseada no ecrã TFT ST7735.  
Foram implementados quatro ecrãs distintos, navegáveis através do botão físico:

- **Ecrã de boas-vindas** com mensagem inicial centrada.  
- **Ecrã principal**, mostra temperatura, humidade e estado do sensor em formato de tabela.  
- **Ecrã de logs**, imprime até 5 mensagens de logs
- **Ecrã de gráfico**, mostra a evolução da temperatura e humidade em tempo-real, com eixos assinalados por cores.  

Quando o botão de Standy é pressionado aparece no TFT que o sensor está em STANDBY e nos valores aparece STOP, ambos a vermelho.  

**Gestão de navegação entre ecrãs** — (concluída)  
O botão alterna ciclicamente pelos quatro modos, mantendo o estado interno entre transições.

**Implementação de estratégia de redução de consumo de energia** — (concluída)  
Agora o LED RGB em vez de estar sempre ligado ele fica 100ms ligado e 900 ms desligado. Esses valores são alteráveis (#define LED_BLINK_ON_MS   100  &&    #define LED_BLINK_OFF_MS  900)


## Semana: 4

**Contribuições:**  
120202: 50% | 120155: 50%

### Progresso:

**Server web** — (conluída)  
Agora dá para ver a lista dos dispositivos ligados ao SoftAP.  
Criamos um server HTTP, em que só dá para entrar pela rede ESP32_Config. Nessa server Web dá para alterar a rede que a placa vai se ligar. 

**Polling microSD** — (concluída)  
Agora o task do sensor passa a ler do microSD apenas quando chega uma notificação de alteração. Evitando assim consultas ao microSD desnecessárias. 

**Integridade dos logs (HMAC)** — (concluída)  
Cada linha do system.log é assinada com HMAC-SHA256 usando a chave secreta do eFuse, permitindo validar a autenticidade dos registos fora do dispositivo.

**Assinatura das mensagens MQTT (HMAC)** — (concluída)  
Cada publicação MQTT leva um HMAC-SHA256 calculado sobre " uid|temperatura|humidade" com a chave no eFuse. Se não baterem certo o sistema aborta a publicação em falha.

**Escrita no cartão com AES (password wifi)** — (concluída)  
Pelo server WEB é possivel escrever o SSID e pwd da rede a que se vai ligar. A escrita no cartão é feita através de encriptação por AES, de modo a evitar que as pessoas que tenha o microSD não consiga ver a password.


*Extra:*  

**Toggle de logging no SD** — (concluída)  
O dashboard HTTPS mostra um switch “Registo em cartão SD” que envia POST para /update_config, atualiza g_enable_sd_logging em RAM sem reboot, a task do sensor só escreve em data.txt quando o toggle está ligado e por fim regista “SD logging ON/OFF” no log interno do microSD. Também adicionamos um cartão de status para o ficheiro data.txt

---

## Semana: 3

**Contribuições:**  
120202: 50% | 120155: 50%

### Progresso:

**Broker MQTT funcional** — (concluída)  
Sistema MQTT com TLS operacional. Ligações estáveis e publicações confirmadas. O sistema obtém o certificado pelo ficheiro mosquitto.crt que está localizado no microSD.

**Alteração do período via servidor HTTPS** — (concluída)  
Antes só era possivel ver as temperaturas e humidade, agora é possivel com que o período de leitura seja atualizado pelo servidor, guardado no microSD e aplicado pela task_sensor.

**Upgrade no servidor HTTPS** — (concluída)  
Mais informações no server. Ex: Status de cada componente (Sensor, MQTT, microSD,...) 

*Extra:*

**Implementação do botão de standby** — (concluída)  
O objetivo desta funcionalidade pode ser alterada. Neste momento o botão físico ligado ao GPIO10 que suspende completamente o funcionamento da task_sensor enquanto está premido e retoma assim que é largado. Mais para a frente podemos usar este botão para outras funções, como por exemplo, ligar/desligar o sensor, mudar o modo de consumo de energia, etc...
