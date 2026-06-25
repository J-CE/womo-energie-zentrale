// ============================================================
//  bms.h — Womo Energy Core v5.0
//  JK-BMS RS485 Parser
//
//  Reduzierter Datenumfang ab v5.0 (bewusst):
//  Zellspannungen, Zyklen, Balancing, Alarm-/Schutz-Bitmasks
//  und Schutzparameter werden nicht mehr gespeichert.
//
//  STROMKONVENTION (intern): + = LADEN, − = ENTLADEN
// ============================================================
#pragma once
#include <Arduino.h>

struct BMSData {
    // ── Verfügbarkeit ──────────────────────────────────────
    bool        valid;
    uint32_t    lastUpdateMs;       // millis() letztes Update (Staleness!)
    uint32_t    frameCount;
    uint32_t    errorCount;

    // ── Temperaturen (Doku: raw 0-100 = +°C, 101-140 = negativ) ────
    // 0x80 Power tube temperature  → MOS-Transistor (BMS-Board)
    // 0x81 Battery box temperature → Umgebung im Akkugehäuse
    // 0x82 Battery temperature     → Zellentemp. → wird an MPPT gesendet
    float       tempMOS;            // °C 0x80 MOS/Power-Tube
    float       tempSensor1;        // °C 0x81 Akkugehäuse
    float       tempSensor2;        // °C 0x82 Zellentemperatur (→ MPPT)

    // ── Haupt-Messwerte ────────────────────────────────────
    float       totalVoltage;       // V Gesamtspannung
    float       current;            // A (+ = LADEN, − = ENTLADEN)
    float       power;              // W = totalVoltage × current

    // ── Ladezustand ────────────────────────────────────────
    uint8_t     soc;                // % State of Charge
    float       remainingCapacityAh;// Ah — abgeleitet: nominal × SoC / 100
    float       nominalCapacityAh;  // Ah Nennkapazität (0xAA)

    // ── MOSFET-Status ──────────────────────────────────────
    bool        chargeMOSFETEnabled;
    bool        dischargeMOSFETEnabled;
};

extern BMSData           g_bms;
extern SemaphoreHandle_t g_bmsMutex;

void   bms_init();
bool   bms_poll();
String bms_to_json();
