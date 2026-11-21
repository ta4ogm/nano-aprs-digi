#include <avr/pgmspace.h>
#include <stdio.h>
#include <stdbool.h>
#include <avr/io.h>
#include <string.h>

#include "device.h"
#include "util/FIFO.h"
#include "util/time.h"
#include "hardware/AFSK.h"
#include "hardware/Serial.h"
#include "protocol/AX25.h"
#include "protocol/Digipeater.h"

Serial serial;
Afsk modem;
AX25Ctx AX25;

unsigned long custom_preamble = CONFIG_AFSK_PREAMBLE_LEN;
unsigned long custom_tail = CONFIG_AFSK_TRAILER_LEN;


// ===== AYAR BLOGU =====
// Bu blok flash'ta sabit bir yerde durur. Web araci hex dosyasinda
// "APRSCFG1" isaretini bulup bu alanlari degistirir.
typedef struct __attribute__((packed)) {
    char magic[8];          // "APRSCFG1"
    char call[7];           // cagri isareti, NUL ile doldurulur
    uint8_t ssid;
    char dest[7];           // hedef (APZMDG)
    char path1[7];          // ornek "WIDE1"; bos = yol yok
    uint8_t path1_ssid;
    char path2[7];          // ornek "WIDE2"; bos = ikinci adim yok
    uint8_t path2_ssid;
    uint16_t first_s;       // acilistan sonra ilk beacon (sn)
    uint16_t interval_min;  // beacon araligi (dk), 0 = kapali
    uint8_t clamp_n;        // en fazla kac WIDE adimi tekrarlanir (1 = fill-in)
    char info[81];          // APRS bilgi alani (konum + sembol + aciklama)
} cfg_t;

const cfg_t CFG PROGMEM __attribute__((used)) = {
    {'A','P','R','S','C','F','G','1'},
    "NOCALL", 0,            // varsayilan: ayar araci ile degistirin
    "APZMDG",
    "WIDE1", 1,
    "WIDE2", 1,
    30, 0, 2,               // beacon varsayilan olarak kapali
    ""
};

char digi_call[7];
uint8_t digi_ssid;
extern int clamp_n;

static uint8_t cfg_b(const void *p) { return pgm_read_byte(p); }
static uint16_t cfg_w(const void *p) { return pgm_read_word(p); }

static void ax25_callback(struct AX25Ctx *ctx) {
    digipeater_messageCallback(ctx);
}


static void addr_put(uint8_t *b, const char *call, uint8_t ssid, bool last) {
    uint8_t n = strlen(call);
    for (uint8_t i = 0; i < 6; i++) b[i] = (i < n ? call[i] : ' ') << 1;
    b[6] = 0x60 | ((ssid & 0x0F) << 1) | (last ? 1 : 0);
}

static void addr_put_P(uint8_t *b, const char *call_P, uint8_t ssid, bool last) {
    char tmp[7];
    memcpy_P(tmp, call_P, 7); tmp[6] = 0;
    addr_put(b, tmp, ssid, last);
}

static void send_beacon(void) {
    static uint8_t f[7 * 4 + 2 + 81];
    uint8_t len = 0;
    bool has1 = cfg_b(&CFG.path1[0]) != 0;
    bool has2 = has1 && cfg_b(&CFG.path2[0]) != 0;
    addr_put_P(f + len, CFG.dest, 0, false); f[len + 6] |= 0x80; len += 7;
    addr_put(f + len, digi_call, digi_ssid, !has1); len += 7;
    if (has1) { addr_put_P(f + len, CFG.path1, cfg_b(&CFG.path1_ssid), !has2); len += 7; }
    if (has2) { addr_put_P(f + len, CFG.path2, cfg_b(&CFG.path2_ssid), true); len += 7; }
    f[len++] = 0x03;
    f[len++] = 0xF0;
    for (uint8_t i = 0; i < 80; i++) {
        char ch = cfg_b(&CFG.info[i]);
        if (!ch) break;
        f[len++] = ch;
    }
    digipeater_csma(&AX25, f, len);
    printf_P(PSTR("Beacon gonderildi.\n"));
}

void init(void) {
    memcpy_P(digi_call, CFG.call, 7); digi_call[6] = 0;
    digi_ssid = cfg_b(&CFG.ssid);
    clamp_n = cfg_b(&CFG.clamp_n);
    sei();

    AFSK_init(&modem);
    ax25_init(&AX25, &modem.fd, ax25_callback);
    digipeater_init(&AX25, &modem, &serial);

    serial_init(&serial);
    stdout = &serial.uart0;
    stdin  = &serial.uart0;

    printf_P(PSTR("\n%s-%d digi basladi. "), digi_call, digi_ssid);
    if (cfg_w(&CFG.interval_min))
        printf_P(PSTR("Ilk beacon %u sn sonra, sonra %u dk arayla.\n"), cfg_w(&CFG.first_s), cfg_w(&CFG.interval_min));
    else
        printf_P(PSTR("Beacon kapali (test icin b + Enter).\n"));

}

int main (void) {
    init();

    uint32_t last_beacon = (uint32_t)timer_clock();
    uint32_t interval_ticks = (uint32_t)cfg_w(&CFG.interval_min) * 60UL * CLOCK_TICKS_PER_SEC;
    bool beacon_on = cfg_w(&CFG.interval_min) != 0;
    uint32_t wait_ticks = (uint32_t)cfg_w(&CFG.first_s) * CLOCK_TICKS_PER_SEC;

    while (true) {
        uint32_t now = (uint32_t)timer_clock();
        if (beacon_on && now - last_beacon >= wait_ticks) {
            send_beacon();
            last_beacon = (uint32_t)timer_clock();
            wait_ticks = interval_ticks;
        }

        ax25_poll(&AX25);
        digipeater_processPackets();
        
        if (serial_available(0)) {
            char sbyte = uart0_getchar_nowait();
            if (sbyte == 'b' || sbyte == 'B') {
                send_beacon();
                last_beacon = (uint32_t)timer_clock();
                wait_ticks = interval_ticks;
            }
        }
    }

    return(0);
}