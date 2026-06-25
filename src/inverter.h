// ============================================================
//  inverter.h — Womo Energy Core v5.0
//  Wechselrichter RJ12-Sniffer — STUB (Phase 2)
//
//  Hardware: Edecoa 12V WR, RJ12-Kommunikation
//  Protokoll: noch unbekannt (Logic-Analyzer-Analyse ausstehend)
//  Status:    inverter_poll() ist No-Op, inverter_to_json()
//             gibt immer {"valid":false} zurück
// ============================================================
#pragma once
#include <Arduino.h>

struct InverterData {
    bool     valid;
    uint32_t lastUpdateMs;
    float    acVoltage;     // V AC Ausgang
    float    acCurrent;     // A AC Ausgang
    uint16_t acPower;       // W
    float    dcVoltage;     // V DC Eingang (Batterie)
    float    dcCurrent;     // A DC Eingang
    uint8_t  loadPercent;   // % Last
    int8_t   temperature;   // °C
    uint8_t  statusCode;    // Gerätestatus
    bool     outputOn;      // Ausgang aktiv
    char     rawProtocol[8]; // "TTL5V" oder "RS232" nach Messung
};

extern InverterData g_inverter;

void   inverter_init();
bool   inverter_poll();     // Stub — gibt immer false zurück
String inverter_to_json();
