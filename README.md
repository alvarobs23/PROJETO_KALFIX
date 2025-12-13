# Projeto Kalfix - Monitoramento de Produção IoT

Este projeto utiliza um Raspberry Pi Pico W para monitorar pulsos de produção, exibir contagens em tempo real em um display LCD, armazenar dados de forma persistente na memória Flash e enviar as informações para um servidor remoto via Wi-Fi.

## Funcionalidades Principais

*   **Contagem de Pulsos:** Monitoramento de sensor digital no GPIO 20 com debounce.
*   **Display Local:** Exibição de contagem, hora atual e status do turno em LCD 16x2 (I2C).
*   **Relógio em Tempo Real (RTC):** Integração com módulo DS3231 para precisão temporal e manutenção da hora sem energia.
*   **Gestão de Turnos:** Lógica automática que zera ou restaura contadores baseada em horários definidos (Turno 1, Turno 2, Intervalo).
*   **Persistência de Dados:** Salvamento automático na memória Flash interna, garantindo que a contagem não seja perdida em caso de reinício.
*   **Conectividade IoT:** Envio assíncrono ou síncrono dos dados de contagem para servidor HTTP via Wi-Fi.
*   **Estabilidade (Dual Core):** Separação de processos críticos (contagem/UI) e processos de rede/armazenamento para evitar travamentos.

## Hardware e Pinagem

*   **Microcontrolador:** Raspberry Pi Pico W
*   **Sensor de Entrada:** GPIO 20 (Configurado com Pull-up)
*   **Display LCD (I2C1):**
    *   SDA: GPIO 18
    *   SCL: GPIO 19
    *   Endereço I2C: `0x27`
*   **RTC DS3231 (I2C0):**
    *   SDA: GPIO 8
    *   SCL: GPIO 9
    *   Endereço I2C: `0x68`

## Guia de Instalação e Uso

### 1. Configuração do Firmware (Pico W)

1.  Abra o arquivo `projeto_kalfix.c` no VS Code.
2.  Localize e edite as definições de Wi-Fi e Servidor no início do código para corresponder à sua rede local:
    ```c
    #define WIFI_SSID "NOME_DA_SUA_REDE"
    #define WIFI_PASSWORD "SENHA_DA_SUA_REDE"
    #define HOST "192.168.X.X" // IP do computador onde o servidor Python está rodando
    ```
3.  Compile o projeto (utilizando a extensão "Raspberry Pi Pico" ou CMake via terminal).
4.  Conecte o Pico W ao USB segurando o botão **BOOTSEL** e copie o arquivo `.uf2` gerado para a unidade que aparecerá.

### 2. Configuração do Servidor (Python)

Para que o Pico consiga enviar os dados, o servidor deve estar rodando na mesma rede.

1.  Certifique-se de ter o Python instalado no computador.
2.  Instale as bibliotecas necessárias (Flask e SocketIO) executando no terminal:
    ```bash
    pip install flask flask-socketio eventlet
    ```
3.  Inicie o servidor executando o script Python (geralmente na pasta `web`):
    ```bash
    python server.py
    ```

## Arquitetura de Software

O sistema utiliza os dois núcleos (cores) do RP2040:

### Core 0 (Gerenciamento, Rede e Flash)
*   **Wi-Fi:** Gerencia a conexão e reconexão automática (SSID: `KALFIX`).
*   **HTTP Client:** Envia dados para o servidor configurado (`192.168.18.184:5000`).
*   **Flash Storage:** Gerencia a gravação segura na memória Flash. Utiliza `multicore_lockout` para pausar o Core 1 durante a escrita, prevenindo erros de XIP (Execute In Place).
*   **Loop Principal:** Processa solicitações de gravação vindas do Core 1 e gerencia a fila de envio de dados para a rede.

### Core 1 (Tempo Real e Interface)
*   **Loop de Contagem:** Monitora o GPIO 20 continuamente.
*   **Interface (LCD):** Atualiza a contagem e o relógio no display utilizando Mutex para acesso seguro.
*   **Lógica de Turnos:**
    *   **Turno 1:** 06:00 às 19:59
    *   **Turno 2:** 22:00 às 05:59
    *   **Intervalo:** Demais horários (Contagem pausada/zerada).
*   **RTC:** Lê a hora do DS3231 a cada segundo.
*   **Trigger de Salvamento:** Solicita ao Core 0 que salve os dados na Flash periodicamente (5s) ou por quantidade de eventos (+5), apenas se houver mudanças.

## Detalhes Técnicos

### Armazenamento (Flash)
Utiliza o último setor da Flash (`PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE`) para armazenar registros contendo:
*   Contador atual
*   Data e Hora
*   Número de sequência e CRC32 para integridade.

O sistema implementa um log sequencial dentro do setor para distribuir o desgaste (wear leveling), apagando o setor apenas quando todas as páginas estão preenchidas.

### Configuração
As configurações de rede (SSID/Senha) e servidor (IP/Porta) estão definidas via macros (`#define`) no início do arquivo `projeto_kalfix.c`.