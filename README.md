# 🏭 Sistema de Contador por Turnos - Projeto Kalfix

Um sistema completo de monitoramento e contagem de eventos em tempo real, desenvolvido para ambientes industriais com controle por turnos. O projeto combina hardware embarcado (Raspberry Pi Pico W) com uma aplicação web moderna para fornecer monitoramento contínuo e análise histórica.

## 📋 Índice

- [Visão Geral](#-visão-geral)
- [Arquitetura do Sistema](#-arquitetura-do-sistema)
- [Funcionalidades](#-funcionalidades)
- [Tecnologias Utilizadas](#-tecnologias-utilizadas)
- [Estrutura do Projeto](#-estrutura-do-projeto)
- [Instalação e Configuração](#-instalação-e-configuração)
- [Uso](#-uso)
- [API Endpoints](#-api-endpoints)
- [Configuração de Rede](#-configuração-de-rede)
- [Banco de Dados](#-banco-de-dados)
- [Monitoramento](#-monitoramento)
- [Troubleshooting](#-troubleshooting)
- [Contribuição](#-contribuição)
- [Licença](#-licença)

## 🎯 Visão Geral

O Sistema de Contador por Turnos é uma solução IoT completa que monitora eventos físicos através de sensores conectados ao Raspberry Pi Pico W, enviando dados em tempo real para um servidor web que gerencia turnos de trabalho e armazena histórico em banco de dados PostgreSQL.

### Principais Características:
- ⚡ **Monitoramento em Tempo Real**: Detecção instantânea de eventos via GPIO
- 🔄 **Gestão Automática de Turnos**: Sistema inteligente de turnos (06:00-16:00 e 22:00-06:00)
- 📊 **Dashboard Interativo**: Interface web moderna com gráficos e análises
- 💾 **Persistência de Dados**: Armazenamento seguro em PostgreSQL
- 🌐 **Comunicação Wi-Fi**: Conectividade sem fio para flexibilidade de instalação
- 📱 **Interface Responsiva**: Acesso via qualquer dispositivo com navegador

## 🏗️ Arquitetura do Sistema

```
┌─────────────────┐    Wi-Fi    ┌─────────────────┐    HTTP    ┌─────────────────┐
│                 │ ──────────► │                 │ ─────────► │                 │
│ Raspberry Pi    │             │ Servidor Flask  │            │ PostgreSQL      │
│ Pico W          │             │ + Socket.IO     │            │ Database        │
│                 │             │                 │            │                 │
│ • GPIO Monitor  │             │ • API REST      │            │ • Turnos        │
│ • Wi-Fi Client  │             │ • WebSocket     │            │ • Histórico     │
│ • HTTP Client   │             │ • Dashboard     │            │ • Contadores    │
└─────────────────┘             └─────────────────┘            └─────────────────┘
```

## ✨ Funcionalidades

### Hardware (Raspberry Pi Pico W)
- **Monitoramento GPIO**: Detecção de sinais HIGH/LOW no pino 20
- **Debounce Inteligente**: Filtragem de ruídos para evitar contagens falsas
- **Conectividade Wi-Fi**: Conexão automática à rede configurada
- **Comunicação HTTP**: Envio de dados para servidor em tempo real
- **LED de Status**: Indicador visual de funcionamento

### Software (Servidor Web)
- **Dashboard em Tempo Real**: Interface moderna com atualizações instantâneas
- **Gestão de Turnos**: Controle automático baseado em horários
- **Análise Histórica**: Gráficos interativos por período (dia/semana/mês/ano)
- **Persistência de Dados**: Armazenamento seguro em PostgreSQL
- **API REST**: Endpoints para integração com outros sistemas
- **WebSocket**: Comunicação bidirecional em tempo real

## 🛠️ Tecnologias Utilizadas

### Hardware
- **Raspberry Pi Pico W**: Microcontrolador ARM Cortex-M0+ com Wi-Fi
- **Pico SDK**: Framework oficial para desenvolvimento
- **C/C++**: Linguagem de programação para firmware
- **GPIO**: Interface de entrada/saída para sensores

### Backend
- **Python 3.8+**: Linguagem principal
- **Flask**: Framework web minimalista
- **Flask-SocketIO**: WebSocket para comunicação em tempo real
- **PostgreSQL**: Banco de dados relacional
- **psycopg2**: Driver PostgreSQL para Python

### Frontend
- **HTML5/CSS3**: Estrutura e estilização
- **JavaScript ES6+**: Lógica de interface
- **Chart.js**: Biblioteca de gráficos interativos
- **Socket.IO Client**: Cliente WebSocket
- **Design Responsivo**: Interface adaptável a diferentes dispositivos

### DevOps
- **CMake**: Sistema de build para firmware
- **Git**: Controle de versão
- **Environment Variables**: Configuração segura

## 📁 Estrutura do Projeto

```
projeto_kalfix/
├── 📁 build/                          # Arquivos de compilação
├── 📁 images/                         # Documentação visual
├── 📁 web/                            # Aplicação web
│   ├── 📄 app.py                      # Aplicação Flask principal
│   ├── 📄 config.py                   # Configurações
│   ├── 📄 database.py                # Gerenciamento de banco
│   ├── 📄 server.py                   # Servidor principal
│   └── 📁 templates/
│       └── 📄 index.html              # Interface web
├── 📄 CMakeLists.txt                  # Configuração de build
├── 📄 projeto_kalfix.c               # Firmware principal
├── 📄 example_http_client_util.c     # Utilitários HTTP
├── 📄 example_http_client_util.h     # Headers HTTP
├── 📄 lwipopts.h                      # Configurações LwIP
├── 📄 mbedtls_config.h               # Configurações TLS
└── 📄 README.md                       # Este arquivo
```

## 🚀 Instalação e Configuração

### Pré-requisitos

#### Hardware
- Raspberry Pi Pico W
- Cabo USB-C para programação
- Sensor/conector para GPIO 20
- Fonte de alimentação adequada

#### Software
- **Para Firmware**:
  - Pico SDK 1.5.1+
  - CMake 3.13+
  - Compilador ARM GCC
  - picotool 2.0.0+

- **Para Servidor Web**:
  - Python 3.8+
  - PostgreSQL 12+
  - pip (gerenciador de pacotes Python)

### Instalação do Firmware

1. **Clone o repositório**:
```bash
git clone <url-do-repositorio>
cd projeto_kalfix
```

2. **Configure as credenciais Wi-Fi** no `CMakeLists.txt`:
```cmake
set(WIFI_SSID "Sua_Rede_WiFi")
set(WIFI_PASSWORD "Sua_Senha_WiFi")
```

3. **Configure o IP do servidor** no `projeto_kalfix.c`:
```c
#define HOST "192.168.1.100"  // IP do seu servidor Flask
#define PORT 5000
```

4. **Compile o firmware**:
```bash
mkdir build
cd build
cmake ..
make -j4
```

5. **Flash no Pico W**:
```bash
picotool load projeto_kalfix.uf2
```

### Instalação do Servidor Web

1. **Instale as dependências Python**:
```bash
cd web
pip install -r requirements.txt
```

2. **Configure o banco PostgreSQL**:
```sql
CREATE DATABASE dados_contagem;
CREATE USER kalfix_user WITH PASSWORD 'sua_senha';
GRANT ALL PRIVILEGES ON DATABASE dados_contagem TO kalfix_user;
```

3. **Configure as variáveis de ambiente**:
```bash
# Crie um arquivo .env na pasta web/
DATABASE_URL=postgresql://kalfix_user:sua_senha@localhost/dados_contagem
FLASK_SERVER_HOST=0.0.0.0
FLASK_SERVER_PORT=5000
DB_NAME=dados_contagem
IGNORE_SHIFT_CHECK=false
```

4. **Execute o servidor**:
```bash
python server.py
```

## 📖 Uso

### Inicialização do Sistema

1. **Conecte o hardware**: Conecte o sensor ao GPIO 20 do Pico W
2. **Alimente o dispositivo**: Conecte via USB ou fonte externa
3. **Inicie o servidor**: Execute `python server.py` na pasta web/
4. **Acesse o dashboard**: Abra `http://seu-servidor:5000` no navegador

### Monitoramento

- **Status em Tempo Real**: O dashboard mostra o contador atual e turno ativo
- **Histórico**: Visualize dados históricos por período
- **Gráficos**: Análise visual com diferentes tipos de visualização
- **Logs**: Monitore eventos no terminal do servidor

### Gestão de Turnos

O sistema gerencia automaticamente dois turnos:
- **Turno 1**: 06:00 às 16:00
- **Turno 2**: 22:00 às 06:00 (do dia seguinte)
- **Fora dos turnos**: 16:00 às 22:00 (sem contagem)

## 🔌 API Endpoints

### GET `/update`
Atualiza o contador com dados do hardware.
```http
GET /update?counter=123
```

**Resposta**:
```json
{
  "ok": true,
  "count": 123,
  "received": 123,
  "shift": "Turno 1 (06:00 - 16:00 h)",
  "ignore_shift_check": false
}
```

### GET `/debug_status`
Status atual do sistema para diagnóstico.
```http
GET /debug_status
```

**Resposta**:
```json
{
  "count": 123,
  "shift": "Turno 1 (06:00 - 16:00 h)",
  "shift_key": "Turno 1 (06:00 - 16:00 h) - 2024-01-15",
  "ignore_shift_check": false
}
```

### WebSocket Events

#### `status`
Dados atualizados do sistema:
```json
{
  "count": 123,
  "current_shift": "Turno 1 (06:00 - 16:00 h)",
  "history": [...],
  "timestamp": "2024-01-15 10:30:00"
}
```

## 🌐 Configuração de Rede

### Wi-Fi
Configure no `CMakeLists.txt`:
```cmake
set(WIFI_SSID "Nome_da_Rede")
set(WIFI_PASSWORD "Senha_da_Rede")
```

### Servidor
Configure no `projeto_kalfix.c`:
```c
#define HOST "192.168.1.100"  // IP do servidor
#define PORT 5000             // Porta do servidor
```

### Firewall
Certifique-se de que a porta 5000 está aberta:
```bash
# Ubuntu/Debian
sudo ufw allow 5000

# CentOS/RHEL
sudo firewall-cmd --permanent --add-port=5000/tcp
sudo firewall-cmd --reload
```

## 🗄️ Banco de Dados

### Estrutura da Tabela `shifts`

```sql
CREATE TABLE shifts (
    id SERIAL PRIMARY KEY,
    turno_nome VARCHAR(255) NOT NULL,
    data_turno DATE NOT NULL,
    contador INTEGER DEFAULT 0,
    inicio_turno TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    fim_turno TIMESTAMP NULL,
    UNIQUE(turno_nome, data_turno)
);
```

### Campos
- **id**: Identificador único
- **turno_nome**: Nome do turno (ex: "Turno 1 (06:00 - 16:00 h)")
- **data_turno**: Data do turno
- **contador**: Número de eventos contados
- **inicio_turno**: Timestamp de início
- **fim_turno**: Timestamp de fim (NULL se ativo)

## 📊 Monitoramento

### Logs do Servidor
O servidor produz logs detalhados:
```
[2024-01-15 10:30:00] Recebido: counter=123
[2024-01-15 10:30:00] Turno ativo: Turno 1 (06:00 - 16:00 h) - 2024-01-15
[2024-01-15 10:30:00] ✅ CONTADOR ATUALIZADO NO BANCO: 123
```

### Indicadores Visuais
- **LED no Pico W**: Pisca a cada evento detectado
- **Status no Dashboard**: Indicador de conexão em tempo real
- **Contador**: Atualização animada no frontend

### Métricas
- **Latência**: Tempo entre evento físico e atualização no dashboard
- **Precisão**: Taxa de eventos detectados vs. eventos reais
- **Disponibilidade**: Uptime do sistema

## 🔧 Troubleshooting

### Problemas Comuns

#### Hardware não conecta ao Wi-Fi
```bash
# Verifique as credenciais no CMakeLists.txt
# Teste a conectividade manualmente
# Verifique se a rede suporta dispositivos IoT
```

#### Servidor não recebe dados
```bash
# Verifique o IP do servidor no firmware
# Teste conectividade: ping <ip-do-servidor>
# Verifique se a porta 5000 está aberta
# Monitore logs do servidor
```

#### Banco de dados não conecta
```bash
# Verifique a string de conexão no .env
# Teste conexão: psql $DATABASE_URL
# Verifique se PostgreSQL está rodando
# Confirme permissões do usuário
```

#### Dashboard não atualiza
```bash
# Verifique conexão WebSocket no navegador (F12)
# Teste endpoint: curl http://localhost:5000/debug_status
# Verifique logs do servidor
```

### Logs de Diagnóstico

#### Firmware
```c
// Adicione mais logs no projeto_kalfix.c
printf("DEBUG: GPIO state = %d\n", gpio_state);
printf("DEBUG: HTTP response = %d\n", res);
```

#### Servidor
```python
# Ative logs detalhados
import logging
logging.basicConfig(level=logging.DEBUG)
```

## 🤝 Contribuição

1. **Fork** o projeto
2. **Crie** uma branch para sua feature (`git checkout -b feature/nova-funcionalidade`)
3. **Commit** suas mudanças (`git commit -am 'Adiciona nova funcionalidade'`)
4. **Push** para a branch (`git push origin feature/nova-funcionalidade`)
5. **Abra** um Pull Request

### Padrões de Código
- **C/C++**: Siga o estilo do Pico SDK
- **Python**: Use PEP 8
- **JavaScript**: Use ESLint
- **Commits**: Use mensagens descritivas em português

## 📄 Licença

Este projeto está licenciado sob a Licença MIT - veja o arquivo [LICENSE](LICENSE) para detalhes.

## 📞 Suporte

Para suporte técnico ou dúvidas:
- **Issues**: Use o sistema de issues do GitHub
- **Documentação**: Consulte este README e comentários no código
- **Comunidade**: Participe das discussões no repositório

---

**Desenvolvido com ❤️ para ambientes industriais modernos**

