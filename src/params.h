// ============================================================
//  params.h — Womo Energy Core v5.0
//  12 NVS-Parameter (Namespace "womo_cfg")
//
//  v5.0 Änderungen:
//   + socDPlusHigh  (weiche AUS-Schwelle D+, neu)
//   + socGelHigh    (weiche AUS-Schwelle Gel, neu)
//   - socGelOff     (= socGelOn, kein eigener Parameter mehr)
//   - pvWRThresholdOn/Off (WR rein SoC-basiert)
//   ~ pvGelMinW Default: 30→60W
//   ~ Keine Cross-Validierung ON>OFF mehr
// ============================================================
#pragma once
#include <Arduino.h>
#include "config.h"

struct Params {
    // ── D+-Relais Kühlschrank (5 Parameter) ───────────────
    uint8_t  socDPlusOn;        // % EIN-Schwelle
    uint8_t  socDPlusOff;       // % harte AUS-Schwelle
    uint8_t  socDPlusHigh;      // % weiche PV-AUS nur wenn SoC < High
    uint16_t pvThresholdOn;     // W PV EIN-Schwelle
    uint16_t pvThresholdOff;    // W PV AUS-Schwelle (wirkt nur wenn SoC < High)

    // ── Gel-Lader Starterbatterie (3 Parameter) ───────────
    uint8_t  socGelOn;          // % EIN-Schwelle + harte AUS-Schwelle
    uint8_t  socGelHigh;        // % weiche PV-AUS nur wenn SoC < High
    uint16_t pvGelMinW;         // W Mindest-PV (EIN + weiche AUS)

    // ── Wechselrichter Remote (2 Parameter, nur SoC) ──────
    uint8_t  socWROn;           // % EIN-Schwelle
    uint8_t  socWROff;          // % AUS-Schwelle

    // ── System (2 Parameter) ──────────────────────────────
    uint8_t  relayDebounceCycles;
    uint32_t logIntervalMs;
};

extern Params g_params;

void params_init();
void params_reset();

bool params_set_soc_dplus_on         (uint8_t  v);
bool params_set_soc_dplus_off        (uint8_t  v);
bool params_set_soc_dplus_high       (uint8_t  v);
bool params_set_pv_threshold_on      (uint16_t v);
bool params_set_pv_threshold_off     (uint16_t v);
bool params_set_soc_gel_on           (uint8_t  v);
bool params_set_soc_gel_high         (uint8_t  v);
bool params_set_pv_gel_min_w         (uint16_t v);
bool params_set_soc_wr_on            (uint8_t  v);
bool params_set_soc_wr_off           (uint8_t  v);
bool params_set_relay_debounce_cycles(uint8_t  v);
bool params_set_log_interval_ms      (uint32_t v);

String params_to_json();
