// ============================================================
//  level.h — Womo Energy Core (optionales Modul)
//  Elektronische Wasserwaage / Lagesensor
//
//  Sensor:  MMA8452Q (3-Achs-Beschleunigungssensor) am GEMEINSAMEN
//           I2C-Bus (SDA=GPIO1, SCL=GPIO2 — geteilt mit DS3231).
//           Adresse 0x1D (SA0=1, Default) oder 0x1C (SA0=0).
//           Versorgung 3,3V direkt, KEIN Pegelwandler nötig.
//           Kein zusätzlicher GPIO — nur SDA/SCL/3V3/GND.
//
//  Prinzip: Im Stillstand misst der Accelerometer ausschließlich die
//           Schwerkraft (1g). Aus deren Verteilung auf die Achsen folgt
//           die Neigung (roll/pitch). Daraus + Spurweite/Radstand wird
//           pro Rad die nötige Keilhöhe berechnet.
//
//  Keil-Regel: Das HÖCHSTE Rad ist Referenz (Keil 0). Die anderen drei
//           werden auf dessen Höhe angehoben → das tiefste Rad bekommt
//           den größten Keil:  keil_i = max(z) − z_i.
//
//  Bus-Koordination: Alle I2C-Transaktionen laufen unter g_i2cMutex
//           (deklariert in clock.h), da DS3231 und Lagesensor denselben
//           Bus teilen und aus verschiedenen Tasks/Cores zugegriffen wird.
//
//  Threading: level_task (Core 0, ~4Hz) liest den Sensor und legt das
//           Ergebnis in einem per-portMUX geschützten Cache ab. HTTP-
//           Handler und WebSocket-Broadcast lesen nur den Cache (kein I2C).
// ============================================================
#pragma once
#include <Arduino.h>

// Radindex-Konvention: 0=VL(FL) 1=VR(FR) 2=HL(RL) 3=HR(RR)
struct LevelState {
    bool     present;        // Sensor am Bus erkannt
    bool     valid;          // letzte Messung gültig
    bool     enabled;        // Modul aktiv (NVS)
    bool     levelOk;        // |roll| und |pitch| < Toleranz → eben
    float    ax, ay, az;     // g, geglättet (Diagnose/Kalibrierung)
    float    roll, pitch;    // Grad, kalibriert (Null-Offset abgezogen)
    float    rawRoll, rawPitch; // Grad, VOR Null-Offset (für Kalibrierung)
    float    wedge[4];       // mm Keilhöhe je Rad [VL,VR,HL,HR]
    uint8_t  refWheel;       // Index des Referenzrads (höchstes, Keil 0)
    uint32_t lastMs;         // millis() der letzten gültigen Messung
};

void   level_init();              // Sensor erkennen + konfigurieren (in setup)
void   level_task(void*);         // Poll-Task (Core 0)
void   level_get(LevelState& out);// thread-sicherer Schnappschuss des Cache

String level_to_json();           // aktueller Lage-Zustand
String level_cfg_to_json();       // aktuelle Konfiguration

// Setter (Wertebereichsprüfung, persistieren in NVS-Namespace "level")
bool   level_set_track    (uint16_t mm);    // Spurweite 800..3000
bool   level_set_wheelbase(uint16_t mm);    // Radstand  1500..8000
bool   level_set_rot      (uint16_t deg);   // Einbaudrehung 0/90/180/270
bool   level_set_invert   (bool invRoll, bool invPitch);
bool   level_set_enabled  (bool en);

// Kalibrierung: reset=false → aktuelle Lage als "eben" speichern;
//               reset=true  → Null-Offset löschen.
bool   level_calibrate(bool reset);
