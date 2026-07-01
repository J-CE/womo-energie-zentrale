// ============================================================
//  inverter.h — Womo Energy Core v5.4
//  Wechselrichter-Status — Renogy 12V/2000W mit NVS
//
//  Hardware: Renogy Pure Sine Wave Inverter mit Netzvorrangschaltung
//  (Transfer-Switch-Modell), RJ11/RJ12-Fernbedienungsport.
//
//  Kein Datenprotokoll vorhanden (anders als BMS/MPPT) — der Port
//  liefert nur zwei rohe LED-Spannungspegel (Power/grün, Alarm/rot)
//  und einen Schalterkontakt. AC-Leistung/Spannung/Ladezustand sind
//  über dieses Interface NICHT auslesbar (vgl. Recherche zur Renogy-
//  Fernbedienung: reines Schalter+LED-Interface, kein RS485/Modbus
//  wie bei der teureren REGO/PCL-Serie).
//
//  Die EIN/AUS-Fernsteuerung läuft weiterhin getrennt über
//  GPIO_OPTO_WR_REMOTE (io.cpp/logic.cpp, SoC-basiert) — unverändert
//  zu v5.1/5.2, nur ohne Landstrom-Abhängigkeit (NVS übernimmt die
//  AC-Umschaltung selbst). Dieses Modul deckt NUR die beiden
//  Status-EINGÄNGE ab.
//
//  Cross-Core: inverter_poll() schreibt aus mppt_task (Core 1),
//  inverter_to_json() liest aus ws_task (Core 0) → g_invMutex
//  schützt den Snapshot (gleiches Muster wie g_bmsMutex/g_mpptMutex).
//
//  ACHTUNG: GPIO_WR_LED_POWER/ALARM-Pinbelegung und Spannungsteiler-
//  Dimensionierung (config.h) sind Platzhalter bis zur Multimeter-
//  Messung am realen Gerät — Pinout variiert je Modell/Charge.
//  Der Teiler MUSS den Pin im LED-AUS-Zustand definiert auf GND
//  ziehen (Pulldown-Wirkung) — der Eingang ist INPUT ohne internen
//  Pull, ein floatender Pegel würde Fehlmessungen liefern.
// ============================================================
#pragma once
#include <Arduino.h>

struct InverterData {
    bool     valid;          // false bis zum ersten Poll; danach true (GPIO-Pegel immer "frisch")
    uint32_t lastUpdateMs;   // millis() des letzten Polls
    bool     powerOn;        // Power-LED (grün) — debounced
    bool     alarm;          // Alarm-LED (rot)  — debounced
};

extern InverterData      g_inverter;
extern SemaphoreHandle_t g_invMutex;

void   inverter_init();
void   inverter_poll();      // liest beide LED-Eingänge, debounced (s. inverter.cpp)
String inverter_to_json();
