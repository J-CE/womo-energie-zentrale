// ============================================================
//  params.cpp — Womo Energy Core v5.5
//  v5.5: Parameterbereinigung (13→10, s. params.h). Entfallene
//  NVS-Schlüssel werden beim ersten Boot aufgeräumt; neue Schlüssel
//  (pvDPlusMinW, socGelOff) mit Defaults angelegt. Bestehende Werte
//  von socGelOn/pvGelMinW bleiben erhalten — nach dem Update im
//  Dashboard prüfen (neue Empfehlung: Gel EIN 95 % / Mindest-PV 20 W)
//  oder Werks-Reset ausführen.
// ============================================================
#include "params.h"
#include <Preferences.h>

Params g_params;
static Preferences prefs;
#define NVS_NS "womo_cfg"

static void _write_defaults() {
    prefs.putUChar ("socDPlusOn",    DEFAULT_SOC_D_PLUS_ON);
    prefs.putUChar ("socDPlusOff",   DEFAULT_SOC_D_PLUS_OFF);
    prefs.putUShort("pvDPlusMinW",   DEFAULT_PV_D_PLUS_MIN_W);
    prefs.putUChar ("socGelOn",      DEFAULT_SOC_GEL_ON);
    prefs.putUChar ("socGelOff",     DEFAULT_SOC_GEL_OFF);
    prefs.putUShort("pvGelMinW",     DEFAULT_PV_GEL_MIN_W);
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
    g_params.pvDPlusMinW         = _clu16(g_params.pvDPlusMinW,         10, 2000);
    g_params.socGelOn            = _clu8 (g_params.socGelOn,            50, 100);
    g_params.socGelOff           = _clu8 (g_params.socGelOff,           20,  99);
    g_params.pvGelMinW           = _clu16(g_params.pvGelMinW,            0,  500);
    g_params.socWROff            = _clu8 (g_params.socWROff,            20,  99);
    g_params.relayDebounceCycles = _clu8 (g_params.relayDebounceCycles,  1,   60);
    g_params.logIntervalMs       = _clu32(g_params.logIntervalMs,   60000, 3600000);
    g_params.manualTimeoutMin    = _clu8 (g_params.manualTimeoutMin,     1,  240);
}

static void _load_from_nvs() {
    g_params.socDPlusOn          = prefs.getUChar ("socDPlusOn",    DEFAULT_SOC_D_PLUS_ON);
    g_params.socDPlusOff         = prefs.getUChar ("socDPlusOff",   DEFAULT_SOC_D_PLUS_OFF);
    g_params.pvDPlusMinW         = prefs.getUShort("pvDPlusMinW",   DEFAULT_PV_D_PLUS_MIN_W);
    g_params.socGelOn            = prefs.getUChar ("socGelOn",      DEFAULT_SOC_GEL_ON);
    g_params.socGelOff           = prefs.getUChar ("socGelOff",     DEFAULT_SOC_GEL_OFF);
    g_params.pvGelMinW           = prefs.getUShort("pvGelMinW",     DEFAULT_PV_GEL_MIN_W);
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
    // v5.5-Migration: neue Schlüssel anlegen, entfallene aufräumen.
    if (!prefs.isKey("pvDPlusMinW")) prefs.putUShort("pvDPlusMinW", DEFAULT_PV_D_PLUS_MIN_W);
    if (!prefs.isKey("socGelOff"))   prefs.putUChar ("socGelOff",   DEFAULT_SOC_GEL_OFF);
    if (!prefs.isKey("manualTOMin")) prefs.putUChar ("manualTOMin", DEFAULT_MANUAL_TIMEOUT_MIN);
    if (prefs.isKey("socDPlusHigh")) prefs.remove("socDPlusHigh");
    if (prefs.isKey("pvThreshOn"))   prefs.remove("pvThreshOn");
    if (prefs.isKey("pvThreshOff"))  prefs.remove("pvThreshOff");
    if (prefs.isKey("socGelHigh"))   prefs.remove("socGelHigh");
    if (prefs.isKey("socWROn"))      prefs.remove("socWROn");

    _load_from_nvs();
    Serial.printf("[PARAMS] D+: SOC %u/%u PV>=%uW|Float\n",
        g_params.socDPlusOn, g_params.socDPlusOff, g_params.pvDPlusMinW);
    Serial.printf("[PARAMS] Gel: SOC %u/%u PV>=%uW|Float | WR: AUS<%u%% (EIN nur manuell)\n",
        g_params.socGelOn, g_params.socGelOff, g_params.pvGelMinW,
        g_params.socWROff);
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
bool params_set_pv_dplus_min_w(uint16_t v) {
    if (v < 10 || v > 2000) return false;
    g_params.pvDPlusMinW = v; prefs.putUShort("pvDPlusMinW", v); return true;
}
bool params_set_soc_gel_on(uint8_t v) {
    if (v < 50 || v > 100) return false;
    g_params.socGelOn = v; prefs.putUChar("socGelOn", v); return true;
}
bool params_set_soc_gel_off(uint8_t v) {
    if (v < 20 || v > 99) return false;
    g_params.socGelOff = v; prefs.putUChar("socGelOff", v); return true;
}
bool params_set_pv_gel_min_w(uint16_t v) {
    if (v > 500) return false;
    g_params.pvGelMinW = v; prefs.putUShort("pvGelMinW", v); return true;
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
    char buf[400];
    snprintf(buf, sizeof(buf),
        "{\"socDPlusOn\":%u,\"socDPlusOff\":%u,\"pvDPlusMinW\":%u,"
        "\"socGelOn\":%u,\"socGelOff\":%u,\"pvGelMinW\":%u,"
        "\"socWROff\":%u,"
        "\"relayDebounceCycles\":%u,\"logIntervalMs\":%lu,"
        "\"manualTimeoutMin\":%u}",
        g_params.socDPlusOn, g_params.socDPlusOff, g_params.pvDPlusMinW,
        g_params.socGelOn, g_params.socGelOff, g_params.pvGelMinW,
        g_params.socWROff,
        g_params.relayDebounceCycles,
        (unsigned long)g_params.logIntervalMs,
        g_params.manualTimeoutMin);
    return String(buf);
}
