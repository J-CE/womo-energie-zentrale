// ============================================================
//  logger.h — Womo Energy Core v5.4
//  PSRAM-Ringpuffer (86400 × 32B = 48h @ 2s) + SD-CSV-Logging
//
//  LogEntry:  __attribute__((packed)), exakt 32 Bytes
//             static_assert(sizeof(LogEntry) == 32) aktiv
//  Ringpuffer:Alle 2s (logger_append), SD-Flush alle 5s
//  SD-Dateien:/log_NNNNN.csv (NNNNN = Tage seit 1970 in MEZ)
//  CSV-Header: ts,v,i,soc,t_mos,t_s1,t_s2,t_mos2,...
//  g_sdMutex: schützt alle SD-Zugriffe (logger + http_server)
// ============================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// ── Log-Eintrag (32 Byte, aligned) ───────────────────────────
struct __attribute__((packed)) LogEntry {
    uint32_t timestamp;        // Unix-Epoch (gesyncte / fortgesetzte Zeit)
    uint16_t bmsVoltage;       // V * 100  (0.01V Aufl.)
    int16_t  bmsCurrent;       // A * 10   (0.1A  Aufl., + = Laden)
    uint8_t  soc;              // %
    int8_t   tempTube;         // degC 0x80 MOS/Power-Tube (Feld-Name historisch)
    int8_t   tempSensor1;      // degC
    int8_t   tempSensor2;      // degC
    int8_t   tempMOS;          // degC
    uint16_t cellMinMV;        // mV niedrigste Zelle
    uint16_t cellMaxMV;        // mV höchste Zelle
    uint16_t remainAh10;       // Ah * 10  (0.1Ah Aufl.)
    uint16_t pvPower;          // W
    uint16_t pvVoltage10;      // V * 10   (0.1V Aufl.)
    uint16_t pvCurrent100;     // A * 100  (0.01A Aufl.)
    uint8_t  mpptState;        // CS
    uint8_t  mpptError;        // ERR
    uint8_t  flags;            // bit0=Landstrom bit1=D+ bit2=Gel bit3=WR
    uint8_t  reserved[4];      // Padding → 32 Byte gesamt (4+2+2+1+1+1+1+1+2+2+2+2+2+2+1+1+1+4=32)
};
static_assert(sizeof(LogEntry) == 32, "LogEntry muss 32 Byte sein");

// ── Globaler SD-Mutex ─────────────────────────────────────────
// Muss vor JEDEM SD.open/close/read/write genommen werden.
// Gilt auch für: handle_sdfiles, handle_sddata, logger_emergency_back_up.
extern SemaphoreHandle_t g_sdMutex;

// ── Ringpuffer-Status ─────────────────────────────────────────
extern uint32_t g_log_count;
extern uint32_t g_log_head;

// ── Funktionen ────────────────────────────────────────────────
void     logger_init();
void     logger_append();           // 2s-Rohwert → Ringpuffer + Minuten-Akku
void     logger_flush_sd();         // Minuten-Batch auf SD, wenn fällig
bool     logger_sd_available();

// Notfallsicherung (Watchdog vor Reboot).
void     logger_emergency_back_up(const char* reason);

// Snapshot-API für http_server (Streaming-JSON).
// Kopiert bis zu `count` Einträge (neueste zuerst) mit Dezimierung
// `step` in den vom Aufrufer bereitgestellten PSRAM-Puffer `out`.
// Gibt Anzahl tatsächlich kopierter Einträge zurück.
uint32_t logger_snapshot(uint32_t offset, uint32_t count,
                         uint32_t step, LogEntry* out);

// CSV-Serialisierung (SD-Schreiben)
String   logger_entry_to_csv(const LogEntry& e);
