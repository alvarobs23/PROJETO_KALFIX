
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "pico/mutex.h"
#include "example_http_client_util.h"
#include "hardware/structs/resets.h"
#include "hardware/sync.h"

// ========== CONFIGURAÇÕES ==========
// Ajuste seu SSID/SENHA se necessário
#ifndef WIFI_SSID
#define WIFI_SSID "KALFIX"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "9988776655"
#endif

#define HOST        "192.168.18.184"
#define PORT        5000

// GPIO monitor (entrada com pull-up)
#define GPIO_MONITOR 20

// I2C (LCD) - I2C1 (GP18 SDA, GP19 SCL)
#define I2C_PORT i2c1
#define SDA_PIN 18
#define SCL_PIN 19

// I2C (RTC DS3231) - I2C0 (GP8 SDA, GP9 SCL)
#define RTC_I2C_PORT i2c0
#define RTC_SDA_PIN 8
#define RTC_SCL_PIN 9
#define DS3231_ADDR 0x68

// PCF8574 / LCD
#define LCD_ADDR 0x27
#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE_BIT 0x04

// Flag para configurar o RTC na primeira gravação
#define SET_RTC_TIME 0

// Debounce (ms)
const uint32_t MIN_EVENT_INTERVAL = 10;

// Wi-Fi timings (ms)
const uint32_t WIFI_INIT_RETRY_MS    = 10000;
const uint32_t WIFI_CONNECT_RETRY_MS = 3000;
const uint32_t WIFI_SEND_RETRY_MS    = 5000;
const int      SEND_FAILS_TO_RECONNECT = 3;

// ========== SALVAMENTO EM FLASH (SEGURANÇA) ==========
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define NV_PAGES_PER_SECTOR (FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE)
#define NV_RECORD_MAGIC 0xA5A55A5Au

// Critérios para pedir gravação (no core1):
const uint32_t SAVE_EVENT_THRESHOLD = 5;      // grava ao alcançar +5 eventos além do último salvo
const uint32_t SAVE_TIME_THRESHOLD_MS = 5000; // grava pelo menos a cada 5s se tiver mudança

// Estrutura armazenada em cada página (no início da página)
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t counter;
    uint8_t  day;
    uint8_t  month;
    uint8_t  year;
    uint8_t  hour;
    uint32_t crc32; // crc de magic+seq+counter+day+month+year+hour (exclui crc32)
    uint8_t  reserved[FLASH_PAGE_SIZE - 20]; // preenche a página
} nv_page_t;
_Static_assert(sizeof(nv_page_t) == FLASH_PAGE_SIZE, "nv_page_t must equal flash page size");

// -------- CRC32 (polinômio 0xEDB88320) --------
static uint32_t crc32_table[256];
static void crc32_init_table(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
        crc32_table[i] = crc;
    }
}
static uint32_t crc32_compute(const uint8_t *data, size_t len) {
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        uint8_t idx = (uint8_t)((crc ^ data[i]) & 0xFF);
        crc = (crc >> 8) ^ crc32_table[idx];
    }
    return crc ^ 0xFFFFFFFFu;
}

// ========== VARIÁVEIS COMPARTILHADAS ENTRE CORES ==========
static volatile uint32_t event_counter = 0;
static volatile uint32_t latest_pending = 0;
static volatile bool has_pending_data = false;

// Wi-Fi flags
static volatile bool wifi_init_ok = false;
static volatile bool wifi_mode_enabled = false;
static volatile bool wifi_connected = false;
static volatile bool wifi_connecting = false;

// HTTP state (mantido por compatibilidade; não usamos httpc_get_file aqui)
static volatile bool http_req_in_progress = false;
static EXAMPLE_HTTP_REQUEST_T http_req_state;
static char http_req_path[128];

// Mutex para LCD
static mutex_t lcd_mutex;

// Pedido seguro de gravação: core1 escreve estes e core0 processa
static volatile bool flash_save_request = false;
static volatile uint32_t flash_save_value = 0;
static volatile uint8_t flash_save_day = 0;
static volatile uint8_t flash_save_month = 0;
static volatile uint8_t flash_save_year = 0;
static volatile uint8_t flash_save_hour = 0;

// ========== I2C / LCD (PCF8574 + HD44780 4-bit) ==========
static void pcf_write_byte(uint8_t data) {
    i2c_write_blocking(I2C_PORT, LCD_ADDR, &data, 1, false);
}
static void lcd_pulse_enable(uint8_t data) {
    uint8_t d = data | LCD_BACKLIGHT;
    pcf_write_byte(d);
    pcf_write_byte(d | LCD_ENABLE_BIT);
    sleep_us(500);
    pcf_write_byte(d & ~LCD_ENABLE_BIT);
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

// =====================
// Envio usando API antiga (síncrona) - REUTILIZA example_http_client_util
// =====================
static int start_sending_to_server_by_ip(uint32_t value) {
    // monta path como no sistema antigo
    snprintf(http_req_path, sizeof(http_req_path), "/update?counter=%lu", (unsigned long)value);
    printf("[CORE0] (http sync) Enviando para http://%s:%d%s\n", HOST, PORT, http_req_path);

    // Prepara requisição usando a estrutura utilitária
    EXAMPLE_HTTP_REQUEST_T req = {
        .hostname   = HOST,
        .url        = http_req_path,
        .port       = PORT,
        .headers_fn = http_client_header_print_fn,
        .recv_fn    = http_client_receive_print_fn
    };

    // Usa a API síncrona que funcionou no código antigo
    int res = http_client_request_sync(cyw43_arch_async_context(), &req);
    if (res == 0) {
        printf("[CORE0] Envio síncrono OK (valor=%lu)\n", (unsigned long)value);
        return 0;
    } else {
        printf("[CORE0] Envio síncrono FALHOU (res=%d)\n", res);
        return -1;
    }
}

// ========== FUNÇÕES DO RTC DS3231 ==========
static uint8_t bcd_to_dec(uint8_t val) {
    return (val / 16 * 10) + (val % 16);
}
static uint8_t dec_to_bcd(uint8_t val) {
    return (val / 10 * 16) + (val % 10);
}
static void ds3231_set_time(uint8_t sec, uint8_t min, uint8_t hour, uint8_t day_of_week, uint8_t day, uint8_t month, uint8_t year) {
    uint8_t data[8];
    data[0] = 0x00;
    data[1] = dec_to_bcd(sec);
    data[2] = dec_to_bcd(min);
    data[3] = dec_to_bcd(hour);
    data[4] = dec_to_bcd(day_of_week);
    data[5] = dec_to_bcd(day);
    data[6] = dec_to_bcd(month);
    data[7] = dec_to_bcd(year);
    i2c_write_blocking(RTC_I2C_PORT, DS3231_ADDR, data, 8, false);
    printf("[CORE1] RTC DS3231 configurado.\n");
}
struct ds3231_time {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
};
static void ds3231_get_time(struct ds3231_time *t) {
    uint8_t buffer[7];
    uint8_t reg = 0x00;
    i2c_write_blocking(RTC_I2C_PORT, DS3231_ADDR, &reg, 1, true);
    i2c_read_blocking(RTC_I2C_PORT, DS3231_ADDR, buffer, 7, false);
    t->sec = bcd_to_dec(buffer[0]);
    t->min = bcd_to_dec(buffer[1]);
    t->hour = bcd_to_dec(buffer[2] & 0x3F);
    t->day = bcd_to_dec(buffer[4]);
    t->month = bcd_to_dec(buffer[5] & 0x7F);
    t->year = bcd_to_dec(buffer[6]);
}

// ========== FUNÇÕES DE FLASH (CORE0 APENAS) ==========
static int nv_find_latest(uint32_t *out_counter, uint32_t *out_seq, uint8_t *out_day, uint8_t *out_month, uint8_t *out_year, uint8_t *out_hour) {
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
    uint32_t best_seq = 0;
    uint32_t best_counter = 0;
    uint8_t best_day = 0;
    uint8_t best_month = 0;
    uint8_t best_year = 0;
    uint8_t best_hour = 0;
    bool found = false;

    for (uint32_t page = 0; page < NV_PAGES_PER_SECTOR; ++page) {
        const nv_page_t *p = (const nv_page_t *)(flash_ptr + page * FLASH_PAGE_SIZE);
        if (p->magic != NV_RECORD_MAGIC) continue;
        // compute crc over first 16 bytes (magic, seq, counter, day, month, year, hour)
        uint32_t crc_calc = crc32_compute((const uint8_t *)p, 16);
        if (crc_calc != p->crc32) continue;
        // válido
        if (!found || p->seq > best_seq) {
            best_seq = p->seq;
            best_counter = p->counter;
            best_day = p->day;
            best_month = p->month;
            best_year = p->year;
            best_hour = p->hour;
            found = true;
        }
    }

    if (found) {
        *out_counter = best_counter;
        *out_seq = best_seq;
        if (out_day) *out_day = best_day;
        if (out_month) *out_month = best_month;
        if (out_year) *out_year = best_year;
        if (out_hour) *out_hour = best_hour;
        return 0;
    }
    return -1;
}

static int nv_load_counter(uint32_t *out_counter, uint32_t *out_seq, uint8_t *out_day, uint8_t *out_month, uint8_t *out_year, uint8_t *out_hour) {
    // Retorna 0 se encontrou, -1 se não
    return nv_find_latest(out_counter, out_seq, out_day, out_month, out_year, out_hour);
}

static int nv_save_counter(uint32_t counter, uint8_t day, uint8_t month, uint8_t year, uint8_t hour) {
    // Procura a primeira página livre (0xFF no magic)
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
    int free_page = -1;
    for (uint32_t page = 0; page < NV_PAGES_PER_SECTOR; ++page) {
        const nv_page_t *p = (const nv_page_t *)(flash_ptr + page * FLASH_PAGE_SIZE);
        // Se magic == 0xFFFFFFFF -> página livre
        if (p->magic == 0xFFFFFFFFu) {
            free_page = (int)page;
            break;
        }
    }

    // Se não encontrou página livre, precisamos apagar o setor e usar a primeira página
    bool erased = false;
    if (free_page < 0) {
        printf("[CORE0] NV sector cheio: apagando sector.\n");
        multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
        free_page = 0;
        erased = true;
    }

    // Determina seq para o novo registro (base no maior seq atual)
    uint32_t cur_counter = 0;
    uint32_t cur_seq = 0;
    nv_find_latest(&cur_counter, &cur_seq, NULL, NULL, NULL, NULL);
    uint32_t new_seq = cur_seq + 1;

    // Prepara a página inteira: set 0xFF e depois preenche o cabeçalho
    nv_page_t page_buf;
    memset(&page_buf, 0xFF, sizeof(page_buf));
    page_buf.magic = NV_RECORD_MAGIC;
    page_buf.seq = new_seq;
    page_buf.counter = counter;
    page_buf.day = day;
    page_buf.month = month;
    page_buf.year = year;
    page_buf.hour = hour;
    // calcula crc
    page_buf.crc32 = crc32_compute((const uint8_t *)&page_buf, 16);

    // Programa a página (precisa escrever FLASH_PAGE_SIZE bytes)
    uint32_t write_offset = FLASH_TARGET_OFFSET + (free_page * FLASH_PAGE_SIZE);
    // Protege interrupções durante program
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(write_offset, (const uint8_t *)&page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();

    printf("[CORE0] NV salvo (page=%d, seq=%lu, counter=%lu, date=%02d/%02d/%02d %02dh) erased=%d\n", free_page, (unsigned long)new_seq, (unsigned long)counter, day, month, year, hour, erased ? 1 : 0);
    return 0;
}

// Função wrapper para leitura na inicialização (usada por core1 via leitura global)
static bool load_counter_from_flash_wrapper(uint32_t *counter, uint8_t *day, uint8_t *month, uint8_t *year, uint8_t *hour) {
    uint32_t seq = 0;
    if (nv_load_counter(counter, &seq, day, month, year, hour) == 0) {
        return true;
    } else {
        return false;
    }
}

// ========== LÓGICA DE TURNOS ==========
typedef enum {
    TURNO_1,   // 06:00 - 19:59 (ajustado conforme necessidade)
    TURNO_2,   // 22:00 - 05:59
    INTERVALO
} ShiftState;

static ShiftState get_current_shift_state(uint8_t hour) {
    if (hour >= 6 && hour < 20) {
        return TURNO_1;
    } else if (hour >= 22 || hour < 6) {
        return TURNO_2;
    } else {
        return INTERVALO;
    }
}

// Update LCD helpers
static void update_lcd_count() {
    char buf[17];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)event_counter);
    mutex_enter_blocking(&lcd_mutex);
    lcd_set_cursor(10, 0); // Posição após "Contador: "
    lcd_write_string("      "); // Limpa 6 caracteres
    lcd_set_cursor(10, 0);
    lcd_write_string(buf);
    mutex_exit(&lcd_mutex);
}

static void update_lcd_time(struct ds3231_time *t, ShiftState state) {
    char buf[17];
    const char *state_str;
    switch (state) {
        case TURNO_1:   state_str = "Turno 1"; break;
        case TURNO_2:   state_str = "Turno 2"; break;
        default:        state_str = "INT"; break;
    }
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %-9s", t->hour, t->min, t->sec, state_str);
    mutex_enter_blocking(&lcd_mutex);
    lcd_set_cursor(0, 1);
    lcd_write_string("                ");
    lcd_set_cursor(0, 1);
    lcd_write_string(buf);
    mutex_exit(&lcd_mutex);
}

// Helper para verificar se a data salva é "ontem"
static bool is_previous_day(uint8_t c_d, uint8_t c_m, uint8_t c_y, uint8_t p_d, uint8_t p_m, uint8_t p_y) {
    if (c_y != p_y) {
        // Mudança de ano (ex: 01/01/24 vs 31/12/23)
        return (c_m == 1 && c_d == 1 && p_m == 12 && p_d == 31 && c_y == p_y + 1);
    }
    if (c_m != p_m) {
        // Mudança de mês (ex: 01/02 vs 31/01)
        if (c_d != 1 || c_m != p_m + 1) return false;
        uint8_t days_in_month[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        if (p_y % 4 == 0) days_in_month[2] = 29; // Bissexto simples
        return (p_d == days_in_month[p_m]);
    }
    // Mesmo mês/ano
    return (c_d == p_d + 1);
}

// ========== LÓGICA DO CORE 1 (Contagem de Pulsos) ==========
void core1_entry() {
    multicore_lockout_victim_init();
    printf("[CORE1] Core 1 iniciado. Monitorando GPIO %d...\n", GPIO_MONITOR);

    // Inicializa I2C do LCD (i2c1)
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    mutex_enter_blocking(&lcd_mutex);
    lcd_init();
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_write_string("Contador: 0");
    lcd_set_cursor(0, 1);
    lcd_write_string("HH:MM:SS Status");
    mutex_exit(&lcd_mutex);

    // Inicializa I2C do RTC (i2c0)
    i2c_init(RTC_I2C_PORT, 100 * 1000);
    gpio_set_function(RTC_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(RTC_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(RTC_SDA_PIN);
    gpio_pull_up(RTC_SCL_PIN);

#if SET_RTC_TIME
    // Ajuste manual: descomente apenas na primeira gravação do firmware
    //ds3231_set_time(0, 38, 18, 5, 27, 11, 25);
#endif

    // Lê hora para determinar o estado inicial do turno ANTES de carregar a flash
    struct ds3231_time current_rtc_time;
    ds3231_get_time(&current_rtc_time);
    ShiftState current_shift_state = get_current_shift_state(current_rtc_time.hour);
    ShiftState previous_shift_state = current_shift_state;

    // Carrega contador da flash SOMENTE se estivermos dentro de um turno.
    // Se estivermos em INTERVALO, inicia com 0.
    if (current_shift_state == INTERVALO) {
        event_counter = 0;
        printf("[CORE1] Inicializado em INTERVALO -> contador=0\n");
    } else {
        uint32_t saved_counter = 0;
        uint8_t s_day = 0, s_month = 0, s_year = 0, s_hour = 0;
        if (load_counter_from_flash_wrapper(&saved_counter, &s_day, &s_month, &s_year, &s_hour)) {
            bool should_restore = false;
            bool same_day = (s_day == current_rtc_time.day && s_month == current_rtc_time.month && s_year == current_rtc_time.year);

            if (current_shift_state == TURNO_1) {
                // Turno 1 (06:00 - 19:59): Deve ser o mesmo dia
                if (same_day && get_current_shift_state(s_hour) == TURNO_1) should_restore = true;
            } else if (current_shift_state == TURNO_2) {
                // Turno 2 (22:00 - 05:59): Pode cruzar a meia-noite
                if (current_rtc_time.hour >= 22) {
                    // Estamos na parte "inicial" do turno (noite). Registro deve ser de hoje e noite.
                    if (same_day && s_hour >= 22) should_restore = true;
                } else {
                    // Estamos na parte "final" do turno (madrugada/manhã).
                    // Registro pode ser de hoje (madrugada) OU de ontem (noite).
                    if (same_day && s_hour < 6) should_restore = true;
                    else if (is_previous_day(current_rtc_time.day, current_rtc_time.month, current_rtc_time.year, s_day, s_month, s_year) && s_hour >= 22) {
                        should_restore = true;
                    }
                }
            }

            if (should_restore) {
                event_counter = saved_counter;
                printf("[CORE1] Inicializado em TURNO -> Registro válido (%02d/%02d/%02d %02dh). Contador restaurado: %lu\n", s_day, s_month, s_year, s_hour, (unsigned long)event_counter);
            } else {
                event_counter = 0;
                printf("[CORE1] Inicializado em TURNO -> Registro de outro turno/dia (Salvo: %02d/%02d/%02d %02dh). Contador zerado.\n", s_day, s_month, s_year, s_hour);
            }
        } else {
            event_counter = 0;
            printf("[CORE1] Inicializado em TURNO -> Flash vazia ou inválida. Contador zerado.\n");
        }
    }
    update_lcd_count();
    update_lcd_time(&current_rtc_time, current_shift_state);

    // Estado para otimizar gravação
    uint32_t last_saved_count = event_counter;
    uint32_t last_save_time = to_ms_since_boot(get_absolute_time());

    gpio_init(GPIO_MONITOR);
    gpio_set_dir(GPIO_MONITOR, GPIO_IN);
    gpio_pull_up(GPIO_MONITOR);

    int last_gpio_state = 1;
    uint32_t last_event_time = 0;
    uint32_t last_time_update = 0;

    while (1) {
        int gpio_state = gpio_get(GPIO_MONITOR);
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // Atualiza a hora a cada segundo (para decidir turno)
        if (current_time - last_time_update >= 1000) {
            last_time_update = current_time;
            ds3231_get_time(&current_rtc_time);
            current_shift_state = get_current_shift_state(current_rtc_time.hour);
            // ATUALIZA O DISPLAY AQUI!
            update_lcd_time(&current_rtc_time, current_shift_state);
        }

        // Detecta mudança de estado (turno <-> intervalo ou troca de turno)
        if (current_shift_state != previous_shift_state) {
            printf("[CORE1] Mudança de estado: de %d para %d\n", previous_shift_state, current_shift_state);

            // O salvamento do valor final do turno foi removido conforme solicitado.
            // A lógica agora apenas zera o contador para o novo período.
            printf("[CORE1] Fim do turno. O contador será zerado sem salvar na flash.\n");
            
            // 2. Zera o contador e o estado para o novo turno/intervalo.
            //    Isso acontece em TODAS as transições.
            event_counter = 0;
            latest_pending = 0;
            has_pending_data = false;
            update_lcd_count();
            printf("[CORE1] Contador zerado para o novo período.\n");

            // Atualiza previous_shift_state para o novo estado
            previous_shift_state = current_shift_state;
            // Atualiza LCD da hora/turno (para refletir mudança imediata)
            update_lcd_time(&current_rtc_time, current_shift_state);
            // Reseta timers de gravação
            last_saved_count = event_counter;
            last_save_time = current_time;
        }

        // Contagem condicional por turno (apenas se estivermos em um turno)
        if (current_shift_state != INTERVALO) {
            if (last_gpio_state == 1 && gpio_state == 0) {
                if (current_time - last_event_time > MIN_EVENT_INTERVAL) {
                    last_event_time = current_time;
                    event_counter++;
                    latest_pending = event_counter;
                    has_pending_data = true;
                    update_lcd_count();
                }
            }
        } else {
            // Em intervalo: não contabilizar nada
        }

        last_gpio_state = gpio_state;

        // Decidir gravação periódica na flash (pedido para core0)
        if (current_time - last_time_update < 10000) {
            // nothing special here (keeps logic below)
        }

        // A cada segundo (no bloco de tempo), decidir pedido de gravação
        // (usamos last_time_update como referência do tick de 1s)
        // Aqui usamos uma checagem simples por tempo absoluto:
        if ((to_ms_since_boot(get_absolute_time()) - last_save_time) >= SAVE_TIME_THRESHOLD_MS) {
            if (event_counter != last_saved_count) {
                flash_save_value = event_counter;
                flash_save_day = current_rtc_time.day;
                flash_save_month = current_rtc_time.month;
                flash_save_year = current_rtc_time.year;
                flash_save_hour = current_rtc_time.hour;
                flash_save_request = true;
                last_saved_count = event_counter;
                last_save_time = to_ms_since_boot(get_absolute_time());
                printf("[CORE1] Pedido periódico de salvar enviado (counter=%lu)\n", (unsigned long)event_counter);
            } else {
                last_save_time = to_ms_since_boot(get_absolute_time()); // evita múltiplos checks rápidos
            }
        } else {
            // também checa por SAVE_EVENT_THRESHOLD
            if (event_counter > last_saved_count && (event_counter - last_saved_count) >= SAVE_EVENT_THRESHOLD) {
                flash_save_value = event_counter;
                flash_save_day = current_rtc_time.day;
                flash_save_month = current_rtc_time.month;
                flash_save_year = current_rtc_time.year;
                flash_save_hour = current_rtc_time.hour;
                flash_save_request = true;
                last_saved_count = event_counter;
                last_save_time = to_ms_since_boot(get_absolute_time());
                printf("[CORE1] Pedido (threshold evento) de salvar enviado (counter=%lu)\n", (unsigned long)event_counter);
            }
        }

        sleep_ms(1);
    }
}

// ========== MAIN (CORE 0 - Rede, Flash e Display com mutex) ==========
int main() {
    stdio_init_all();
    sleep_ms(1000);

    mutex_init(&lcd_mutex);

    // lança core1
    multicore_launch_core1(core1_entry);

    // tenta iniciar Wi-Fi stack
    if (try_cyw43_init_once()) {
        try_enable_sta_mode_once();
    } else {
        printf("[CORE0] Continuando sem Wi-Fi por enquanto\n");
    }

    // Carrega contagem atual em core0 (para status/log e seq inicial)
    uint32_t nv_counter = 0, nv_seq = 0;
    if (nv_load_counter(&nv_counter, &nv_seq, NULL, NULL, NULL, NULL) == 0) {
        printf("[CORE0] NV encontrado: counter=%lu seq=%lu\n", (unsigned long)nv_counter, (unsigned long)nv_seq);
    } else {
        printf("[CORE0] NV vazio/inválido no início\n");
    }

    uint32_t last_wifi_init_attempt = to_ms_since_boot(get_absolute_time());
    uint32_t last_wifi_connect_attempt = to_ms_since_boot(get_absolute_time());
    uint32_t last_send_attempt = 0;
    int send_fail_count = 0;

    while (1) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // PROCESSA pedidos de gravação vindos do core1
        if (flash_save_request) {
            // captura atomica
            uint32_t to_save = flash_save_value;
            uint8_t d = flash_save_day;
            uint8_t m = flash_save_month;
            uint8_t y = flash_save_year;
            uint8_t h = flash_save_hour;
            // limpa pedido
            flash_save_request = false;
            // salva na flash (apenas aqui, no core0)
            nv_save_counter(to_save, d, m, y, h);
        }

        // =========================
        // Gerenciamento do Wi-Fi
        // =========================
        if (!wifi_init_ok && current_time - last_wifi_init_attempt >= WIFI_INIT_RETRY_MS) {
            last_wifi_init_attempt = current_time;
            try_cyw43_init_once();
            if (wifi_init_ok) try_enable_sta_mode_once();
        }
        if (wifi_init_ok && !wifi_mode_enabled) try_enable_sta_mode_once();

        // Conexão assíncrona
        if (wifi_mode_enabled && !wifi_connected) {
            if (!wifi_connecting) {
                if (current_time - last_wifi_connect_attempt >= WIFI_CONNECT_RETRY_MS) {
                    last_wifi_connect_attempt = current_time;
                    printf("[CORE0] Iniciando tentativa de conexão Wi-Fi assíncrona...\n");
                    int res = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
                    if (res == 0) wifi_connecting = true;
                    else printf("[CORE0] Falha ao iniciar conexão async (res=%d)\n", res);
                }
            } else {
                int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                if (link_status == CYW43_LINK_UP) {
                    printf("[CORE0] Conectado ao Wi-Fi!\n");
                    wifi_connected = true;
                    wifi_connecting = false;
                } else if (link_status < 0) {
                    if (link_status == CYW43_LINK_FAIL) {
                        printf("[CORE0] Falha ao conectar (status=%d): Verifique a SENHA do Wi-Fi.\n", link_status);
                    } else if (link_status == CYW43_LINK_NONET) {
                        printf("[CORE0] Falha ao conectar (status=%d): Rede (SSID) não encontrada.\n", link_status);
                    } else if (link_status == CYW43_LINK_BADAUTH) {
                        printf("[CORE0] Falha ao conectar (status=%d): Falha de autenticação (senha incorreta).\n", link_status);
                    } else {
                        printf("[CORE0] Falha ao conectar (status=%d)\n", link_status);
                    }
                    wifi_connecting = false;
                }
            }
        }

        // Envio síncrono (usa a função que implementa o comportamento do código antigo)
        if (!http_req_in_progress) {
            if (wifi_connected && has_pending_data && (current_time - last_send_attempt >= WIFI_SEND_RETRY_MS)) {
                uint32_t to_send = latest_pending;
                last_send_attempt = current_time;
                int send_res = start_sending_to_server_by_ip(to_send);
                if (send_res == 0) {
                    // sucesso
                    has_pending_data = false;
                    send_fail_count = 0;
                } else {
                    // falha no envio
                    printf("[CORE0] Falha ao enviar o contador ao servidor (sync).\n");
                    send_fail_count++;
                    if (send_fail_count >= SEND_FAILS_TO_RECONNECT) {
                        send_fail_count = 0;
                        wifi_connected = false; // força reconnect
                        last_wifi_connect_attempt = current_time;
                        printf("[CORE0] Muitos erros de envio -> forçando reconnect\n");
                    }
                }
            }
        }

        // Ciclo de poll e pequeno delay
        cyw43_arch_poll();
        sleep_ms(1);
    }

    if (wifi_init_ok) cyw43_arch_deinit();
    return 0;
}
