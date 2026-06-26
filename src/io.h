// ============================================================
//  io.h — Womo Energy Core v5.1
//  GPIO-Abstraktion: Relais, MOSFETs, LEDs, Landstrom-Sensor
//
//  Ausgänge (alle Fail-Safe LOW bei Reset):
//    GPIO_RELAY_D_PLUS   21  Active-LOW   D+-Relais Kühlschrank
//    GPIO_MOSFET_GEL     39  Active-HIGH  Gel-Lader MOSFET
//    GPIO_OPTO_WR_REMOTE 40  Active-HIGH  WR-Remote Optokoppler
//    GPIO_STATUS_LED     41  Active-HIGH  Status-LED
//  Eingänge:
//    GPIO_LANDSTROM_SENSOR 18 INPUT (Teiler 2k/1,5k von 5V) HIGH=Landstrom
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

// ── On-board RGB-LED — Rundlauf-Encoding ─────────────────────
// v5.1: Statt einem einzelnen Prioritätszustand zeigt die LED
// JEDEN Sensor/Aktor als eigenen farbigen "Blip" in einem festen,
// zyklisch wiederholten Schema (7 Slots fester Länge). Inaktive
// Kanäle bleiben in ihrem Slot dunkel — Slot-POSITION = Kanal-
// Identität, unabhängig davon wie viele andere Kanäle aktiv sind.
// Alle Kanäle sind gleichberechtigt (auch BMS-/MPPT-Fehler).
//
// Slot-Reihenfolge (fix, sieh led_task() in io.cpp):
//   0 BMS-Fehler   1 MPPT-Fehler   2 SoC-Gauge (immer sichtbar)
//   3 Landstrom    4 D+ Kühlschrank   5 Gel-Lader   6 WR-Remote
//
// g_rgbChannelMask: ein 32-Bit-Wort, atomar in einem Schreibvorgang
// von logic_evaluate() gesetzt (Bits = Flags, Bits 8–15 = SoC%).
// Vermeidet Races durch Single-Word-Read/Write (wie zuvor bei
// g_rgbState), ohne dass ein Mutex für 7 Einzelwerte nötig wäre.
#define RGB_BIT_BMS_FAULT    (1u << 0)
#define RGB_BIT_MPPT_FAULT   (1u << 1)
#define RGB_BIT_LANDSTROM    (1u << 2)
#define RGB_BIT_DPLUS        (1u << 3)
#define RGB_BIT_GEL          (1u << 4)
#define RGB_BIT_WR           (1u << 5)
#define RGB_SOC_SHIFT        8
#define RGB_SOC_MASK         (0xFFu << RGB_SOC_SHIFT)

enum RgbBootState : uint8_t {
    RGB_BOOT = 0,   // weiß — Startup, bis logic_evaluate() erstmals läuft
    RGB_RUNNING     // Rundlauf aktiv (gesteuert über g_rgbChannelMask)
};

// Von logic_evaluate() gesetzt, vom LED-Task gelesen (Cross-Core).
extern volatile RgbBootState g_rgbState;
extern volatile uint32_t     g_rgbChannelMask;
// Von loop() bei Heap-Mangel gesetzt — hat im LED-Task Vorrang (Magenta).
// Bewusste Ausnahme vom Rundlauf: kein Betriebszustand, sondern ein
// Selbstschutz-Signal des Systems (drohender Heap-Crash) — muss
// sofort erkennbar sein, ohne auf einen Zyklus-Slot zu warten.
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

// Alle Aktoren in sicheren Aus-Zustand (Fail-Safe)
void io_all_safe();

// Aktuellen IO-Zustand als JSON
String io_to_json();

