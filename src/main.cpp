// ============================================================
//  main.cpp — Womo Energy Core v5.4
//
//  Watchdog (zweistufig):
//   • HW-WDT (esp_task_wdt, 4s) überwacht loop() → harter Reboot
//   • SW-WDT (watchdog.cpp) überwacht BMS/MPPT/Logger
//
//  Tasks:
//   Core 1 | Prio 4: logic_task    (BMS poll + Logik, alle 2s)
//   Core 1 | Prio 3: mppt_task     (VE.Direct lesen + HEX-TX Temp)
//   Core 0 | Prio 2: logger_task   (SD flush, alle 5s)
//   Core 0 | Prio 2: ws_task       (WebSocket broadcast, alle 2s)
//   Core 0 | Prio 1: watchdog_task (watchdog.cpp)
// ============================================================

#include <Arduino.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "params.h"
#include "bms.h"
#include "mppt.h"
#include "inverter.h"
#include "io.h"
#include "logic.h"
#include "logger.h"
#include "http_server.h"
#include "watchdog.h"
#include "clock.h"
#include "level.h"

static TaskHandle_t h_logic  = nullptr;
static TaskHandle_t h_mppt   = nullptr;
static TaskHandle_t h_logger = nullptr;
static TaskHandle_t h_ws     = nullptr;
static TaskHandle_t h_level  = nullptr;

// ── BMS abfragen + Logik (Core 1, 2s-Takt) ───────────────────
static void logic_task(void*) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        watchdog_kick(WDT_BMS);
        if (!bms_poll()) Serial.println("[MAIN] BMS: kein Frame");
        logic_evaluate();
        logger_append();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(BMS_POLL_INTERVAL_MS));
    }
}

// ── MPPT VE.Direct lesen + HEX-TX Temperatur (Core 1) ────────
// HEX-TX alle MPPT_HEX_SEND_INTERVAL_MS (10s)
static void mppt_task(void*) {
    uint32_t last_hex_tx = 0;
    for (;;) {
        watchdog_kick(WDT_MPPT);
        mppt_poll();
        inverter_poll();

        // BMS-Kerntemperatur periodisch an MPPT senden (HEX-TX, Register 0x2003)
        // Timeout MPPT: 60s → Intervall 10s ausreichend
        if ((uint32_t)(millis() - last_hex_tx) >= MPPT_HEX_SEND_INTERVAL_MS) {
            // Zellentemperatur (0x82 "battery temperature") senden
            // NOT tempMOS (0x80 "power tube") — der MPPT braucht Akkutemp!
            float cell_temp = 0.0f;
            bool  bms_valid = false;
            if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                cell_temp = g_bms.tempSensor2;   // 0x82 = Zellentemperatur
                bms_valid = g_bms.valid &&
                    (uint32_t)(millis() - g_bms.lastUpdateMs) < BMS_STALE_TIMEOUT_MS;
                xSemaphoreGive(g_bmsMutex);
            }
            if (bms_valid) {
                // Zellentemperatur → MPPT Register 0x2003
                mppt_send_temperature(cell_temp);
            } else {
                // BMS veraltet/ungültig → N/A senden (0x7FFF)
                // MPPT fällt sofort auf interne Temperaturmessung zurück
                mppt_send_temp_na();
            }
            last_hex_tx = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── Logger SD flush (Core 0, alle 5s) ────────────────────────
static void logger_task(void*) {
    for (;;) {
        watchdog_kick(WDT_LOGGER);
        logger_flush_sd();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ── WebSocket Broadcast (Core 0, alle 2s) ────────────────────
static void ws_task(void*) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        webserver_broadcast();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(2000));
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    // K-1: USB-CDC verwirft TX-Daten statt zu blockieren, wenn kein Host
    // liest (Normalfall im Womo-Betrieb ohne angeschlossenen PC). Ohne das
    // kann Serial.write() die aufrufende Task bis zum TX-Timeout blockieren —
    // fatal, wenn das im gehaltenen g_bmsMutex passiert (→ SW-WDT-Reboot).
    Serial.setTxTimeoutMs(0);
    delay(500);

    Serial.println("\n╔═══════════════════════════════╗");
    Serial.println("║   GODMODULE — Energy Core     ║");
    Serial.println("║   Womo Energy Core v5.4       ║");
    Serial.println("╚═══════════════════════════════╝\n");

    io_init();
    io_rgb_init();        // RGB-LED auf weiß (Boot-Lebenszeichen)
    params_init();
    bms_init();
    mppt_init();          // UART2 RX=38 TX=42
    inverter_init();
    logic_init();
    logger_init();
    clock_init();
    level_init();         // Lagesensor (optional) — nach clock_init: Wire + g_i2cMutex bereit
    webserver_init();

    xTaskCreatePinnedToCore(logic_task,  "logic",  8192, nullptr, 4, &h_logic,  1);
    xTaskCreatePinnedToCore(mppt_task,   "mppt",   4096, nullptr, 3, &h_mppt,   1);
    xTaskCreatePinnedToCore(logger_task, "logger", 6144, nullptr, 2, &h_logger, 0);  // K-4: SD-Aufrufkette tief
    xTaskCreatePinnedToCore(ws_task,     "ws",     4096, nullptr, 2, &h_ws,     0);
    xTaskCreatePinnedToCore(level_task,  "level",  4096, nullptr, 1, &h_level,  0);

    esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
    esp_task_wdt_add(nullptr);
    watchdog_init();

    io_rgb_task_start();  // LED-Render-Task (übernimmt RGB ab jetzt)

    io_blink_status_led(3, 100, 100);
    Serial.println("[MAIN] Alle Tasks gestartet");
    Serial.printf("[MAIN] Heap: %u B  PSRAM: %u B\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop() {
    esp_task_wdt_reset();
    uint32_t freeHeap = ESP.getFreeHeap();
    // Nicht-blockierend: Flag setzen, der LED-Task zeigt Magenta-Burst.
    // (Früher blockierte io_blink_status_led() hier bis 450ms in loop().)
    g_rgbHeapAlert = (freeHeap < 20480);
    if (g_rgbHeapAlert)
        Serial.printf("[MAIN] WARNUNG: Heap niedrig: %u B\n", freeHeap);
    vTaskDelay(pdMS_TO_TICKS(1000));
}
