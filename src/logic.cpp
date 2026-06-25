// ============================================================
//  logic.cpp — Womo Energy Core v5.1
//
//  v5.1 Änderungen:
//   • RGB-LED: Rundlauf-Bitmask statt Prioritäts-Enum (s. io.h/io.cpp)
//     — alle Kanäle (BMS/MPPT/SoC/Landstrom/D+/Gel/WR) gleichberechtigt
//
//  v5.0 Änderungen:
//   • Neue AUS-Formeln für D+ und Gel (socHigh-Parameter):
//     AUS = (PV<Off AND SoC<High) OR (SoC<hardOff)
//   • WR rein SoC-basiert: kein MPPT-ok Check, kein PV-Check
//   • Staleness-Timeout auf BMS_STALE_TIMEOUT_MS (60s aus config.h)
//   • socGelOff entfernt (harte AUS = socGelOn)
// ============================================================
#include "logic.h"
#include "config.h"
#include "params.h"
#include "bms.h"
#include "mppt.h"
#include "io.h"
#include <stdarg.h>

static int8_t db_dplus = 0;
static int8_t db_gel   = 0;
static int8_t db_wr    = 0;

// PV-Glättung
static uint16_t s_ppv_ma[LOGIC_PPV_MA_WINDOW] = {};
static uint8_t  s_ppv_idx   = 0;
static uint8_t  s_ppv_count = 0;

static uint16_t ppv_moving_avg(uint16_t v) {
    s_ppv_ma[s_ppv_idx] = v;
    s_ppv_idx = (s_ppv_idx + 1) % LOGIC_PPV_MA_WINDOW;
    if (s_ppv_count < LOGIC_PPV_MA_WINDOW) s_ppv_count++;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < s_ppv_count; i++) sum += s_ppv_ma[i];
    return (uint16_t)(sum / s_ppv_count);
}

// MPPT-Recovery-Debounce
static int8_t   s_mppt_recovery  = 0;
// D+-Mindestlaufzeit
static uint32_t s_dplus_on_since = 0;

// Reason-Strings (Spinlock gegen Cross-Core-Race)
#define REASON_LEN 64
static portMUX_TYPE s_reason_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_reason_dplus[REASON_LEN] = "Init";
static char s_reason_gel  [REASON_LEN] = "Init";
static char s_reason_wr   [REASON_LEN] = "Init";

static void set_reason(char* dst, const char* fmt, ...) {
    char tmp[REASON_LEN];
    va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    taskENTER_CRITICAL(&s_reason_mux);
    strlcpy(dst, tmp, REASON_LEN);
    taskEXIT_CRITICAL(&s_reason_mux);
}

static bool debounce(int8_t& ctr, bool want, bool cur) {
    int8_t lim = (int8_t)g_params.relayDebounceCycles;
    if (want && !cur) { ctr = (ctr < 0) ? 1 : ctr+1; return ctr >= lim; }
    if (!want && cur) { ctr = (ctr > 0) ? -1 : ctr-1; return ctr <= -lim; }
    ctr = 0; return false;
}

void logic_init() {
    db_dplus = 0; db_gel = 0; db_wr = 0;
    memset(s_ppv_ma, 0, sizeof(s_ppv_ma));
    s_ppv_idx = 0; s_ppv_count = 0;
    s_mppt_recovery = 0; s_dplus_on_since = 0;
    Serial.println("[LOGIC] Initialisiert");
}

void logic_evaluate() {
    bool landstrom = io_read_landstrom();

    // ── BMS-Snapshot + Staleness ──────────────────────────
    uint8_t soc    = 0;
    bool    bms_ok = false;
    if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        soc    = g_bms.soc;
        bms_ok = g_bms.valid &&
                 (uint32_t)(millis() - g_bms.lastUpdateMs) < BMS_STALE_TIMEOUT_MS;
        xSemaphoreGive(g_bmsMutex);
    }

    // ── MPPT-Snapshot + Recovery-Debounce ────────────────
    uint16_t ppv_raw      = 0;
    bool     mppt_raw_ok  = false;
    if (xSemaphoreTake(g_mpptMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        ppv_raw     = g_mppt.panelPower;
        mppt_raw_ok = g_mppt.valid && !g_mppt.timeout &&
                      (uint32_t)(millis() - g_mppt.lastUpdateMs) < (MPPT_FRAME_TIMEOUT_MS * 2);
        xSemaphoreGive(g_mpptMutex);
    }
    if (mppt_raw_ok) { if (s_mppt_recovery < LOGIC_MPPT_RECOVERY_MIN) s_mppt_recovery++; }
    else              { s_mppt_recovery = 0; }
    bool mppt_ok = (s_mppt_recovery >= LOGIC_MPPT_RECOVERY_MIN);

    // PV-MA: bei MPPT-Ausfall 0 einschieben → gradueller Rückgang
    uint16_t ppv = ppv_moving_avg(mppt_ok ? ppv_raw : 0);

    // ── 1. D+-Relais Kühlschrank ──────────────────────────
    // AUS-Formel weich: (ppv < pvThreshOff AND soc < socDPlusHigh) OR soc < socDPlusOff
    {
        bool cur  = g_io.relayDPlus;
        bool want = cur;
        if (!cur && bms_ok && mppt_ok && !landstrom &&
            soc >= g_params.socDPlusOn && ppv >= g_params.pvThresholdOn)
            want = true;

        const char* off = nullptr;
        // Hart (sofort, auch während Mindestlaufzeit)
        if (!off && !bms_ok)   off = "BMS veraltet/ungültig";
        if (!off && !mppt_ok)  off = "MPPT Timeout";
        if (!off && landstrom) off = "Landstrom";
        // Weich (durch Mindestlaufzeit hemmbar)
        bool min_on = cur && (uint32_t)(millis()-s_dplus_on_since) < LOGIC_DPLUS_MIN_ON_MS;
        if (!off && !min_on) {
            bool soc_low = (soc < g_params.socDPlusOff);
            bool soft_off = (ppv < g_params.pvThresholdOff) && (soc < g_params.socDPlusHigh);
            if (soc_low)         off = "SoC < hardOff";
            else if (soft_off)   off = "PV<Off & SoC<High";
        }
        if (off) want = false;

        // L1-Fix: Hart-AUS wird durch min_on NICHT gehemmt.
        // off hier = harte Bedingung (BMS/MPPT/Landstrom), want=false.
        if (off && min_on)
            set_reason(s_reason_dplus, "AUS (hart): %s", off);
        else if (!want && !cur)
            set_reason(s_reason_dplus, "AUS: %s", off ? off : "EIN-Bed. nicht erfüllt");
        else if (want && !cur)
            set_reason(s_reason_dplus, "EIN: SoC=%u%% PV=%uW(MA)", soc, ppv);
        else
            set_reason(s_reason_dplus, "%s SoC=%u%% PV=%uW", cur?"EIN(halte)":"AUS", soc, ppv);

        if (debounce(db_dplus, want, cur)) {
            io_set_relay_dplus(want);
            if (want) s_dplus_on_since = millis();
        }
    }

    // ── 2. Gel-Lader ──────────────────────────────────────
    // Harte AUS-Schwelle = socGelOn (kein separater socGelOff)
    // AUS-Formel weich: (ppv < pvGelMinW AND soc < socGelHigh) OR soc < socGelOn
    {
        bool cur  = g_io.mosfetGel;
        bool want = cur;
        if (!cur && bms_ok && mppt_ok &&
            soc >= g_params.socGelOn && ppv >= g_params.pvGelMinW)
            want = true;

        const char* off = nullptr;
        if (!off && !bms_ok)  off = "BMS veraltet/ungültig";
        if (!off && !mppt_ok) off = "MPPT Timeout";
        if (!off) {
            bool soc_low  = (soc < g_params.socGelOn);
            bool soft_off = (ppv < g_params.pvGelMinW) && (soc < g_params.socGelHigh);
            if (soc_low)       off = "SoC < GelOn";
            else if (soft_off) off = "PV<GelMin & SoC<GelHigh";
        }
        if (off) want = false;

        if (!want && !cur)
            set_reason(s_reason_gel, "AUS: %s", off ? off : "EIN-Bed. nicht erfüllt");
        else if (want && !cur)
            set_reason(s_reason_gel, "EIN: SoC=%u%% PV=%uW(MA)", soc, ppv);
        else
            set_reason(s_reason_gel, "%s SoC=%u%% PV=%uW", cur?"EIN(halte)":"AUS", soc, ppv);

        if (debounce(db_gel, want, cur))
            io_set_mosfet_gel(want);
    }

    // ── 3. Wechselrichter Remote (nur SoC + Landstrom) ───
    // Kein MPPT-Check, kein PV-Check (v5.0)
    {
        bool cur  = g_io.wrRemote;
        bool want = cur;
        if (!cur && bms_ok && !landstrom && soc >= g_params.socWROn)
            want = true;

        const char* off = nullptr;
        if (!off && !bms_ok)               off = "BMS veraltet/ungültig";
        if (!off && landstrom)             off = "Landstrom";
        if (!off && soc < g_params.socWROff) off = "SoC < WROff";
        if (off) want = false;

        if (!want && !cur)
            set_reason(s_reason_wr, "AUS: %s", off ? off : "EIN-Bed. nicht erfüllt");
        else if (want && !cur)
            set_reason(s_reason_wr, "EIN: SoC=%u%%", soc);
        else
            set_reason(s_reason_wr, "%s SoC=%u%%", cur?"EIN(halte)":"AUS", soc);

        if (debounce(db_wr, want, cur))
            io_set_wr_remote(want);
    }

    // ── Status-LED (GPIO 41, einfach: an=ok, Blinken=BMS-Fehler) ──
    io_set_status_led(bms_ok ? true : !g_io.statusLed);

    // ── On-board RGB-LED: alle Kanäle gleichberechtigt als Bitmask ──
    // v5.1: kein Prioritäts-Switch mehr — jeder Sensor/Aktor liefert
    // sein eigenes Bit (bzw. den SoC-Wert), der LED-Task zeigt daraus
    // im Rundlauf jeden Kanal in seinem festen Slot (s. io.cpp).
    uint32_t rgbMask = ((uint32_t)soc << RGB_SOC_SHIFT) & RGB_SOC_MASK;
    if (!bms_ok)            rgbMask |= RGB_BIT_BMS_FAULT;
    if (!mppt_ok)           rgbMask |= RGB_BIT_MPPT_FAULT;
    if (landstrom)          rgbMask |= RGB_BIT_LANDSTROM;
    if (g_io.relayDPlus)    rgbMask |= RGB_BIT_DPLUS;
    if (g_io.mosfetGel)     rgbMask |= RGB_BIT_GEL;
    if (g_io.wrRemote)      rgbMask |= RGB_BIT_WR;
    g_rgbChannelMask = rgbMask;       // ein atomarer Write (Single-Word)
    g_rgbState       = RGB_RUNNING;   // Boot-Phase endgültig verlassen
}

String logic_status_json() {
    char r1[REASON_LEN], r2[REASON_LEN], r3[REASON_LEN];
    taskENTER_CRITICAL(&s_reason_mux);
    strlcpy(r1, s_reason_dplus, REASON_LEN);
    strlcpy(r2, s_reason_gel,   REASON_LEN);
    strlcpy(r3, s_reason_wr,    REASON_LEN);
    taskEXIT_CRITICAL(&s_reason_mux);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"db_dplus\":%d,\"db_gel\":%d,\"db_wr\":%d,"
        "\"reason_dplus\":\"%s\","
        "\"reason_gel\":\"%s\","
        "\"reason_wr\":\"%s\","
        "\"ppv_ma_n\":%u,\"mppt_rec\":%d}",
        db_dplus, db_gel, db_wr, r1, r2, r3,
        (unsigned)s_ppv_count, (int)s_mppt_recovery);
    return String(buf);
}
