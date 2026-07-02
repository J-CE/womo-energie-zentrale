// ============================================================
//  params.cpp — Womo Energy Core v5.4
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
    prefs.putUChar ("manualTOMin",   DEFAULT_MANUAL_TIMEOUT_MIN);
}

// M-5: Bereichsgrenzen (identisch zu den Settern) — für Ladepfad-Validierung
static inline uint8_t  _clu8 (uint8_t  v, uint8_t  lo, uint8_t  hi){ return v<lo?lo:(v>hi?hi:v); }
static inline uint16_t _clu16(uint16_t v, uint16_t lo, uint16_t hi){ return v<lo?lo:(v>hi?hi:v); }
static inline uint32_t _clu32(uint32_t v, uint32_t lo, uint32_t hi){ return v<lo?lo:(v>hi?hi:v); }

static void _clamp_loaded() {
    g_params.socDPlusOn          = _clu8 (g_params.socDPlusOn,          50, 100);
    g_params.socDPlusOff         = _clu8 (g_params.socDPlusOff,         20,  99);
    g_params.socDPlusHigh        = _clu8 (g_params.socDPlusHigh,        50, 100);
    g_params.pvThresholdOn       = _clu16(g_params.pvThresholdOn,       10, 2000);
    g_params.pvThresholdOff      = _clu16(g_params.pvThresholdOff,       0, 2000);
    g_params.socGelOn            = _clu8 (g_params.socGelOn,            50, 100);
    g_params.socGelHigh          = _clu8 (g_params.socGelHigh,          50, 100);
    g_params.pvGelMinW           = _clu16(g_params.pvGelMinW,            0,  500);
    g_params.socWROn             = _clu8 (g_params.socWROn,             50, 100);
    g_params.socWROff            = _clu8 (g_params.socWROff,            20,  99);
    g_params.relayDebounceCycles = _clu8 (g_params.relayDebounceCycles,  1,   60);
    g_params.logIntervalMs       = _clu32(g_params.logIntervalMs,   60000, 3600000);
    g_params.manualTimeoutMin    = _clu8 (g_params.manualTimeoutMin,     1,  240);
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
    g_params.manualTimeoutMin    = prefs.getUChar ("manualTOMin",   DEFAULT_MANUAL_TIMEOUT_MIN);

    // M-5: NVS-Werte gegen die Setter-Grenzen clampen. Der Ladepfad übernahm
    // bisher JEDEN gespeicherten Wert ungeprüft — ein durch FW-Wechsel oder
    // Flash-Bitfehler entstandener Ausreißer (z. B. debounceCyc>127) landete
    // dann in g_params. Kritisch bei relayDebounceCycles: der (int8_t)-Cast in
    // logic.cpp::debounce() würde bei >127 negativ → Debounce wirkungslos,
    // Relais-Flattern. Clamp erzwingt gültige Bereiche identisch zu den Settern.
    _clamp_loaded();
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
    // v5.4: manueller Aktor-Override — Timeout-Parameter falls NVS aus älterer Version kommt
    if (!prefs.isKey("manualTOMin"))  prefs.putUChar("manualTOMin",  DEFAULT_MANUAL_TIMEOUT_MIN);

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
bool params_set_manual_timeout_min(uint8_t v) {
    if (v < 1 || v > 240) return false;
    g_params.manualTimeoutMin = v; prefs.putUChar("manualTOMin", v); return true;
}

String params_to_json() {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"socDPlusOn\":%u,\"socDPlusOff\":%u,\"socDPlusHigh\":%u,"
        "\"pvThresholdOn\":%u,\"pvThresholdOff\":%u,"
        "\"socGelOn\":%u,\"socGelHigh\":%u,\"pvGelMinW\":%u,"
        "\"socWROn\":%u,\"socWROff\":%u,"
        "\"relayDebounceCycles\":%u,\"logIntervalMs\":%lu,"
        "\"manualTimeoutMin\":%u}",
        g_params.socDPlusOn, g_params.socDPlusOff, g_params.socDPlusHigh,
        g_params.pvThresholdOn, g_params.pvThresholdOff,
        g_params.socGelOn, g_params.socGelHigh, g_params.pvGelMinW,
        g_params.socWROn, g_params.socWROff,
        g_params.relayDebounceCycles,
        (unsigned long)g_params.logIntervalMs,
        g_params.manualTimeoutMin);
    return String(buf);
}
