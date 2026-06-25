// ============================================================
//  io.cpp — Womo Energy Core v5.0
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
volatile RgbState g_rgbState     = RGB_BOOT;
volatile bool     g_rgbHeapAlert = false;

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

const char* io_rgb_state_name(RgbState s) {
    switch (s) {
        case RGB_BOOT:       return "boot";
        case RGB_FAULT_BMS:  return "fault_bms";
        case RGB_FAULT_MPPT: return "fault_mppt";
        case RGB_SOC_LOW:    return "soc_low";
        case RGB_LANDSTROM:  return "landstrom";
        case RGB_ACTIVE:     return "active";
        case RGB_IDLE_OK:    return "idle_ok";
        case RGB_OFF:
        default:             return "off";
    }
}

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

// LED-Render-Task: liest g_rgbState/g_rgbHeapAlert und erzeugt das
// Muster mit eigener Zeitbasis. Nicht sicherheitskritisch → NICHT
// unter den HW-Watchdog gehängt. Einziger Schreiber der LED-Hardware.
static void led_task(void*) {
    const uint32_t t0 = millis();
    for (;;) {
        uint32_t t = millis() - t0;

        // 1) Heap-Mangel hat absoluten Vorrang — Magenta, schnell.
        if (g_rgbHeapAlert) {
            bool on = (t / 120) & 1;
            io_rgb_set(on ? 255 : 0, 0, on ? 255 : 0);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        switch (g_rgbState) {
            case RGB_FAULT_BMS: {                 // rot, schnelles Blinken
                bool on = (t / 200) & 1;
                io_rgb_set(on ? 255 : 0, 0, 0);
                break;
            }
            case RGB_FAULT_MPPT: {                // orange, mittleres Blinken
                bool on = (t / 400) & 1;
                io_rgb_set(on ? 255 : 0, on ? 90 : 0, 0);
                break;
            }
            case RGB_SOC_LOW:                     // gelb, Dauerlicht
                io_rgb_set(255, 150, 0);
                break;
            case RGB_LANDSTROM:                   // blau, Dauerlicht
                io_rgb_set(0, 60, 255);
                break;
            case RGB_ACTIVE: {                    // grün, Pulsieren (Dreieck, 2s)
                uint32_t ph  = t % 2000;
                uint8_t  lvl = (ph < 1000) ? (ph * 255 / 1000)
                                           : (255 - (ph - 1000) * 255 / 1000);
                io_rgb_set(0, lvl, 0);
                break;
            }
            case RGB_IDLE_OK:                     // grün, gedimmt Dauerlicht
                io_rgb_set(0, 80, 0);
                break;
            case RGB_BOOT:                        // weiß
                io_rgb_set(255, 255, 255);
                break;
            case RGB_OFF:
            default:
                io_rgb_set(0, 0, 0);
                break;
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
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"landstrom\":%s,\"relay_dplus\":%s,"
        "\"mosfet_gel\":%s,\"wr_remote\":%s,"
        "\"status_led\":%s,\"rgb\":\"%s\"}",
        g_io.landstrom  ? "true" : "false",
        g_io.relayDPlus ? "true" : "false",
        g_io.mosfetGel  ? "true" : "false",
        g_io.wrRemote   ? "true" : "false",
        g_io.statusLed  ? "true" : "false",
        io_rgb_state_name(g_rgbState));
    return String(buf);
}