// ============================================================
//  logic.h — Womo Energy Core v5.0
//  Schaltlogik: D+-Relais, Gel-MOSFET, WR-Remote
//
//  Ausgewertet alle 2s in logic_task (Core 1):
//    D+-Relais:   SoC>=On UND PV(MA)>=On UND kein Landstrom
//                 AUS hart: SoC<Off, BMS/MPPT ungültig, Landstrom
//                 AUS weich: PV<Off UND SoC<High (min. 5min Laufzeit)
//    Gel-MOSFET:  SoC>=On UND PV(MA)>=GelMin
//                 AUS: wie D+, ohne min. Laufzeit
//    WR-Remote:   SoC>=On UND kein Landstrom UND BMS ok
//                 Kein PV-Check
//  PV-MA: 15-Sample Moving Average (30s), MPPT-Debounce: 5 Frames
// ============================================================
#pragma once
#include <Arduino.h>

// Einmalig in setup() aufrufen
void logic_init();

// In Haupt-Task aufrufen (alle 2s)
// Liest g_bms, g_mppt, g_io und steuert Aktoren
void logic_evaluate();

// Diagnose-String für Webinterface
String logic_status_json();
