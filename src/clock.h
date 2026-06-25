// ============================================================
//  clock.h — Womo Energy Core v5.0
//  Zeitzone: MEZ (UTC+1), keine Sommerzeit.
//  Zeitbasis: DS3231 RTC (führend) + esp_timer_get_time() (Laufzeit).
//             Fallback NVS/Build, wenn DS3231 fehlt/OSF gesetzt.
//             24h-Resync gegen DS3231 bei lokalem Tageswechsel.
//
//  clock_now()         → UTC-Epoch (Speicherformat)
//  clock_now_local()   → MEZ-Epoch (UTC+1, für Dateinamen + Anzeige)
//  clock_set_epoch()   → bool; kein Monotonizitätsschutz (v5.0);
//                        stellt zusätzlich die DS3231-Hardware-Uhr
//  clock_is_synced()   → true nach gültiger RTC- oder Browser-Quelle
//  clock_rtc_present() → true wenn DS3231 aktuell auf I2C antwortet
//  clock_rtc_json()    → RTC-Health als JSON {present,valid,drift,temp}
// ============================================================
#pragma once
#include <Arduino.h>

void     clock_init();
bool     clock_set_epoch(uint32_t epoch);   // false = unplausibel (< 2024)
bool     clock_is_synced();
bool     clock_rtc_present();                // DS3231 antwortet aktuell
String   clock_rtc_json();                   // {present,valid,drift,temp}
uint32_t clock_now();                        // UTC-Epoch
uint32_t clock_now_local();                  // MEZ-Epoch (UTC+1)
void     clock_persist(bool force = false);
