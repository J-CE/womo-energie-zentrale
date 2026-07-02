// ============================================================
//  watchdog.cpp — Womo Energy Core v5.4
//  Software-Watchdog: Modul-Liveness-Überwachung
//
//  Überwachte Module: WDT_BMS (6s), WDT_MPPT (6s), WDT_LOGGER (10s)
//  Timeout → logger_emergency_back_up() → ESP.restart()
//  lastKick[]: volatile uint32_t — Cross-Core-sicher (atomar auf
//    Xtensa + volatile verhindert Compiler-Caching)
// ============================================================
#include "watchdog.h"
#include "logger.h"          // logger_emergency_back_up()
#include <esp_task_wdt.h>

#define WDT_NONE        0xFFFFFFFFu
#define WDT_MAGIC       0xC0FFEE42u
#define COOLDOWN_15MIN  900000u      // 15 min in ms

// ── RTC-Memory: übersteht SW-Reset, nicht Power-On ────────────
RTC_NOINIT_ATTR static uint32_t s_persMagic;
RTC_NOINIT_ATTR static uint32_t s_persFailModule;

// ── Modul-Timeouts (ms) ───────────────────────────────────────
static const uint32_t WDT_TIMEOUTS[WDT_NUM_MODULES] = {
    6000,   // WDT_BMS    (Poll alle 2s)
    6000,   // WDT_MPPT   (Poll alle 100ms)
    10000   // WDT_LOGGER (Task-Tick alle 5s)
};
static const char* WDT_NAMES[WDT_NUM_MODULES] = {
    "BMS_PARSER", "MPPT_PARSER", "LOGGER_SD"
};

// volatile: Cross-Core-Sichtbarkeit — kick aus logic/mppt/logger-Tasks (Core 0+1),
// Auswertung in watchdog_task (Core 0). 32-bit-Zugriffe sind atomar auf Xtensa,
// volatile verhindert Wegoptimieren/Caching der Reads.
static volatile uint32_t lastKick[WDT_NUM_MODULES];
static volatile bool     inError[WDT_NUM_MODULES];
static volatile uint32_t errorStart[WDT_NUM_MODULES];

void watchdog_kick(WatchdogModule m) {
    if (m < WDT_NUM_MODULES) lastKick[m] = millis();
}

static void reboot_with_save(int i) {
    Serial.flush();
    logger_emergency_back_up(WDT_NAMES[i]);   // immer erst Daten sichern
    s_persMagic      = WDT_MAGIC;
    s_persFailModule = (uint32_t)i;            // Grund über Reboot hinweg merken
    Serial.flush();
    ESP.restart();
}

static void watchdog_task(void*) {
    // Eigene Task ebenfalls unter HW-WDT: blockiert die SD-Sicherung,
    // greift nach 4 s der harte Reset statt eines Dauerhängers.
    esp_task_wdt_add(NULL);

    for (;;) {
        esp_task_wdt_reset();
        uint32_t now = millis();

        for (int i = 0; i < WDT_NUM_MODULES; i++) {
            bool late = (now - lastKick[i]) > WDT_TIMEOUTS[i];

            if (!late) {
                // Modul lebt
                if (inError[i]) {
                    inError[i] = false;
                    Serial.printf("[WDT] ENTWARNUNG: %s wieder aktiv.\n", WDT_NAMES[i]);
                }
                if (s_persFailModule == (uint32_t)i) {
                    Serial.printf("[WDT] %s nach Reboot erholt — wieder normal.\n", WDT_NAMES[i]);
                    s_persFailModule = WDT_NONE;
                }
                continue;
            }

            // ── Modul i hängt ──────────────────────────────────
            if (s_persFailModule == (uint32_t)i) {
                // Wiederholungsfall: war schon Reboot-Grund, weiterhin tot
                if (!inError[i]) {
                    inError[i]    = true;
                    errorStart[i] = now;
                    Serial.printf("[WDT] %s weiterhin tot nach Reboot — 15 min degradiert, "
                                  "dann erneuter Versuch.\n", WDT_NAMES[i]);
                }
                if (now - errorStart[i] > COOLDOWN_15MIN) {
                    Serial.printf("[WDT] %s: 15 min abgelaufen — erneuter Reboot-Versuch.\n",
                                  WDT_NAMES[i]);
                    reboot_with_save(i);
                }
            } else {
                // Frischer Fehler → sofort sichern + reboot
                Serial.printf("[WDT] %s hängt — Notsicherung + sofortiger Reboot.\n",
                              WDT_NAMES[i]);
                reboot_with_save(i);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void watchdog_init() {
    // RTC-Memory: nur nach SW-Reset gültig, nach Power-On Müll
    if (s_persMagic != WDT_MAGIC) {
        s_persMagic      = WDT_MAGIC;
        s_persFailModule = WDT_NONE;
        Serial.println("[WATCHDOG] Kaltstart — kein vorheriger Modulfehler.");
    } else if (s_persFailModule != WDT_NONE && s_persFailModule < WDT_NUM_MODULES) {
        Serial.printf("[WATCHDOG] Letzter Reboot wegen %s — beobachte, ob es zurückkommt.\n",
                      WDT_NAMES[s_persFailModule]);
    }

    uint32_t now = millis();
    for (int i = 0; i < WDT_NUM_MODULES; i++) {
        lastKick[i]   = now;
        inError[i]    = false;
        errorStart[i] = now;
    }

    // K-4: 6144 statt 4096 — logger_emergency_back_up() läuft in diesem Task
    // und ruft die tiefe SD-Schreibkette auf (Off-Stack-Puffer allein reicht
    // als Reserve nicht sicher im Reboot-Moment).
    // F-01: Erzeugung prüfen. Schlägt sie fehl, gibt es KEINE SW-Überwachung —
    // die "aktiv"-Meldung darf das dann nicht behaupten. Der HW-WDT auf loop()
    // (in main.cpp) bleibt als letzte Instanz aktiv.
    BaseType_t rc = xTaskCreatePinnedToCore(watchdog_task, "wdt_task", 6144, NULL, 1, NULL, 0);
    if (rc != pdPASS) {
        Serial.println(F("[WATCHDOG] FEHLER: watchdog_task nicht erstellt — "
                         "KEINE SW-Modulüberwachung! (nur HW-WDT auf loop())"));
        return;
    }
    Serial.println(F("[WATCHDOG] Software-Modulüberwachung aktiv + HW-WDT auf loop()."));
}
