// ============================================================
//  watchdog.h — Womo Energy Core v5.4
//  Zweistufiger Watchdog-Mechanismus
//
//  Stufe 1: HW-WDT (esp_task_wdt, 4s) — überwacht loop()
//           Timeout → harter Reset
//  Stufe 2: SW-WDT (watchdog.cpp) — überwacht BMS/MPPT/Logger
//           Timeout → Notfallsicherung (PSRAM→SD) + Reset
//  Kick:    watchdog_kick(WDT_BMS|WDT_MPPT|WDT_LOGGER) aus Tasks
//  lastKick[]: volatile (Cross-Core-Sichtbarkeit)
// ============================================================
#pragma once
#include <Arduino.h>

// Vom Software-Watchdog überwachte Peripherie-Module.
// loop() ist hier bewusst NICHT enthalten (HW-WDT).
enum WatchdogModule {
    WDT_BMS = 0,
    WDT_MPPT,
    WDT_LOGGER,
    WDT_NUM_MODULES   // Automatische Anzahl
};

// Initialisiert die Modul-Überwachung und startet die Task.
// Liest RTC-Memory aus, um Reboots über Neustarts hinweg zu
// verfolgen (Anti-Bootloop).
void watchdog_init();

// Setzt den Timer eines Moduls zurück ("Füttern").
void watchdog_kick(WatchdogModule module);
