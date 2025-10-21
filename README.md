# Sistema de Contador de Produção com Raspberry Pi Pico W (projeto_kalfix)

Este projeto implementa um sistema completo para monitorar a produção em tempo real. Uma placa Raspberry Pi Pico W detecta sinais de um sensor (ou botão) em uma de suas portas GPIO, contabiliza os eventos e envia os dados para um servidor web. O servidor, construído com Flask e Socket.IO, armazena os dados em um banco de dados PostgreSQL e exibe as informações em um dashboard web interativo e em tempo real.

## 🚀 Funcionalidades

- **Contagem em Tempo Real:** O Pico W detecta eventos (borda de descida no pino GPIO 20) e envia a contagem atualizada instantaneamente.
- **Dashboard Web:** Uma interface web moderna exibe o contador atual, o turno de produção, histórico de contagens, metas e outras métricas.
- **Gerenciamento de Turnos:** O servidor organiza a contagem com base em turnos de trabalho pré-definidos.
- **Persistência de Dados:** Todas as contagens e informações de turno são armazenadas em um banco de dados PostgreSQL.
- **Comunicação via Wi-Fi:** O Pico W se conecta à rede local via Wi-Fi para se comunicar com o servidor.
- **Análise Histórica:** O dashboard apresenta gráficos para análise de produção e perdas ao longo do tempo.

## 🛠️ Arquitetura

O sistema é composto por três partes principais:

1.  **Firmware (Raspberry Pi Pico W):**
    - Escrito em C/C++.
    - Conecta-se a uma rede Wi-Fi.
    - Monitora o pino `GPIO 20` para detectar sinais (quando o pino vai de HIGH para LOW).
    - A cada sinal detectado, incrementa um contador e envia o valor total para o servidor via uma requisição HTTP GET.

2.  **Backend (Servidor Flask):**
    - Escrito em Python usando o framework Flask.
    - Recebe os dados do Pico W através de um endpoint `/update`.
    - Gerencia a lógica de turnos com base no horário.
    - Armazena e recupera os dados de um banco de dados PostgreSQL.
    - Usa Socket.IO para enviar atualizações em tempo real para todos os clientes (navegadores) conectados.

3.  **Frontend (Dashboard Web):**
    - Construído com HTML, CSS e JavaScript.
    - Conecta-se ao servidor via Socket.IO para receber atualizações.
    - Exibe os dados de forma visual, com contadores, gráficos e tabelas de histórico.

## ✅ Pré-requisitos

Antes de começar, garanta que você tenha o seguinte:

### Hardware
- Raspberry Pi Pico W.
- Um computador para rodar o servidor Flask (Windows, macOS ou Linux).
- Um sensor com saída digital ou um botão (para conectar ao Pico W).
- Cabo Micro-USB.

### Software
- **Python 3.8+** e **pip**.
- **PostgreSQL** (versão 12 ou superior) instalado e em execução.
- **Git** para clonar o repositório.
- **Ambiente de desenvolvimento C/C++ para Raspberry Pi Pico:**
  - Pico C/C++ SDK
  - CMake
  - Compilador ARM GCC
  - (Recomendado) Visual Studio Code com a extensão Raspberry Pi Pico/RP2040.

## ⚙️ Guia de Instalação e Configuração

Siga os passos abaixo para configurar e executar o projeto.

### 1. Clone o Repositório

```bash
git clone <URL_DO_SEU_REPOSITORIO>
cd projeto_kalfix
```

### 2. Configuração do Backend (Servidor Flask)

Nesta etapa, vamos configurar o servidor web que receberá os dados do Pico.

1.  **Navegue até a pasta `web`:**
    ```bash
    cd web
    ```

2.  **Crie e ative um ambiente virtual Python:**
    ```bat
    rem No Prompt de Comando (cmd.exe) do Windows:
    python -m venv venv
    venv\Scripts\activate
    ```

3.  **Instale as dependências Python:**
    ```bash
    pip install -r requirements.txt
    ```

4.  **Configure o Banco de Dados PostgreSQL:**
    - Crie um banco de dados para o projeto. Ex: `dados_contagem`.
    - Crie um usuário e senha para acessar este banco de dados.

5.  **Configure as Variáveis de Ambiente:**
    - Na pasta `web`, crie um arquivo chamado `.env`.
    - Adicione as seguintes variáveis, substituindo pelos seus valores.
    - **Importante:** Use `0.0.0.0` para o `FLASK_SERVER_HOST`. Isso faz com que o servidor aceite conexões de qualquer dispositivo na sua rede.

    ```ini
    # Exemplo de arquivo .env
    DATABASE_URL="postgresql://SEU_USUARIO:SUA_SENHA@localhost:5432/dados_contagem"
    DB_NAME="dados_contagem"
    FLASK_SERVER_HOST="0.0.0.0" # Deixe 0.0.0.0 para escutar em todas as interfaces de rede
    FLASK_SERVER_PORT=5000
    ```
    > Para descobrir o IP que você deve usar no firmware do Pico, abra o `cmd` e digite `ipconfig`. Procure pelo "Endereço IPv4" do seu adaptador de Wi-Fi ou Ethernet.

6.  **Inicie o Servidor:**
    ```bash
    python server.py
    ```
    - O servidor iniciará e criará as tabelas no banco de dados automaticamente.
    - Acesse `http://SEU_IP:5000` em um navegador na mesma rede para ver o dashboard.

### 3. Configuração do Firmware (Raspberry Pi Pico W)

Agora, vamos configurar e gravar o código na sua placa.

1.  **Abra o arquivo `projeto_kalfix.c`:**
    - Localize o arquivo na raiz do projeto.

2.  **Modifique as Configurações de Rede e Servidor:**
    - Altere `WIFI_SSID` e `WIFI_PASSWORD` para corresponder à sua rede Wi-Fi.
    - Altere `HOST` para o **endereço IP local do seu computador** (o que você encontrou com o comando `ipconfig`).

    ```c
    // c:\Users\alvaro\kalfix\projeto_kalfix\projeto_kalfix.c

    // ...
    // Configurações de rede (MODIFICAR)
    #ifndef WIFI_SSID // <-- MODIFIQUE OS VALORES DENTRO DAS ASPAS
    #define WIFI_SSID "NOME_DA_SUA_REDE_WIFI"
    #endif
    #ifndef WIFI_PASSWORD  
    #define WIFI_PASSWORD "SENHA_DA_SUA_REDE_WIFI"
    #endif

    // Configurações do servidor (MODIFICAR)
    #define HOST        "192.168.1.10" // COLOQUE AQUI O IP DO SEU COMPUTADOR
    #define PORT        5000
    // ...
    ```

3.  **Compile e Grave o Firmware:**
    - Se estiver usando VS Code com a extensão do Pico, o processo é simplificado:
      - Selecione o kit de compilação correto (GCC for arm-none-eabi).
      - Clique em "Build" na barra de status.
      - Coloque o Pico em modo BOOTSEL (segure o botão BOOTSEL e conecte o cabo USB).
      - Clique em "Upload" para gravar o firmware.
    - Alternativamente, siga a documentação oficial para compilar e gravar via linha de comando.

## ▶️ Uso

1.  Garanta que o servidor Flask esteja em execução.
2.  Conecte a Raspberry Pi Pico W à energia. Ela se conectará ao Wi-Fi e começará a monitorar o pino `GPIO 20`.
3.  Acesse o dashboard no seu navegador (`http://IP_DO_SERVIDOR:5000`).
4.  Conecte seu sensor ou botão ao `GPIO 20` e ao `GND`. Cada vez que o pino `GPIO 20` for para o nível lógico baixo (LOW), o contador será incrementado e o dashboard será atualizado em tempo real.