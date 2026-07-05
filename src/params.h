// ============================================================
//  params.h — Womo Energy Core v5.6.0
//  10 NVS-Parameter (Namespace "womo_cfg")
//
//  v5.6.0: params_apply_json() — gemeinsamer Anwendungs-/
//   Validierungspfad für POST /api/params (HTTP) und
//   {"cmd":"params_set"} (BLE). Semantik unverändert:
//   nur vorhandene Schlüssel werden gesetzt, jeder über die
//   bestehenden Setter (Bereichsprüfung), Rückgabe false sobald
//   EIN Wert außerhalb der Grenzen liegt.
//
//  v5.5 Änderungen (Kriterien-Redesign, s. logic.cpp/L-SW03..05):
//   - socDPlusHigh   (kein weiches PV-AUS mehr)
//   - pvThresholdOff (dito)
//   - socGelHigh     (dito)
//   - socWROn        (WR hat kein Auto-EIN mehr — nur manuell)
//   + socGelOff      (eigene harte AUS-Schwelle, war = socGelOn)
//   ~ pvThresholdOn → pvDPlusMinW (Mindest-PV ODER Float, Default 100W)
//   ~ pvGelMinW Default: 60→20W | socGelOn Default: 82→95%
//
//  v5.0 Änderungen:
//   ~ Keine Cross-Validierung ON>OFF mehr
// ============================================================
#pragma once
#include <Arduino.h>
#include "config.h"

struct Params {
    // ── D+-Relais Kühlschrank (3 Parameter) ───────────────
    uint8_t  socDPlusOn;        // % EIN-Schwelle
    uint8_t  socDPlusOff;       // % harte AUS-Schwelle
    uint16_t pvDPlusMinW;       // W Mindest-PV für EIN (ODER MPPT-Float)

    // ── Gel-Lader Starterbatterie (3 Parameter) ───────────
    uint8_t  socGelOn;          // % EIN-Schwelle
    uint8_t  socGelOff;         // % harte AUS-Schwelle (neu v5.5)
    uint16_t pvGelMinW;         // W Mindest-PV für EIN (ODER MPPT-Float)

    // ── Wechselrichter Remote (1 Parameter, nur Auto-AUS) ─
    uint8_t  socWROff;          // % AUS-Schwelle (EIN nur manuell)

    // ── System (2 Parameter) ──────────────────────────────
    uint8_t  relayDebounceCycles;
    uint32_t logIntervalMs;

    // ── Manueller Aktor-Override (1 Parameter) ────────────
    uint8_t  manualTimeoutMin;  // min Deadman — nur Manuell-EIN D+/Gel
};

extern Params g_params;

void params_init();
void params_reset();

bool params_set_soc_dplus_on         (uint8_t  v);
bool params_set_soc_dplus_off        (uint8_t  v);
bool params_set_pv_dplus_min_w       (uint16_t v);
bool params_set_soc_gel_on           (uint8_t  v);
bool params_set_soc_gel_off          (uint8_t  v);
bool params_set_pv_gel_min_w         (uint16_t v);
bool params_set_soc_wr_off           (uint8_t  v);
bool params_set_relay_debounce_cycles(uint8_t  v);
bool params_set_log_interval_ms      (uint32_t v);
bool params_set_manual_timeout_min   (uint8_t  v);

String params_to_json();

// JSON-Objekt (nur bekannte Schlüssel) anwenden — s. Kopfkommentar.
// Rückgabe: false bei ungültigem JSON oder Wert außerhalb Grenzen.
bool params_apply_json(const char* json);
