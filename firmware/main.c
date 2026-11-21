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
// "APRSCFG2" isaretini bulup bu alanlari degistirir.
typedef struct __attribute__((packed)) {
    char magic[8];          // "APRSCFG2"
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
    char info[101];         // APRS bilgi alani (konum + sembol + aciklama)
    uint16_t tlm_min;       // telemetri araligi (dk), 0 = kapali
} cfg_t;

#define INFO_MAX 100

const cfg_t CFG PROGMEM __attribute__((used)) = {
    {'A','P','R','S','C','F','G','2'},
    "NOCALL", 0,            // varsayilan: ayar araci ile degistirin
    "APZMDG",
    "WIDE1", 1,
    "WIDE2", 1,
    30, 0, 2,               // beacon varsayilan olarak kapali
    "",
    0                       // telemetri varsayilan olarak kapali
};

char digi_call[7];
uint8_t digi_ssid;
extern int clamp_n;
extern volatile uint16_t tlm_rx, tlm_dg;

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

// Beacon/telemetri cercevesi icin digipeater'in cikis tamponunu kullan (RAM tasarrufu).
// Bu tampon sadece digipeater_processPackets() icinde kullaniliyor; ana dongude cakisma olmaz.
extern uint8_t packetBufferOut[];
#define frame packetBufferOut
#define FRAME_MAX (7 * 4 + 2 + INFO_MAX + 1)

// Cerceve basligini (hedef, kaynak, yol, kontrol, PID) yazar; bilgi alaninin baslangicini dondurur
static uint8_t frame_header(void) {
    uint8_t len = 0;
    bool has1 = cfg_b(&CFG.path1[0]) != 0;
    bool has2 = has1 && cfg_b(&CFG.path2[0]) != 0;
    addr_put_P(frame + len, CFG.dest, 0, false); frame[len + 6] |= 0x80; len += 7;
    addr_put(frame + len, digi_call, digi_ssid, !has1); len += 7;
    if (has1) { addr_put_P(frame + len, CFG.path1, cfg_b(&CFG.path1_ssid), !has2); len += 7; }
    if (has2) { addr_put_P(frame + len, CFG.path2, cfg_b(&CFG.path2_ssid), true); len += 7; }
    frame[len++] = 0x03;
    frame[len++] = 0xF0;
    return len;
}

static void send_beacon(void) {
    uint8_t len = frame_header();
    for (uint8_t i = 0; i < INFO_MAX; i++) {
        char ch = cfg_b(&CFG.info[i]);
        if (!ch) break;
        frame[len++] = ch;
    }
    digipeater_csma(&AX25, frame, len);
    printf_P(PSTR("Beacon gonderildi.\n"));
}

// ===== TELEMETRI =====
// Her aralikta: saatlik duyulan paket (Rx1h) ve saatlik tekrarlanan paket (Dg1h).
// Tanim paketleri (PARM/UNIT/EQNS) ilk seferde ve her 6 telemetride bir gonderilir.
#define TLM_PARM 0x01
#define TLM_UNIT 0x02
#define TLM_EQNS 0x04
#define TLM_DATA 0x08
static uint8_t tlm_pending = 0;
static uint8_t tlm_cycle = 0;
static uint16_t tlm_seq = 0;
static uint16_t tlm_rx_rate = 0, tlm_dg_rate = 0;
static uint32_t tlm_last = 0;
static uint32_t tlm_last_tx = 0;

static void tlm_addressee(char *a) {     // "TA4OGM-1 " (9 karakter)
    uint8_t n = snprintf_P(a, 10, PSTR("%s-%d"), digi_call, digi_ssid);
    if (!digi_ssid) n = snprintf_P(a, 10, PSTR("%s"), digi_call);
    while (n < 9) a[n++] = ' ';
    a[9] = 0;
}

static void tlm_due(uint32_t now, bool defs) {
    uint32_t elapsed_s = (now - tlm_last) / CLOCK_TICKS_PER_SEC;
    if (elapsed_s < 60) elapsed_s = 60;
    tlm_rx_rate = (uint16_t)(((uint32_t)tlm_rx * 3600UL) / elapsed_s);
    tlm_dg_rate = (uint16_t)(((uint32_t)tlm_dg * 3600UL) / elapsed_s);
    tlm_rx = 0; tlm_dg = 0;
    tlm_last = now;
    if (defs || tlm_cycle == 0) tlm_pending |= TLM_PARM | TLM_UNIT | TLM_EQNS;
    tlm_pending |= TLM_DATA;
    tlm_cycle = (tlm_cycle + 1) % 6;
}

static void tlm_send_next(void) {
    char a[10];
    uint8_t len = frame_header();
    char *p = (char *)frame + len;
    uint8_t room = FRAME_MAX - len;
    tlm_addressee(a);
    if (tlm_pending & TLM_PARM) {
        len += snprintf_P(p, room, PSTR(":%s:PARM.Rx1h,Dg1h"), a);
        tlm_pending &= ~TLM_PARM;
    } else if (tlm_pending & TLM_UNIT) {
        len += snprintf_P(p, room, PSTR(":%s:UNIT.Pkt,Pkt"), a);
        tlm_pending &= ~TLM_UNIT;
    } else if (tlm_pending & TLM_EQNS) {
        len += snprintf_P(p, room, PSTR(":%s:EQNS.0,2,0,0,2,0,0,0,0,0,0,0,0,0,0"), a);
        tlm_pending &= ~TLM_EQNS;
    } else if (tlm_pending & TLM_DATA) {
        uint16_t rx = (tlm_rx_rate + 1) / 2; if (rx > 255) rx = 255;
        uint16_t dg = (tlm_dg_rate + 1) / 2; if (dg > 255) dg = 255;
        len += snprintf_P(p, room, PSTR("T#%03u,%03u,%03u,000,000,000,00000000"), tlm_seq, rx, dg);
        tlm_seq = (tlm_seq + 1) % 1000;
        tlm_pending &= ~TLM_DATA;
        printf_P(PSTR("Telemetri: Rx %u/saat, Dg %u/saat\n"), tlm_rx_rate, tlm_dg_rate);
    } else {
        return;
    }
    digipeater_csma(&AX25, frame, len);
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
    if (cfg_w(&CFG.tlm_min))
        printf_P(PSTR("Telemetri %u dk arayla (test icin t + Enter).\n"), cfg_w(&CFG.tlm_min));
}

int main (void) {
    init();

    uint32_t last_beacon = (uint32_t)timer_clock();
    uint32_t interval_ticks = (uint32_t)cfg_w(&CFG.interval_min) * 60UL * CLOCK_TICKS_PER_SEC;
    bool beacon_on = cfg_w(&CFG.interval_min) != 0;
    uint32_t wait_ticks = (uint32_t)cfg_w(&CFG.first_s) * CLOCK_TICKS_PER_SEC;

    bool tlm_on = cfg_w(&CFG.tlm_min) != 0;
    uint32_t tlm_ticks = (uint32_t)cfg_w(&CFG.tlm_min) * 60UL * CLOCK_TICKS_PER_SEC;
    tlm_last = (uint32_t)timer_clock();

    while (true) {
        uint32_t now = (uint32_t)timer_clock();
        if (beacon_on && now - last_beacon >= wait_ticks) {
            send_beacon();
            last_beacon = (uint32_t)timer_clock();
            wait_ticks = interval_ticks;
            tlm_last_tx = last_beacon;
        }

        if (tlm_on && !tlm_pending && now - tlm_last >= tlm_ticks) tlm_due(now, false);

        // Telemetri paketlerini kanal bosken, aralarinda 3 sn birakarak gonder
        if (tlm_pending && !modem.hdlc.receiving && !hw_afsk_dac_isr &&
            now - tlm_last_tx >= 3UL * CLOCK_TICKS_PER_SEC) {
            tlm_send_next();
            tlm_last_tx = (uint32_t)timer_clock();
        }

        ax25_poll(&AX25);
        digipeater_processPackets();

        if (serial_available(0)) {
            char sbyte = uart0_getchar_nowait();
            if (sbyte == 'b' || sbyte == 'B') {
                send_beacon();
                last_beacon = (uint32_t)timer_clock();
                wait_ticks = interval_ticks;
                tlm_last_tx = last_beacon;
            }
            if ((sbyte == 't' || sbyte == 'T') && !tlm_pending) {
                tlm_due((uint32_t)timer_clock(), true);
            }
        }
    }

    return(0);
}
