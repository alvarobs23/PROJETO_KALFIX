// main.c (versão corrigida - evita retries contínuos que travam a contagem)
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/multicore.h" // Suporte a multicore
#include "pico/mutex.h"     // Suporte a mutex para acesso concorrente ao LCD
#include "example_http_client_util.h"

// ========== CONFIGURAÇÕES ==========
#ifndef WIFI_SSID
#define WIFI_SSID "KALFIX"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "9988776655"
#endif

#define HOST        "10.78.166.109"
#define PORT        5000

// GPIO monitor (entrada com pull-up)
#define GPIO_MONITOR 6

// I2C (LCD) - I2C1 (GP4 SDA, GP5 SCL)
#define I2C_PORT i2c1
#define SDA_PIN 18
#define SCL_PIN 19

// PCF8574 / LCD
#define LCD_ADDR 0x27
#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE 0x04

// Debounce (ms)
const uint32_t MIN_EVENT_INTERVAL = 10;

// Wi-Fi timings (ms) - ajuste conforme necessário
const uint32_t WIFI_INIT_RETRY_MS    = 10000; // tentar inicializar stack se falhar
const uint32_t WIFI_CONNECT_RETRY_MS = 3000;  // intervalo para tentar conectar ao AP
const uint32_t WIFI_SEND_RETRY_MS    = 5000;  // intervalo entre tentativas de envio ao servidor
const int      SEND_FAILS_TO_RECONNECT = 3;   // número de falhas consecutivas que forçam reconnect

// ======= VARIÁVEIS COMPARTILHADAS ENTRE CORES =======
// contador
static volatile uint32_t event_counter = 0;

// comunicação para a rotina de envio (somente último valor importa)
static volatile uint32_t latest_pending = 0;
static volatile bool has_pending_data = false;

// flags de wifi
static volatile bool wifi_init_ok = false;     // cyw43_arch_init já ok
static volatile bool wifi_mode_enabled = false;// cyw43_arch_enable_sta_mode feito
static volatile bool wifi_connected = false;   // conecção ao AP já estabelecida
static volatile bool wifi_connecting = false;  // flag para indicar tentativa de conexão em andamento

// flags para envio assíncrono
static volatile bool http_req_in_progress = false;
static EXAMPLE_HTTP_REQUEST_T http_req_state; // Mantém o estado da requisição HTTP
static char http_req_path[128]; // Buffer para o path da URL

// Mutex para proteger o acesso ao LCD a partir de ambos os cores
static mutex_t lcd_mutex;

// ========== I2C / LCD (PCF8574 + HD44780 4-bit) ==========
static void pcf_write_byte(uint8_t data) {
    i2c_write_blocking(I2C_PORT, LCD_ADDR, &data, 1, false);
}

static void lcd_pulse_enable(uint8_t data) {
    uint8_t d = data | LCD_BACKLIGHT;
    pcf_write_byte(d);
    pcf_write_byte(d | LCD_ENABLE);
    sleep_us(500);
    pcf_write_byte(d & ~LCD_ENABLE);
    sleep_us(100);
}

static void lcd_write4bits(uint8_t value) {
    pcf_write_byte(value | LCD_BACKLIGHT);
    lcd_pulse_enable(value);
}

static void lcd_send_byte(uint8_t val, bool rs) {
    uint8_t rsbit = rs ? 0x01 : 0x00;
    uint8_t high = (val & 0xF0) | rsbit;
    uint8_t low  = ((val << 4) & 0xF0) | rsbit;
    lcd_write4bits(high);
    lcd_write4bits(low);
}

static void lcd_cmd(uint8_t cmd) { lcd_send_byte(cmd, false); }
static void lcd_data(uint8_t data) { lcd_send_byte(data, true); }

static void lcd_init() {
    sleep_ms(50);
    lcd_write4bits(0x30); sleep_ms(5);
    lcd_write4bits(0x30); sleep_us(200);
    lcd_write4bits(0x30); sleep_ms(5);
    lcd_write4bits(0x20); // 4-bit mode
    lcd_cmd(0x28); // 2 linhas, 5x8
    lcd_cmd(0x08); // display off
    lcd_cmd(0x01); // clear
    sleep_ms(2);
    lcd_cmd(0x06); // entry mode
    lcd_cmd(0x0C); // display on, cursor off
}

static void lcd_clear() {
    lcd_cmd(0x01);
    sleep_ms(2);
}

static void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    if (row > 1) row = 1;
    lcd_cmd(0x80 | (col + row_offsets[row]));
}

static void lcd_write_string(const char *s) {
    while (*s) lcd_data((uint8_t)*s++);
}

// ========== FUNÇÕES DE REDE (CORE0 irá gerenciar) ==========
static bool try_cyw43_init_once() {
    printf("[CORE0] Tentando cyw43_arch_init()...\n");
    int res = cyw43_arch_init();
    // cyw43_arch_set_country(CYW43_COUNTRY_BRAZIL); // Opcional: Define o país. Removido para evitar o erro.
    if (res == 0) {
        printf("[CORE0] cyw43_arch_init OK\n");
        wifi_init_ok = true;
        return true;
    } else {
        printf("[CORE0] cyw43_arch_init FALHOU (res=%d)\n", res);
        wifi_init_ok = false;
        return false;
    }
}

static bool try_enable_sta_mode_once() {
    if (!wifi_init_ok) return false;
    cyw43_arch_enable_sta_mode();
    wifi_mode_enabled = true;
    printf("[CORE0] Modo STA habilitado\n");
    return true;
}

// NOVA FUNÇÃO: Envia a requisição HTTP usando um endereço IP diretamente, sem DNS.
static int start_sending_to_server_by_ip(uint32_t value) {
    ip_addr_t server_ip;
    // Converte a string de IP para o formato que a lwIP entende.
    if (!ip4addr_aton(HOST, &server_ip)) {
        printf("[CORE0] ERRO: Endereço IP do host é inválido: %s\n", HOST);
        return -1; // Retorna um erro se o IP for inválido
    }

    snprintf(http_req_path, sizeof(http_req_path), "/update?counter=%lu", value);
    printf("[CORE0] Enviando para IP %s: %lu\n", HOST, value);

    // Zera e configura o estado da requisição.
    memset(&http_req_state, 0, sizeof(http_req_state));
    http_req_state.hostname = HOST;
    http_req_state.url = http_req_path;
    http_req_state.port = PORT;

    // Usa httpc_get_file, que não faz consulta DNS e evita o bloqueio.
    // Note que passamos o IP convertido e os callbacks diretamente.
    err_t err = httpc_get_file(&server_ip, PORT, http_req_path, &http_req_state.settings,
                             http_req_state.recv_fn, &http_req_state, NULL);

    return (err == ERR_OK) ? 0 : -1;
}

// Função auxiliar para atualizar o contador no display (movida para fora de core1_entry)
static void update_lcd_count() {
    char buf[17];
    snprintf(buf, sizeof(buf), "%lu", event_counter);
    mutex_enter_blocking(&lcd_mutex);
    lcd_set_cursor(10, 0); // Posição após "Contador: "
    lcd_write_string("      "); // Limpa 6 caracteres
    lcd_set_cursor(10, 0);
    lcd_write_string(buf);
    mutex_exit(&lcd_mutex);
}


// ========== LÓGICA DO CORE 1 (Contagem de Pulsos) ==========
void core1_entry() {
    printf("[CORE1] Core 1 iniciado. Monitorando GPIO %d...\n", GPIO_MONITOR);

    // Inicializa I2C e LCD no Core 1
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    // Adquire o lock para inicializar e escrever o estado inicial do LCD
    mutex_enter_blocking(&lcd_mutex);
    lcd_init();
    lcd_clear();
    lcd_set_cursor(0,0);
    lcd_write_string("Contador: 0");
    lcd_set_cursor(0,1);
    lcd_write_string("Inicializando...");
    mutex_exit(&lcd_mutex);

    // Variável para otimizar atualização do LCD
    uint32_t last_displayed_count = 0;

    // Inicializa GPIO monitor no Core 1
    gpio_init(GPIO_MONITOR);
    gpio_set_dir(GPIO_MONITOR, GPIO_IN);
    gpio_pull_up(GPIO_MONITOR);

    int last_gpio_state = 1;
    uint32_t last_event_time = 0;

    while (1) {
        int gpio_state = gpio_get(GPIO_MONITOR);
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // Detecta borda de descida (1 -> 0) com debounce
        if (last_gpio_state == 1 && gpio_state == 0) {
            if (current_time - last_event_time > MIN_EVENT_INTERVAL) {
                last_event_time = current_time;
                event_counter++; // Incrementa o contador compartilhado
                latest_pending = event_counter; // Atualiza o valor a ser enviado
                has_pending_data = true; // Sinaliza para o Core 0 que há novos dados

                if (event_counter != last_displayed_count) {
                    update_lcd_count();
                    last_displayed_count = event_counter;
                }
            }
        }
        last_gpio_state = gpio_state;
        sleep_ms(1); // Pequeno delay para não sobrecarregar o core
    }
}

// ========== MAIN (CORE 0 - Rede e Display) ==========
int main() {
    stdio_init_all();
    sleep_ms(1000); // Aguarda um pouco para o monitor serial estabilizar

    // Inicializa o mutex antes de lançar o Core 1
    mutex_init(&lcd_mutex);

    // Lança a rotina de contagem no Core 1
    multicore_launch_core1(core1_entry);
    // Tenta init da stack (se falhar, tentaremos novamente no loop)
    if (try_cyw43_init_once()) {
        try_enable_sta_mode_once();
    } else {
        printf("[CORE0] Continuando sem Wi-Fi por enquanto\n");
    }

    // LOOP principal - detecta pulse, atualiza display e gerencia Wi-Fi/envio
    uint32_t last_wifi_init_attempt = to_ms_since_boot(get_absolute_time());
    uint32_t last_wifi_connect_attempt = to_ms_since_boot(get_absolute_time());
    uint32_t last_send_attempt = 0;
    int send_fail_count = 0;

    while (1) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // =========================
        // Gerenciamento do Wi-Fi
        // =========================

        // 1) Se cyw43 não inicializado com sucesso, tente inicializar periodicamente
        if (!wifi_init_ok && current_time - last_wifi_init_attempt >= WIFI_INIT_RETRY_MS) {
            last_wifi_init_attempt = current_time;
            try_cyw43_init_once();
            if (wifi_init_ok) {
                try_enable_sta_mode_once();
            }
        }

        // 2) Se inicializado mas modo STA não habilitado (caso não tenha sido feito), habilite
        if (wifi_init_ok && !wifi_mode_enabled) {
            try_enable_sta_mode_once();
        }

        // 3) Gerenciamento da conexão Wi-Fi (agora assíncrono)
        if (wifi_mode_enabled && !wifi_connected) {
            if (!wifi_connecting) {
                // Se não estamos conectados e não há uma tentativa em andamento, inicie uma.
                if (current_time - last_wifi_connect_attempt >= WIFI_CONNECT_RETRY_MS) {
                    last_wifi_connect_attempt = current_time;
                    printf("[CORE0] Iniciando tentativa de conexão Wi-Fi assíncrona...\n");
                    int res = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
                    if (res == 0) {
                        wifi_connecting = true;
                    } else {
                        printf("[CORE0] Falha ao iniciar conexão async (res=%d)\n", res);
                    }
                }
            } else {
                // Se uma tentativa de conexão está em andamento, verifique seu status.
                int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                if (link_status == CYW43_LINK_UP) {
                    printf("[CORE0] Conectado ao Wi-Fi!\n");
                    wifi_connected = true;
                    wifi_connecting = false;
                } else if (link_status < 0) { // Qualquer erro é negativo
                    // Adiciona mais detalhes ao log de erro
                    if (link_status == CYW43_LINK_FAIL) {
                        printf("[CORE0] Falha ao conectar (status=%d): Verifique a SENHA do Wi-Fi.\n", link_status);
                    } else if (link_status == CYW43_LINK_NONET) {
                        printf("[CORE0] Falha ao conectar (status=%d): Rede (SSID) não encontrada.\n", link_status);
                    } else if (link_status == CYW43_LINK_BADAUTH) {
                        printf("[CORE0] Falha ao conectar (status=%d): Falha de autenticação (senha incorreta).\n", link_status);
                    } else {
                        printf("[CORE0] Falha ao conectar (status=%d)\n", link_status);
                    }
                    wifi_connecting = false; // Permite uma nova tentativa após o retry
                }
            }
        }

        // 4) Gerenciamento do envio de dados (agora assíncrono)
        if (http_req_in_progress) {
            // Se uma requisição está em andamento, verifica se ela já terminou
            if (http_req_state.complete) {
                http_req_in_progress = false;
                printf("[CORE0] Resultado do envio: %s\n", http_req_state.result == 0 ? "Sucesso" : "Erro");

                if (http_req_state.result == 0) { // Sucesso (HTTP_RESULT_OK)
                    has_pending_data = false; // Limpa a flag, pois o dado mais recente foi enviado
                    send_fail_count = 0;
                    mutex_enter_blocking(&lcd_mutex);
                    lcd_set_cursor(0, 1);
                    lcd_write_string("Status: WiFi OK  ");
                    mutex_exit(&lcd_mutex);
                } else { // Falha
                    send_fail_count++;
                    mutex_enter_blocking(&lcd_mutex);
                    lcd_set_cursor(0, 1);
                    lcd_write_string("Status: Erro envio");
                    mutex_exit(&lcd_mutex);
                    wifi_connecting = false; // Garante que não fique preso se o envio falhar

                    if (send_fail_count >= SEND_FAILS_TO_RECONNECT) {
                        send_fail_count = 0;
                        wifi_connected = false;
                        last_wifi_connect_attempt = current_time;
                        printf("[CORE0] Muitos erros de envio -> forçando reconnect\n");
                    } else {
                        printf("[CORE0] Falha no envio, tentarei novamente em %lums (tentativas=%d)\n", WIFI_SEND_RETRY_MS, send_fail_count);
                    }
                }
            }
        } else {
            // Se não há requisição em andamento, verifica se há dados pendentes para enviar
            if (wifi_connected && has_pending_data && (current_time - last_send_attempt >= WIFI_SEND_RETRY_MS)) {
                uint32_t to_send = latest_pending;
                last_send_attempt = current_time;

                // Inicia o envio assíncrono USANDO A NOVA FUNÇÃO POR IP
                if (start_sending_to_server_by_ip(to_send) == 0) {
                    http_req_in_progress = true;
                    mutex_enter_blocking(&lcd_mutex);
                    lcd_set_cursor(0, 1);
                    lcd_write_string("Status: Enviando..");
                    mutex_exit(&lcd_mutex);
                } else {
                    // A chamada para iniciar a requisição falhou imediatamente
                    printf("[CORE0] Falha ao iniciar a requisição HTTP.\n");
                }
            }
        }

        // pequeno sleep para não ocupar 100% da CPU; mantém responsividade para contagem
        cyw43_arch_poll(); // Essencial para processar eventos de rede no modo assíncrono
        sleep_ms(1);
    }

    // limpeza (não alcançada normalmente)
    if (wifi_init_ok) cyw43_arch_deinit();
    return 0;
}
