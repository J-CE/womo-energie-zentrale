// ============================================================
//  params.cpp — Womo Energy Core v5.0
// ============================================================
#include "params.h"
#include <Preferences.h>

Params g_params;
static Preferences prefs;
#define NVS_NS "womo_cfg"

static void _write_defaults() {
    prefs.putUChar ("socDPlusOn",    DEFAULT_SOC_D_PLUS_ON);
    prefs.putUChar ("socDPlusOff",   DEFAULT_SOC_D_PLUS_OFF);
    prefs.putUChar ("socDPlusHigh",  DEFAULT_SOC_D_PLUS_HIGH);
    prefs.putUShort("pvThreshOn",    DEFAULT_PV_THRESHOLD_ON);
    prefs.putUShort("pvThreshOff",   DEFAULT_PV_THRESHOLD_OFF);
    prefs.putUChar ("socGelOn",      DEFAULT_SOC_GEL_ON);
    prefs.putUChar ("socGelHigh",    DEFAULT_SOC_GEL_HIGH);
    prefs.putUShort("pvGelMinW",     DEFAULT_PV_GEL_MIN_W);
    prefs.putUChar ("socWROn",       DEFAULT_SOC_WR_ON);
    prefs.putUChar ("socWROff",      DEFAULT_SOC_WR_OFF);
    prefs.putUChar ("debounceCyc",   DEFAULT_RELAY_DEBOUNCE_CYCLES);
    prefs.putULong ("logIntervalMs", DEFAULT_LOG_INTERVAL_MS);
}

static void _load_from_nvs() {
    g_params.socDPlusOn          = prefs.getUChar ("socDPlusOn",    DEFAULT_SOC_D_PLUS_ON);
    g_params.socDPlusOff         = prefs.getUChar ("socDPlusOff",   DEFAULT_SOC_D_PLUS_OFF);
    g_params.socDPlusHigh        = prefs.getUChar ("socDPlusHigh",  DEFAULT_SOC_D_PLUS_HIGH);
    g_params.pvThresholdOn       = prefs.getUShort("pvThreshOn",    DEFAULT_PV_THRESHOLD_ON);
    g_params.pvThresholdOff      = prefs.getUShort("pvThreshOff",   DEFAULT_PV_THRESHOLD_OFF);
    g_params.socGelOn            = prefs.getUChar ("socGelOn",      DEFAULT_SOC_GEL_ON);
    g_params.socGelHigh          = prefs.getUChar ("socGelHigh",    DEFAULT_SOC_GEL_HIGH);
    g_params.pvGelMinW           = prefs.getUShort("pvGelMinW",     DEFAULT_PV_GEL_MIN_W);
    g_params.socWROn             = prefs.getUChar ("socWROn",       DEFAULT_SOC_WR_ON);
    g_params.socWROff            = prefs.getUChar ("socWROff",      DEFAULT_SOC_WR_OFF);
    g_params.relayDebounceCycles = prefs.getUChar ("debounceCyc",   DEFAULT_RELAY_DEBOUNCE_CYCLES);
    g_params.logIntervalMs       = prefs.getULong ("logIntervalMs", DEFAULT_LOG_INTERVAL_MS);
}

void params_init() {
    prefs.begin(NVS_NS, false);
    if (!prefs.isKey("socDPlusOn")) {
        Serial.println("[PARAMS] Erster Start — schreibe Defaults");
        _write_defaults();
    }
    // v5.0: neue Schlüssel anlegen falls NVS aus v4.x kommt
    if (!prefs.isKey("socDPlusHigh")) prefs.putUChar("socDPlusHigh", DEFAULT_SOC_D_PLUS_HIGH);
    if (!prefs.isKey("socGelHigh"))   prefs.putUChar("socGelHigh",   DEFAULT_SOC_GEL_HIGH);

    _load_from_nvs();
    Serial.printf("[PARAMS] D+: SOC %u/%u/High%u PV %u/%u\n",
        g_params.socDPlusOn, g_params.socDPlusOff, g_params.socDPlusHigh,
        g_params.pvThresholdOn, g_params.pvThresholdOff);
    Serial.printf("[PARAMS] Gel: SOC %u/High%u PV>=%u | WR: SOC %u/%u\n",
        g_params.socGelOn, g_params.socGelHigh, g_params.pvGelMinW,
        g_params.socWROn, g_params.socWROff);
}

void params_reset() {
    Serial.println("[PARAMS] Fabrik-Reset");
    _write_defaults();
    _load_from_nvs();
}

// Setter — nur Wertebereichsgrenzen, keine ON>OFF-Pflicht (v5.0)
bool params_set_soc_dplus_on(uint8_t v) {
    if (v < 50 || v > 100) return false;
    g_params.socDPlusOn = v; prefs.putUChar("socDPlusOn", v); return true;
}
bool params_set_soc_dplus_off(uint8_t v) {
    if (v < 20 || v > 99) return false;
    g_params.socDPlusOff = v; prefs.putUChar("socDPlusOff", v); return true;
}
bool params_set_soc_dplus_high(uint8_t v) {
    if (v < 50 || v > 100) return false;
    g_params.socDPlusHigh = v; prefs.putUChar("socDPlusHigh", v); return true;
}
bool params_set_pv_threshold_on(uint16_t v) {
    if (v < 10 || v > 2000) return false;
    g_params.pvThresholdOn = v; prefs.putUShort("pvThreshOn", v); return true;
}
bool params_set_pv_threshold_off(uint16_t v) {
    if (v > 2000) return false;
    g_params.pvThresholdOff = v; prefs.putUShort("pvThreshOff", v); return true;
}
bool params_set_soc_gel_on(uint8_t v) {
    if (v < 50 || v > 100) return false;
    g_params.socGelOn = v; prefs.putUChar("socGelOn", v); return true;
}
bool params_set_soc_gel_high(uint8_t v) {
    if (v < 50 || v > 100) return false;
    g_params.socGelHigh = v; prefs.putUChar("socGelHigh", v); return true;
}
bool params_set_pv_gel_min_w(uint16_t v) {
    if (v > 500) return false;
    g_params.pvGelMinW = v; prefs.putUShort("pvGelMinW", v); return true;
}
bool params_set_soc_wr_on(uint8_t v) {
    if (v < 50 || v > 100) return false;
    g_params.socWROn = v; prefs.putUChar("socWROn", v); return true;
}
bool params_set_soc_wr_off(uint8_t v) {
    if (v < 20 || v > 99) return false;
    g_params.socWROff = v; prefs.putUChar("socWROff", v); return true;
}
bool params_set_relay_debounce_cycles(uint8_t v) {
    if (v < 1 || v > 60) return false;
    g_params.relayDebounceCycles = v; prefs.putUChar("debounceCyc", v); return true;
}
bool params_set_log_interval_ms(uint32_t v) {
    if (v < 60000 || v > 3600000) return false;
    g_params.logIntervalMs = v; prefs.putULong("logIntervalMs", v); return true;
}

String params_to_json() {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"socDPlusOn\":%u,\"socDPlusOff\":%u,\"socDPlusHigh\":%u,"
        "\"pvThresholdOn\":%u,\"pvThresholdOff\":%u,"
        "\"socGelOn\":%u,\"socGelHigh\":%u,\"pvGelMinW\":%u,"
        "\"socWROn\":%u,\"socWROff\":%u,"
        "\"relayDebounceCycles\":%u,\"logIntervalMs\":%lu}",
        g_params.socDPlusOn, g_params.socDPlusOff, g_params.socDPlusHigh,
        g_params.pvThresholdOn, g_params.pvThresholdOff,
        g_params.socGelOn, g_params.socGelHigh, g_params.pvGelMinW,
        g_params.socWROn, g_params.socWROff,
        g_params.relayDebounceCycles,
        (unsigned long)g_params.logIntervalMs);
    return String(buf);
}
