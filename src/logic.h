// ============================================================
//  logic.h — Womo Energy Core v5.4
//  Schaltlogik: D+-Relais, Gel-MOSFET, WR-Remote
//
//  Ausgewertet alle 2s in logic_task (Core 1):
//    D+-Relais:   SoC>=On UND PV(MA)>=On UND kein Landstrom
//                 AUS hart: SoC<Off, BMS/MPPT ungültig, Landstrom
//                 AUS weich: PV<Off UND SoC<High (min. 5min Laufzeit)
//    Gel-MOSFET:  SoC>=On UND PV(MA)>=GelMin
//                 AUS: wie D+, ohne min. Laufzeit
//    WR-Remote:   SoC>=On UND BMS ok (v5.3: kein Landstrom-Check mehr —
//                 Renogy NVS übernimmt AC-Umschaltung selbst)
//                 Kein PV-Check
//  PV-MA: 15-Sample Moving Average (30s), MPPT-Debounce: 5 Frames
//
//  v5.4: Manueller Aktor-Override aus dem Webinterface.
//    Im Manual-Modus übersteuert der Nutzerbefehl JEDE Automatik-
//    Bedingung inkl. Hart-Interlocks (BMS/Landstrom) — bewusste
//    Design-Entscheidung, volle manuelle Kontrolle. Als Sicherheits-
//    netz läuft pro Aktor ein Deadman-Timeout (NVS-Parameter
//    manualTimeoutMin, Default 30min), der bei jedem manuellen
//    Befehl neu startet und den Aktor beim Verstreichen automatisch
//    zurück in den Automatikmodus fallen lässt. Kein NVS für den
//    Manual-Zustand selbst — nach Reboot immer Auto (Fail-Safe).
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

// ── Manueller Aktor-Override (Webinterface, v5.4) ─────────────
enum ManualActuator : uint8_t { MANUAL_DPLUS = 0, MANUAL_GEL = 1, MANUAL_WR = 2 };

// active=false → sofort zurück in Automatik.
// active=true  → want ist der erzwungene Soll-Zustand, Deadman-
//                Timer wird (neu) gestartet. Keine Interlock-Prüfung.
// Rückgabe false nur bei ungültigem Aktor-Index.
bool logic_set_manual(ManualActuator a, bool active, bool want);
