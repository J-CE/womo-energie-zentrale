// ============================================================
//  clock.h — Womo Energy Core v5.1
//  Zeitbasis: DS3231 RTC (führend) + esp_timer_get_time() (Laufzeit).
//             Fallback NVS/Build, wenn DS3231 fehlt/OSF gesetzt.
//             24h-Resync gegen DS3231 bei UTC-Tageswechsel.
//
//  Zeitzone: vollwertiger POSIX-TZ-String (newlib localtime_r/tzset).
//            Sommer-/Winterzeit (DST) wird automatisch behandelt.
//            Default: Europe/Berlin. Zur Laufzeit über /api/tz änderbar,
//            persistiert in NVS-Namespace "clock"/"tz".
//            Speicher/Storage immer UTC — der Offset gilt nur für Anzeige
//            und die lokale CSV-Spalte.
//
//  clock_now()              → UTC-Epoch (Speicherformat)
//  clock_now_local()        → lokale Epoch (UTC + DST-Offset, nur Anzeige)
//  clock_local_offset_at(u) → lokaler Offset (s) für UTC-Zeitpunkt u, DST-korrekt
//  clock_set_tz(tz)         → POSIX-TZ setzen + persistieren
//  clock_tz()               → aktueller POSIX-TZ-String
//  clock_tz_abbr()          → aktuelle Abkürzung (z. B. "CET"/"CEST")
//  clock_set_epoch()        → bool; stellt zusätzlich die DS3231-Hardware-Uhr
//  clock_is_synced()        → true nach gültiger RTC- oder Browser-Quelle
//  clock_rtc_present()      → true wenn DS3231 aktuell auf I2C antwortet
//  clock_rtc_json()         → RTC-Health als JSON {present,valid,drift,temp}
// ============================================================
#pragma once
#include <Arduino.h>

void     clock_init();
bool     clock_set_epoch(uint32_t epoch);   // false = unplausibel (< 2024)
bool     clock_is_synced();
bool     clock_rtc_present();                // DS3231 antwortet aktuell
String   clock_rtc_json();                   // {present,valid,drift,temp}
uint32_t clock_now();                        // UTC-Epoch
uint32_t clock_now_local();                  // lokale Epoch (UTC + DST-Offset)
int32_t  clock_local_offset_at(uint32_t utc);// lokaler Offset (s), DST-korrekt
void     clock_set_tz(const char* tz);       // POSIX-TZ setzen + persistieren
String   clock_tz();                         // aktueller POSIX-TZ-String
String   clock_tz_abbr();                    // Abkürzung jetzt (z. B. "CEST")
void     clock_persist(bool force = false);
