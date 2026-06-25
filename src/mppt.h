// ============================================================
//  mppt.h — Womo Energy Core v5.0
//  Victron VE.Direct Text-Parser + HEX-TX Temperatur
//
//  v5.0: UART2 bidirektional (RX=38, TX=42)
//  Empfangene Felder reduziert; HEX-TX für BMS-Temperatur neu.
// ============================================================
#pragma once
#include <Arduino.h>

#define MPPT_CS_OFF             0
#define MPPT_CS_FAULT           2
#define MPPT_CS_BULK            3
#define MPPT_CS_ABSORPTION      4
#define MPPT_CS_FLOAT           5
#define MPPT_CS_STORAGE         6
#define MPPT_CS_EQUALIZE        7
#define MPPT_CS_EXT_CONTROL    252

struct MPPTData {
    bool        valid;
    uint32_t    lastUpdateMs;
    uint32_t    frameCount;
    bool        timeout;

    float       batteryVoltage;     // V  (Textfeld V, Einheit mV → /1000)
    float       chargeCurrent;      // A  (Textfeld I, Einheit mA → /1000)
    float       panelVoltage;       // V  (Textfeld VPV, Einheit mV → /1000)
    uint16_t    panelPower;         // W  (Textfeld PPV)
    uint8_t     chargeState;        // CS
    uint8_t     errorCode;          // ERR
    float       yieldToday;         // kWh (Textfeld H20, Einheit 0.01kWh → ×0.01)
    uint16_t    maxPowerToday;      // W  (Textfeld H21)
};

extern MPPTData          g_mppt;
extern SemaphoreHandle_t g_mpptMutex;

void   mppt_init();
bool   mppt_poll();
void   mppt_send_temperature(float tempC); // VE.Direct HEX TX — Register 0x2003, °C
void   mppt_send_temp_na();                // N/A senden (BMS veraltet) → 0x7FFF
String mppt_to_json();
String mppt_cs_text(uint8_t cs);
String mppt_error_text(uint8_t err);
