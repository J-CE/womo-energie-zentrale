// ============================================================
//  logic.h — Womo Energy Core v5.5
//  Schaltlogik: D+-MOSFET, Gel-MOSFET, WR-Remote
//
//  Ausgewertet alle 2s in logic_task (Core 1) — Kriterien v5.5:
//    D+:   EIN  SoC>=On UND (PV(MA)>=DPlusMin ODER MPPT-Float)
//               UND BMS ok UND MPPT ok UND kein Landstrom
//          AUS  (alle hart): Landstrom | BMS ungültig | SoC<Off
//    Gel:  EIN  SoC>=GelOn UND (PV(MA)>=GelMin ODER MPPT-Float)
//               UND BMS ok UND MPPT ok UND kein Landstrom
//          AUS  (alle hart): Landstrom | BMS ungültig | SoC<GelOff
//    WR:   KEIN Auto-EIN — Einschalten nur manuell.
//          AUS  hart: BMS ungültig | weich: SoC<WROff
//          Die AUS-Bedingungen gelten AUCH während Manuell-EIN
//          (Sicherheitsnetz, da der WR-Deadman-Timer entfällt).
//  MPPT-Ausfall ist KEINE AUS-Bedingung mehr (v5.5) — ungültige
//  MPPT-Daten blockieren nur die EIN-Seite (PV/Float nicht bewertbar).
//  PV-MA: 15-Sample Moving Average (30s), MPPT-Debounce: 5 Frames.
//
//  Manueller Aktor-Override (v5.4, Semantik geändert v5.5):
//    Manuell EIN (D+/Gel): übersteuert JEDE Automatik-Bedingung inkl.
//      Hart-Interlocks (bewusste Design-Entscheidung, unverändert).
//      Deadman-Timeout (manualTimeoutMin) läuft — bei Ablauf zurück
//      in Automatik. Nie persistent (Reboot = Auto).
//    Manuell EIN (WR): OHNE Deadman-Timer — läuft bis Nutzerbefehl
//      oder bis eine Auto-AUS-Bedingung greift (beendet den Manual-
//      Modus). Nie persistent.
//    Manuell AUS (alle): DAUERHAFT — kein Timer, kein Rückfall auf
//      Automatik, NVS-persistent (überlebt Reboot/Watchdog-Reset).
//      AUS ist die sichere Richtung, daher unbedenklich persistent.
// ============================================================
#pragma once
#include <Arduino.h>

// Einmalig in setup() aufrufen (lädt persistente Manuell-AUS-Zustände)
void logic_init();

// In Haupt-Task aufrufen (alle 2s)
// Liest g_bms, g_mppt, g_io und steuert Aktoren
void logic_evaluate();

// Diagnose-String für Webinterface
String logic_status_json();

// ── Manueller Aktor-Override (Webinterface) ───────────────────
enum ManualActuator : uint8_t { MANUAL_DPLUS = 0, MANUAL_GEL = 1, MANUAL_WR = 2 };

// active=false → sofort zurück in Automatik (löscht auch persist. AUS).
// active=true, want=true  → Manuell EIN (D+/Gel: Deadman startet neu;
//                           WR: ohne Timer, Auto-AUS bleibt wirksam).
// active=true, want=false → Manuell AUS, dauerhaft + NVS-persistent.
// Rückgabe false nur bei ungültigem Aktor-Index.
bool logic_set_manual(ManualActuator a, bool active, bool want);
