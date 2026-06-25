// ============================================================
//  io.cpp — Womo Energy Core v5.1
//  GPIO-Treiber für alle Aktoren und Sensoren
//
//  Alle Ausgänge werden in io_init() explizit LOW gesetzt
//  (Fail-Safe: kein Ausgang aktiv bei Reset/Boot).
//  io_read_landstrom(): INPUT_PULLUP, LOW = Landstrom aktiv.
// ============================================================
#include "io.h"
#include "config.h"

IOState g_io = {};

// LED-Status-Variablen (Cross-Core: nur LED-Task schreibt die Hardware)
volatile RgbBootState g_rgbState        = RGB_BOOT;
volatile uint32_t     g_rgbChannelMask  = 0;
volatile bool         g_rgbHeapAlert    = false;

void io_init() {
    // Eingänge
    pinMode(GPIO_LANDSTROM_SENSOR, INPUT_PULLUP);

    // Ausgänge — zuerst sicheren Zustand setzen, dann pinMode
    // D+ Relais: Active-LOW → HIGH = sicher (Relais abgefallen)
    digitalWrite(GPIO_RELAY_D_PLUS,   HIGH);
    pinMode(GPIO_RELAY_D_PLUS,         OUTPUT);

    // MOSFET Gel: Active-HIGH → LOW = sicher (MOSFET sperrt)
    digitalWrite(GPIO_MOSFET_GEL,     LOW);
    pinMode(GPIO_MOSFET_GEL,           OUTPUT);

    // WR Remote Optokoppler: Active-HIGH → LOW = sicher
    digitalWrite(GPIO_OPTO_WR_REMOTE, LOW);
    pinMode(GPIO_OPTO_WR_REMOTE,       OUTPUT);

    // Status-LED
    digitalWrite(GPIO_STATUS_LED,     LOW);
    pinMode(GPIO_STATUS_LED,           OUTPUT);

    memset(&g_io, 0, sizeof(g_io));

    Serial.println("[IO] GPIO initialisiert — alle Aktoren im Fail-Safe");
    Serial.printf ("[IO] D+=%d GEL=%d WR=%d LED=%d LAND=%d\n",
        GPIO_RELAY_D_PLUS, GPIO_MOSFET_GEL,
        GPIO_OPTO_WR_REMOTE, GPIO_STATUS_LED,
        GPIO_LANDSTROM_SENSOR);
}

bool io_read_landstrom() {
    // Optokoppler mit Pull-Up: LOW wenn Landstrom (Optokoppler leitet)
    // HIGH wenn kein Landstrom (Pull-Up zieht HIGH)
    // → invertieren für logisch "Landstrom = true"
    g_io.landstrom = (digitalRead(GPIO_LANDSTROM_SENSOR) == LOW);
    return g_io.landstrom;
}

void io_set_relay_dplus(bool on) {
    // Active-LOW: ON → LOW, OFF → HIGH
    digitalWrite(GPIO_RELAY_D_PLUS, on ? RELAY_D_PLUS_ACTIVE : !RELAY_D_PLUS_ACTIVE);
    g_io.relayDPlus = on;
    Serial.printf("[IO] D+ Relais: %s\n", on ? "EIN" : "AUS");
}

void io_set_mosfet_gel(bool on) {
    digitalWrite(GPIO_MOSFET_GEL, on ? MOSFET_GEL_ACTIVE : !MOSFET_GEL_ACTIVE);
    g_io.mosfetGel = on;
    Serial.printf("[IO] Gel-Lader MOSFET: %s\n", on ? "EIN" : "AUS");
}

void io_set_wr_remote(bool on) {
    digitalWrite(GPIO_OPTO_WR_REMOTE, on ? OPTO_WR_ACTIVE : !OPTO_WR_ACTIVE);
    g_io.wrRemote = on;
    Serial.printf("[IO] WR Remote: %s\n", on ? "EIN" : "AUS");
}

void io_set_status_led(bool on) {
    digitalWrite(GPIO_STATUS_LED, on ? HIGH : LOW);
    g_io.statusLed = on;
}

void io_blink_status_led(uint8_t times, uint32_t ms_on, uint32_t ms_off) {
    for (uint8_t i = 0; i < times; i++) {
        io_set_status_led(true);
        vTaskDelay(pdMS_TO_TICKS(ms_on));
        io_set_status_led(false);
        if (i < times - 1) vTaskDelay(pdMS_TO_TICKS(ms_off));
    }
}

// ============================================================
//  On-board RGB-LED (WS2812, GPIO 48)
// ============================================================

void io_rgb_set(uint8_t r, uint8_t g, uint8_t b) {
    // Globale Dimmung (RGB_LED_BRIGHTNESS) auf die Basisfarbe anwenden.
    // neopixelWrite() ist RMT-basiert und übernimmt die GRB-Sortierung.
    uint8_t rr = (uint16_t)r * RGB_LED_BRIGHTNESS / 255;
    uint8_t gg = (uint16_t)g * RGB_LED_BRIGHTNESS / 255;
    uint8_t bb = (uint16_t)b * RGB_LED_BRIGHTNESS / 255;
    neopixelWrite(GPIO_RGB_LED, rr, gg, bb);
}

void io_rgb_init() {
    // Beim Boot weiß zeigen — sichtbares Lebenszeichen, bevor Tasks laufen.
    g_rgbState = RGB_BOOT;
    io_rgb_set(255, 255, 255);
    Serial.printf("[IO] RGB-LED auf GPIO %d initialisiert (Helligkeit %d/255)\n",
                  GPIO_RGB_LED, RGB_LED_BRIGHTNESS);
}

// LED-Render-Task: liest g_rgbChannelMask/g_rgbHeapAlert und erzeugt
// das Rundlauf-Muster mit eigener Zeitbasis. Nicht sicherheitskritisch
// → NICHT unter den HW-Watchdog gehängt. Einziger Schreiber der
// LED-Hardware.
//
// Schema: 7 Slots fester Länge (SLOT_MS) laufen nacheinander durch.
// Jeder Slot zeigt seine Farbe nur, wenn der Kanal aktiv ist — sonst
// bleibt der Slot dunkel. Die Slot-DAUER ist immer gleich, damit die
// Position im Zyklus konstant bleibt (Position = Kanal-Identität,
// unabhängig davon, wie viele andere Kanäle gerade aktiv sind).
// Nach dem letzten Slot folgt eine längere Dunkelphase als
// Zyklus-Ende-Marker, bevor der Rundlauf neu beginnt.
static const uint8_t  RGB_NUM_SLOTS      = 7;
static const uint32_t RGB_SLOT_ON_MS     = 220;   // Farbe sichtbar
static const uint32_t RGB_SLOT_GAP_MS    = 40;    // Trennlücke zum nächsten Slot
static const uint32_t RGB_SLOT_MS        = RGB_SLOT_ON_MS + RGB_SLOT_GAP_MS;
static const uint32_t RGB_CYCLE_PAUSE_MS = 560;   // Zyklus-Ende-Marker
static const uint32_t RGB_CYCLE_MS       = RGB_NUM_SLOTS * RGB_SLOT_MS + RGB_CYCLE_PAUSE_MS;

static void led_task(void*) {
    const uint32_t t0 = millis();
    for (;;) {
        uint32_t t = millis() - t0;

        // 1) Heap-Mangel: Selbstschutz-Signal, kein Betriebszustand —
        //    übersteuert den Rundlauf sofort und ausnahmslos (Magenta).
        if (g_rgbHeapAlert) {
            bool on = (t / 120) & 1;
            io_rgb_set(on ? 255 : 0, 0, on ? 255 : 0);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        // 2) Boot: weiß, bis logic_evaluate() zum ersten Mal lief.
        if (g_rgbState == RGB_BOOT) {
            io_rgb_set(255, 255, 255);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        // 3) Normalbetrieb: Rundlauf über alle 7 Kanäle, gleichberechtigt.
        uint32_t mask = g_rgbChannelMask;             // ein atomarer Read
        uint8_t  soc  = (mask >> RGB_SOC_SHIFT) & 0xFF;
        uint32_t tc   = t % RGB_CYCLE_MS;

        if (tc >= (uint32_t)RGB_NUM_SLOTS * RGB_SLOT_MS) {
            io_rgb_set(0, 0, 0);                       // Zyklus-Pause
        } else {
            uint8_t  slot   = tc / RGB_SLOT_MS;
            uint32_t inSlot = tc % RGB_SLOT_MS;
            if (inSlot >= RGB_SLOT_ON_MS) {
                io_rgb_set(0, 0, 0);                   // Slot-Lücke
            } else {
                switch (slot) {
                    case 0:  // BMS-Fehler — rot
                        if (mask & RGB_BIT_BMS_FAULT) io_rgb_set(255, 0, 0);
                        else                           io_rgb_set(0, 0, 0);
                        break;
                    case 1:  // MPPT-Fehler — orange
                        if (mask & RGB_BIT_MPPT_FAULT) io_rgb_set(255, 90, 0);
                        else                            io_rgb_set(0, 0, 0);
                        break;
                    case 2:  // SoC-Gauge — immer sichtbar, Farbverlauf
                        if      (soc < RGB_SOC_GAUGE_CRIT_PCT) io_rgb_set(255, 0, 0);
                        else if (soc < RGB_SOC_GAUGE_LOW_PCT)  io_rgb_set(255, 60, 0);
                        else if (soc < RGB_SOC_GAUGE_MID_PCT)  io_rgb_set(255, 180, 0);
                        else                                     io_rgb_set(0, 200, 0);
                        break;
                    case 3:  // Landstrom — blau
                        if (mask & RGB_BIT_LANDSTROM) io_rgb_set(0, 60, 255);
                        else                            io_rgb_set(0, 0, 0);
                        break;
                    case 4:  // D+ Kühlschrank — cyan
                        if (mask & RGB_BIT_DPLUS) io_rgb_set(0, 200, 200);
                        else                        io_rgb_set(0, 0, 0);
                        break;
                    case 5:  // Gel-Lader — violett
                        if (mask & RGB_BIT_GEL) io_rgb_set(140, 0, 255);
                        else                       io_rgb_set(0, 0, 0);
                        break;
                    case 6:  // WR-Remote — helles Blau-Weiß
                        if (mask & RGB_BIT_WR) io_rgb_set(180, 180, 255);
                        else                      io_rgb_set(0, 0, 0);
                        break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));            // ~25 Hz Render-Takt
    }
}

void io_rgb_task_start() {
    xTaskCreatePinnedToCore(led_task, "led", 2048, nullptr, 1, nullptr, 0);
    Serial.println("[IO] LED-Render-Task gestartet (Core 0)");
}

void io_all_safe() {
    io_set_relay_dplus(false);
    io_set_mosfet_gel(false);
    io_set_wr_remote(false);
    io_set_status_led(false);
    Serial.println("[IO] Fail-Safe: alle Aktoren AUS");
}

String io_to_json() {
    io_read_landstrom();
    uint32_t mask = g_rgbChannelMask;
    uint8_t  soc  = (mask >> RGB_SOC_SHIFT) & 0xFF;
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"landstrom\":%s,\"relay_dplus\":%s,"
        "\"mosfet_gel\":%s,\"wr_remote\":%s,"
        "\"status_led\":%s,"
        "\"rgb\":{\"bms_fault\":%s,\"mppt_fault\":%s,\"soc\":%u,"
        "\"landstrom\":%s,\"d_plus\":%s,\"gel_lader\":%s,\"wr_remote\":%s}}",
        g_io.landstrom  ? "true" : "false",
        g_io.relayDPlus ? "true" : "false",
        g_io.mosfetGel  ? "true" : "false",
        g_io.wrRemote   ? "true" : "false",
        g_io.statusLed  ? "true" : "false",
        (mask & RGB_BIT_BMS_FAULT)  ? "true" : "false",
        (mask & RGB_BIT_MPPT_FAULT) ? "true" : "false",
        soc,
        (mask & RGB_BIT_LANDSTROM) ? "true" : "false",
        (mask & RGB_BIT_DPLUS)     ? "true" : "false",
        (mask & RGB_BIT_GEL)       ? "true" : "false",
        (mask & RGB_BIT_WR)        ? "true" : "false");
    return String(buf);
}