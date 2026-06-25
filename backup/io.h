// ============================================================
//  io.h — Womo Energy Core v5.0
//  GPIO-Abstraktion: Relais, MOSFETs, LEDs, Landstrom-Sensor
//
//  Ausgänge (alle Fail-Safe LOW bei Reset):
//    GPIO_RELAY_D_PLUS   21  Active-LOW   D+-Relais Kühlschrank
//    GPIO_MOSFET_GEL     39  Active-HIGH  Gel-Lader MOSFET
//    GPIO_OPTO_WR_REMOTE 40  Active-HIGH  WR-Remote Optokoppler
//    GPIO_STATUS_LED     41  Active-HIGH  Status-LED
//  Eingänge:
//    GPIO_LANDSTROM_SENSOR 18 INPUT_PULLUP HIGH=Landstrom
// ============================================================
#pragma once
#include <Arduino.h>

// ── Aktor-Zustand (zentraler Status) ─────────────────────────
struct IOState {
    bool landstrom;     // Sensor: Landstrom vorhanden
    bool relayDPlus;    // Aktor: D+ Relais Kühlschrank
    bool mosfetGel;     // Aktor: Starterbatterie-Lader
    bool wrRemote;      // Aktor: Wechselrichter Remote
    bool statusLed;     // Status-LED
};

extern IOState g_io;

// ── On-board RGB-LED ─────────────────────────────────────────
// Zustände in Prioritätsreihenfolge (hoch → niedrig). Der
// LED-Task rendert daraus Farbe + Muster (blinken/pulsen).
enum RgbState : uint8_t {
    RGB_OFF = 0,
    RGB_BOOT,         // weiß       — Startup, bis Logik erstmals läuft
    RGB_FAULT_BMS,    // rot        — schnelles Blinken (BMS ungültig/veraltet)
    RGB_FAULT_MPPT,   // orange     — Blinken (MPPT Timeout)
    RGB_SOC_LOW,      // gelb       — Dauerlicht (SoC < RGB_SOC_LOW_PCT)
    RGB_LANDSTROM,    // blau       — Dauerlicht (Netzbetrieb)
    RGB_ACTIVE,       // grün       — langsames Pulsieren (Aktor aktiv)
    RGB_IDLE_OK       // grün dunkel— Dauerlicht (alles ok, nichts aktiv)
};

// Von logic_evaluate() gesetzt, vom LED-Task gelesen (Cross-Core).
extern volatile RgbState g_rgbState;
// Von loop() bei Heap-Mangel gesetzt — hat im LED-Task Vorrang (Magenta).
extern volatile bool     g_rgbHeapAlert;

void io_init();

// Sensor lesen (nicht-blockierend)
bool io_read_landstrom();

// Aktoren setzen — aktualisieren g_io
void io_set_relay_dplus  (bool on);
void io_set_mosfet_gel   (bool on);
void io_set_wr_remote    (bool on);
void io_set_status_led   (bool on);
void io_blink_status_led (uint8_t times, uint32_t ms_on, uint32_t ms_off);

// ── RGB-LED ──────────────────────────────────────────────────
void io_rgb_init();                              // Pin/Startzustand
void io_rgb_set(uint8_t r, uint8_t g, uint8_t b);// direkt (global gedimmt)
void io_rgb_task_start();                         // startet LED-Render-Task
const char* io_rgb_state_name(RgbState s);        // für JSON/Debug

// Alle Aktoren in sicheren Aus-Zustand (Fail-Safe)
void io_all_safe();

// Aktuellen IO-Zustand als JSON
String io_to_json();

