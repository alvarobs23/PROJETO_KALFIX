#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "example_http_client_util.h"

// Configurações de rede 
#ifndef WIFI_SSID
#define WIFI_SSID "SILVA"
#endif
#ifndef WIFI_PASSWORD  
#define WIFI_PASSWORD "a1l9v7r8o"
#endif

// Configurações do servidor
#define HOST        "10.43.11.109"  // IP do seu servidor Flask
#define PORT        5000
#define INTERVALO_MS 500

// Configurações de hardware
#define GPIO_MONITOR    6// Pino para monitorar sinal HIGH




// Variável global para contador de eventos
static uint32_t event_counter = 0;

// Função para enviar contador ao servidor via GET
void send_counter_to_server(uint32_t counter_value) {
    char path[128];
    
    snprintf(path, sizeof(path), "/update?counter=%lu", counter_value);

    printf("ENVIANDO: contador = %lu\n", counter_value);
    printf("GET http://%s:%d%s\n", HOST, PORT, path);

    EXAMPLE_HTTP_REQUEST_T req = {
        .hostname   = HOST,
        .url        = path,
        .port       = PORT,
        .headers_fn = http_client_header_print_fn,
        .recv_fn    = http_client_receive_print_fn
    };

    int res = http_client_request_sync(cyw43_arch_async_context(), &req);
    printf("Status: %s\n\n", res == 0 ? "Sucesso" : "Erro");
}

int main() {
    stdio_init_all();

    // Configura GPIO de monitoramento e LED
    gpio_init(GPIO_MONITOR);
    gpio_set_dir(GPIO_MONITOR, GPIO_IN);
    gpio_pull_up(GPIO_MONITOR);  // Pull-up para detectar HIGH

   

    // Inicializa Wi-Fi
    if (cyw43_arch_init()) {
        printf("Falha ao inicializar Wi-Fi\n");
        return 1;
    }
    
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_AES_PSK, 10000)) {
    printf("Falha na conexão Wi-Fi\n");
        return 1;
    }

    // Exibe IP obtido
    printf("Conectado! IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    printf("Enviando dados para servidor: %s:%d\n", HOST, PORT);

    // ========== MONITORAMENTO GPIO 20 ==========
    // Detecta bordas de subida (HIGH -> LOW) no GPIO 20
    int last_gpio_state = 1;  // Estado inicial (HIGH)
    uint32_t last_event_time = 0;
    const uint32_t MIN_EVENT_INTERVAL = 10;  

    printf("🔍 Iniciando monitoramento do GPIO %d\n", GPIO_MONITOR);
    printf("📊 Contador inicial: %lu\n", event_counter);

    while (1) {
        int gpio_state = gpio_get(GPIO_MONITOR);
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
     

        // Detecta APENAS quando GPIO vai de HIGH para LOW (borda de subida)
        if (last_gpio_state == 1 && gpio_state == 0) {
            // Evita eventos muito rápidos (debounce)
            if (current_time - last_event_time > MIN_EVENT_INTERVAL) {
                event_counter++;
                printf("⚡ SINAL HIGH DETECTADO! (tempo: %lu ms)\n", current_time);
                printf("📈 Contador: %lu\n", event_counter);
                
                // Envia contador atualizado para o servidor
              send_counter_to_server(event_counter);
               last_event_time = current_time;
            } else {
                printf("🚫 Evento muito rápido ignorado (debounce)\n");
            }
        }
        
        last_gpio_state = gpio_state;
        sleep_ms(5);  //
    }

    /* ========== VERSÃO COM DEBOUNCE AVANÇADO (comentada) ==========
    int last_btn = 1;
    uint32_t last_change_time = 0;
    const uint32_t DEBOUNCE_MS = 20;
    bool button_sent = false;

    while (1) {
        int btn = gpio_get(BUTTON_A);
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        gpio_put(LED_RED, btn == 0);

        if (btn != last_btn) {
            if (current_time - last_change_time > DEBOUNCE_MS) {
                if (btn == 0 && !button_sent) {
                    printf("CLIQUE DETECTADO! (tempo: %lu ms)\n", current_time);
                    send_button_press(0);
                    button_sent = true;
                }
                
                if (btn == 1) {
                    button_sent = false;
                    printf("Botão solto - pronto para próximo clique\n");
                }
                
                last_btn = btn;
                last_change_time = current_time;
            }
        }
        sleep_ms(5);
    }
    ============================================================== */

    cyw43_arch_deinit();
    return 0;
}
