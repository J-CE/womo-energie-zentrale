// ============================================================
//  logic.cpp — Womo Energy Core v5.5
//
//  v5.5 Änderungen (Kriterien-Redesign):
//   • D+/Gel: nur noch HARTE AUS-Bedingungen (Landstrom, BMS
//     ungültig/veraltet, SoC < Off). Kein weiches PV-AUS, kein
//     socHigh, keine D+-Mindestlaufzeit (war nur Hemmung weicher
//     Gründe — die es nicht mehr gibt).
//   • MPPT-Ausfall ist KEINE AUS-Bedingung mehr — ein VE.Direct-
//     Glitch schaltet nichts mehr ab. mppt_ok gilt nur noch als
//     EIN-Voraussetzung (PV/Float sonst nicht bewertbar).
//   • EIN-Bedingung PV: PV(MA) >= Schwelle ODER MPPT-Ladezustand
//     Float (CS=5) — im Float ist der Akku voll, Überschuss da.
//   • Gel: Landstrom neu als hartes AUS; eigener socGelOff.
//   • WR: KEIN Auto-EIN mehr (nur manuell). Auto-AUS (BMS hart,
//     SoC<WROff weich) gilt auch während Manuell-EIN und beendet
//     ihn — Sicherheitsnetz, da der WR-Deadman-Timer entfällt.
//   • Manual-Semantik: EIN D+/Gel mit Deadman (wie v5.4, über-
//     steuert Interlocks); EIN WR ohne Timer; AUS dauerhaft und
//     NVS-persistent (Namespace "logic", überlebt Reboot).
//
//  v5.4: Manueller Aktor-Override (Basis, s. oben)
//  v5.3: WR-Remote ohne Landstrom-Bedingung (Renogy NVS)
//  v5.1: RGB-LED Rundlauf-Bitmask statt Prioritäts-Enum
// ============================================================
#include "logic.h"
#include "config.h"
#include "params.h"
#include "bms.h"
#include "mppt.h"
#include "io.h"
#include <Preferences.h>
#include <stdarg.h>

static int8_t db_dplus = 0;
static int8_t db_gel   = 0;
static int8_t db_wr    = 0;

// ── Manueller Aktor-Override ──────────────────────────────────
// Cross-Core-Zugriff: HTTP-Handler (AsyncTCP-Task) schreibt,
// logic_evaluate() (logic_task) liest+räumt auf. Spinlock analog
// zu s_reason_mux, da nur wenige Bytes und kurze Haltezeit.
// v5.5: Manuell-AUS wird in NVS "logic" persistiert (Schlüssel
// manOff0..2). NVS-Zugriffe laufen IMMER außerhalb des Spinlocks
// (Flash-Write unter taskENTER_CRITICAL wäre fatal).
struct ManualState { bool active; bool want; uint32_t lastCmdMs; };
static ManualState  s_manual[3] = {};
static portMUX_TYPE s_manual_mux = portMUX_INITIALIZER_UNLOCKED;
static Preferences  logic_prefs;
static const char*  MAN_OFF_KEY[3] = { "manOff0", "manOff1", "manOff2" };

bool logic_set_manual(ManualActuator a, bool active, bool want) {
    if (a > MANUAL_WR) return false;
    taskENTER_CRITICAL(&s_manual_mux);
    s_manual[a].active    = active;
    s_manual[a].want      = want;
    s_manual[a].lastCmdMs = millis();
    taskEXIT_CRITICAL(&s_manual_mux);
    // Persistenz NUR für dauerhaftes Manuell-AUS; jeder andere Befehl
    // (Auto/EIN) löscht das Flag. Write außerhalb des Spinlocks.
    bool persistOff = (active && !want);
    if (logic_prefs.getBool(MAN_OFF_KEY[a], false) != persistOff)
        logic_prefs.putBool(MAN_OFF_KEY[a], persistOff);
    return true;
}

// Schnappschuss lesen + Deadman-Timeout auswerten. Der Timeout gilt
// v5.5 NUR für Manuell-EIN von D+ und Gel — Manuell-AUS ist dauerhaft,
// WR-Manuell-EIN läuft ohne Timer. Bei Ablauf wird active hier direkt
// zurückgesetzt (einziger Schreiber im logic_task-Kontext neben
// logic_set_manual selbst).
static ManualState manual_check(ManualActuator a) {
    ManualState ms;
    taskENTER_CRITICAL(&s_manual_mux);
    ms = s_manual[a];
    if (ms.active && ms.want && a != MANUAL_WR) {
        uint32_t limitMs = (uint32_t)g_params.manualTimeoutMin * 60000UL;
        if ((uint32_t)(millis() - ms.lastCmdMs) > limitMs) {
            s_manual[a].active = false;
            ms.active = false;
        }
    }
    taskEXIT_CRITICAL(&s_manual_mux);
    return ms;
}

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
static int8_t s_mppt_recovery = 0;

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
    s_mppt_recovery = 0;

    // v5.5: dauerhaftes Manuell-AUS aus NVS wiederherstellen (überlebt
    // Reboot/Watchdog-Reset). Manuell-EIN ist nie persistent — alles
    // andere startet als Auto (Fail-Safe).
    logic_prefs.begin("logic", false);
    taskENTER_CRITICAL(&s_manual_mux);
    memset(s_manual, 0, sizeof(s_manual));
    taskEXIT_CRITICAL(&s_manual_mux);
    for (uint8_t a = 0; a < 3; a++) {
        if (logic_prefs.getBool(MAN_OFF_KEY[a], false)) {
            taskENTER_CRITICAL(&s_manual_mux);
            s_manual[a].active = true;
            s_manual[a].want   = false;
            s_manual[a].lastCmdMs = millis();
            taskEXIT_CRITICAL(&s_manual_mux);
            Serial.printf("[LOGIC] Aktor %u: Manuell-AUS aus NVS wiederhergestellt\n", a);
        }
    }
    Serial.println("[LOGIC] Initialisiert (v5.5-Kriterien)");
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
    uint8_t  cs_raw       = 0;
    bool     mppt_raw_ok  = false;
    if (xSemaphoreTake(g_mpptMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        ppv_raw     = g_mppt.panelPower;
        cs_raw      = g_mppt.chargeState;
        mppt_raw_ok = g_mppt.valid && !g_mppt.timeout &&
                      (uint32_t)(millis() - g_mppt.lastUpdateMs) < (MPPT_FRAME_TIMEOUT_MS * 2);
        xSemaphoreGive(g_mpptMutex);
    }
    if (mppt_raw_ok) { if (s_mppt_recovery < LOGIC_MPPT_RECOVERY_MIN) s_mppt_recovery++; }
    else              { s_mppt_recovery = 0; }
    bool mppt_ok = (s_mppt_recovery >= LOGIC_MPPT_RECOVERY_MIN);

    // PV-MA: bei MPPT-Ausfall 0 einschieben → gradueller Rückgang
    uint16_t ppv = ppv_moving_avg(mppt_ok ? ppv_raw : 0);
    // Float-Modus zählt als "genug PV" (nur bei gültigen MPPT-Daten)
    bool pv_float = mppt_ok && (cs_raw == MPPT_CS_FLOAT);

    // ── 1. D+ Kühlschrank ─────────────────────────────────
    // v5.5: EIN SoC>=On UND (PV>=Min ODER Float) | AUS nur hart:
    // Landstrom, BMS ungültig, SoC<Off. MPPT-Ausfall schaltet NICHT ab.
    {
        ManualState mst = manual_check(MANUAL_DPLUS);
        bool cur = g_io.relayDPlus;
        if (mst.active) {
            // Manuell übersteuert alles — keine Interlock-Prüfung, kein
            // Debounce (v5.4-Design). EIN: Deadman läuft. AUS: dauerhaft.
            if (mst.want != cur) io_set_relay_dplus(mst.want);
            db_dplus = 0;
            if (mst.want) {
                // F-07: Unterlauf vermeiden (Race mit manual_check)
                uint32_t elapsed = millis() - mst.lastCmdMs;
                uint32_t limitMs = (uint32_t)g_params.manualTimeoutMin * 60000UL;
                uint32_t remainS = (elapsed < limitMs) ? (limitMs - elapsed) / 1000 : 0;
                set_reason(s_reason_dplus, "MANUAL EIN (Auto in %lus)",
                           (unsigned long)remainS);
            } else {
                set_reason(s_reason_dplus, "MANUAL AUS (dauerhaft)");
            }
        } else {
        bool pv_ok = mppt_ok && (ppv >= g_params.pvDPlusMinW || pv_float);
        bool want = cur;
        if (!cur && bms_ok && !landstrom &&
            soc >= g_params.socDPlusOn && pv_ok)
            want = true;

        // AUS-Gründe — v5.5 ausnahmslos HART (sofort, ohne Debounce)
        const char* off = nullptr;
        if (!off && !bms_ok)   off = "BMS veraltet/ungültig";
        if (!off && landstrom) off = "Landstrom";
        if (!off && (soc < g_params.socDPlusOff)) off = "SoC < hardOff";
        if (off) want = false;

        if (off && cur) {
            io_set_relay_dplus(false);
            db_dplus = 0;
            set_reason(s_reason_dplus, "AUS (hart-sofort): %s", off);
        } else {
        if (!want && !cur)
            set_reason(s_reason_dplus, "AUS: %s", off ? off :
                (pv_ok ? "EIN-Bed. nicht erfüllt" : "PV<Min & kein Float"));
        else if (want && !cur)
            set_reason(s_reason_dplus, "EIN: SoC=%u%% PV=%uW%s", soc, ppv,
                       pv_float ? "(Float)" : "(MA)");
        else
            set_reason(s_reason_dplus, "%s SoC=%u%% PV=%uW%s",
                cur?"EIN(halte)":"AUS", soc, ppv, pv_float?" Float":"");

        if (debounce(db_dplus, want, cur))
            io_set_relay_dplus(want);
        }
        }
    }

    // ── 2. Gel-Lader ──────────────────────────────────────
    // v5.5: wie D+ — inkl. Landstrom als hartes AUS (neu) und
    // eigenem socGelOff. MPPT-Ausfall schaltet NICHT ab.
    {
        ManualState mst = manual_check(MANUAL_GEL);
        bool cur = g_io.mosfetGel;
        if (mst.active) {
            if (mst.want != cur) io_set_mosfet_gel(mst.want);
            db_gel = 0;
            if (mst.want) {
                uint32_t elapsed = millis() - mst.lastCmdMs;
                uint32_t limitMs = (uint32_t)g_params.manualTimeoutMin * 60000UL;
                uint32_t remainS = (elapsed < limitMs) ? (limitMs - elapsed) / 1000 : 0;
                set_reason(s_reason_gel, "MANUAL EIN (Auto in %lus)",
                           (unsigned long)remainS);
            } else {
                set_reason(s_reason_gel, "MANUAL AUS (dauerhaft)");
            }
        } else {
        bool pv_ok = mppt_ok && (ppv >= g_params.pvGelMinW || pv_float);
        bool want = cur;
        if (!cur && bms_ok && !landstrom &&
            soc >= g_params.socGelOn && pv_ok)
            want = true;

        const char* off = nullptr;
        if (!off && !bms_ok)   off = "BMS veraltet/ungültig";
        if (!off && landstrom) off = "Landstrom";
        if (!off && (soc < g_params.socGelOff)) off = "SoC < GelOff";
        if (off) want = false;

        if (off && cur) {
            io_set_mosfet_gel(false);
            db_gel = 0;
            set_reason(s_reason_gel, "AUS (hart-sofort): %s", off);
        } else {
        if (!want && !cur)
            set_reason(s_reason_gel, "AUS: %s", off ? off :
                (pv_ok ? "EIN-Bed. nicht erfüllt" : "PV<GelMin & kein Float"));
        else if (want && !cur)
            set_reason(s_reason_gel, "EIN: SoC=%u%% PV=%uW%s", soc, ppv,
                       pv_float ? "(Float)" : "(MA)");
        else
            set_reason(s_reason_gel, "%s SoC=%u%% PV=%uW%s",
                cur?"EIN(halte)":"AUS", soc, ppv, pv_float?" Float":"");

        if (debounce(db_gel, want, cur))
            io_set_mosfet_gel(want);
        }
        }
    }

    // ── 3. Wechselrichter Remote — v5.5: KEIN Auto-EIN ────
    // Einschalten nur manuell (ohne Deadman-Timer). Die AUS-Bedingungen
    // (BMS ungültig hart, SoC<WROff weich/debounced) gelten IMMER —
    // auch während Manuell-EIN — und beenden dann den Manual-Modus.
    // Manuell-AUS ist dauerhaft. Kein MPPT-/PV-/Landstrom-Check (v5.3).
    {
        ManualState mst = manual_check(MANUAL_WR);
        bool cur = g_io.wrRemote;

        // AUS-Bedingungen zentral (gelten für Auto UND Manuell-EIN)
        const char* off = nullptr;
        if (!off && !bms_ok)                 off = "BMS veraltet/ungültig";
        if (!off && soc < g_params.socWROff) off = "SoC < WROff";
        bool hard_off = (!bms_ok);

        if (mst.active && !mst.want) {
            // Manuell AUS — dauerhaft, kein Timer
            if (cur) io_set_wr_remote(false);
            db_wr = 0;
            set_reason(s_reason_wr, "MANUAL AUS (dauerhaft)");
        } else if (mst.active && mst.want) {
            // Manuell EIN ohne Timer — Sicherheitsnetz: Auto-AUS greift
            if (hard_off) {
                if (cur) io_set_wr_remote(false);
                db_wr = 0;
                logic_set_manual(MANUAL_WR, false, false);   // Manual beenden
                set_reason(s_reason_wr, "AUS (hart): %s", off);
            } else if (off) {
                // weiche Bedingung: debounced abschalten, dann Manual beenden
                if (!cur) {
                    // EIN-Befehl bei bereits verletzter Bedingung → ablehnen
                    db_wr = 0;
                    logic_set_manual(MANUAL_WR, false, false);
                    set_reason(s_reason_wr, "EIN abgelehnt: %s", off);
                } else {
                    set_reason(s_reason_wr, "MANUAL EIN — AUS folgt (Debounce): %s", off);
                    if (debounce(db_wr, false, cur)) {
                        io_set_wr_remote(false);
                        logic_set_manual(MANUAL_WR, false, false);
                    }
                }
            } else {
                if (!cur) io_set_wr_remote(true);
                db_wr = 0;
                set_reason(s_reason_wr, "MANUAL EIN (bis Befehl/AUS-Bed.) SoC=%u%%", soc);
            }
        } else {
            // Automatik: es gibt kein Auto-EIN → Soll ist immer AUS.
            if (hard_off && cur) {
                io_set_wr_remote(false);
                db_wr = 0;
                set_reason(s_reason_wr, "AUS (hart-sofort): %s", off);
            } else if (cur) {
                // z. B. nach Ende eines Manuell-EIN per "Auto"-Befehl:
                // debounced abschalten (weich), Grund anzeigen.
                set_reason(s_reason_wr, "AUS folgt (Debounce): %s",
                           off ? off : "kein Auto-EIN");
                if (debounce(db_wr, false, cur))
                    io_set_wr_remote(false);
            } else {
                db_wr = 0;
                set_reason(s_reason_wr, "AUS: EIN nur manuell (SoC=%u%%)", soc);
            }
        }
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

    // Manual-Snapshot für UI-Segmentbuttons (Auto/Ein/Aus-Anzeige).
    // Kein Deadman-Check hier (nur lesend) — logic_evaluate() räumt
    // abgelaufene Manual-Flags spätestens beim nächsten 2s-Zyklus auf.
    ManualState mm[3];
    taskENTER_CRITICAL(&s_manual_mux);
    mm[0] = s_manual[MANUAL_DPLUS];
    mm[1] = s_manual[MANUAL_GEL];
    mm[2] = s_manual[MANUAL_WR];
    taskEXIT_CRITICAL(&s_manual_mux);

    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"db_dplus\":%d,\"db_gel\":%d,\"db_wr\":%d,"
        "\"reason_dplus\":\"%s\","
        "\"reason_gel\":\"%s\","
        "\"reason_wr\":\"%s\","
        "\"ppv_ma_n\":%u,\"mppt_rec\":%d,"
        "\"manual\":{"
          "\"dplus\":{\"a\":%s,\"w\":%s},"
          "\"gel\":{\"a\":%s,\"w\":%s},"
          "\"wr\":{\"a\":%s,\"w\":%s}"
        "},\"manualTimeoutMin\":%u}",
        db_dplus, db_gel, db_wr, r1, r2, r3,
        (unsigned)s_ppv_count, (int)s_mppt_recovery,
        mm[0].active?"true":"false", mm[0].want?"true":"false",
        mm[1].active?"true":"false", mm[1].want?"true":"false",
        mm[2].active?"true":"false", mm[2].want?"true":"false",
        g_params.manualTimeoutMin);
    return String(buf);
}
