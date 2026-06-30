// ============================================================
//  clock.cpp — Womo Energy Core v5.1
//
//  Zeitquelle-Hierarchie beim Boot:
//    1) DS3231 RTC (I2C, GPIO 1/2) — batteriegepuffert, genau
//    2) NVS-Epoch (letzter persistierter Wert)         } Fallback
//    3) build_epoch() (Compile-Zeit)                   }
//
//  Im Betrieb zählt esp_timer_get_time() hoch (kein I2C bei
//  jedem clock_now(); cross-core-sicher via Spinlock).
//
//  24h-Resync: einmal täglich (UTC-Tageswechsel) wird die interne
//  Zeitbasis gegen den genauen DS3231 nachgezogen und die Drift
//  (intern − RTC) gemessen → Frühwarnung für müde Pufferbatterie.
//  Läuft im clock_persist()-Tick (logger_task), NICHT im
//  Notfallpfad (force==true überspringt I2C).
//
//  RTC-Health (present/OSF/Drift/Temp) wird in clock_persist()
//  gecacht (throttled 10 s) → build_live_json() liest nur den
//  Cache, kein I2C im Web-Task, kein Bus-Mutex nötig.
//
//  Zeitzone: DS3231 + interne Basis halten UTC (= Speicherformat).
//  Der lokale Offset (inkl. Sommerzeit) kommt aus newlib localtime_r,
//  konfiguriert über einen POSIX-TZ-String (setenv("TZ")+tzset).
//  Default Europe/Berlin; zur Laufzeit via clock_set_tz() änderbar,
//  persistiert in NVS "clock"/"tz". Kein Monotonizitätsschutz (v5.0).
// ============================================================
#include "clock.h"
#include "config.h"
#include <Preferences.h>
#include <esp_timer.h>
#include <Wire.h>
#include <time.h>      // localtime_r, tzset, strftime, struct tm
#include <stdlib.h>    // setenv

// ── DS3231 ──
#define DS3231_ADDR        0x68
#define DS3231_REG_TIME    0x00   // Sek,Min,Std,Wochentag,Tag,Monat,Jahr (BCD)
#define DS3231_REG_TEMP    0x11   // Temp MSB (signed °C), LSB Bit7:6 = .25°C
#define DS3231_REG_STATUS  0x0F   // Bit7 = OSF (Oscillator Stop Flag)

static Preferences cprefs;
static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;

// Gemeinsamer I2C-Bus-Mutex (DS3231 + Lagesensor). In clock_init() erzeugt.
SemaphoreHandle_t g_i2cMutex = nullptr;
static uint64_t s_baseUs   = 0;
static bool     s_hasTime  = false;
static bool     s_synced   = false;
static uint32_t s_lastPersistEpoch = 0;

// ── RTC-Health (gecacht, von clock_persist gepflegt) ──────────
static bool     s_rtcValid        = false;  // OSF beim Boot NICHT gesetzt
static bool     s_rtcPresentLive  = false;  // antwortet aktuell auf I2C
static float    s_rtcTempC        = 0.0f;
static bool     s_haveDrift       = false;
static int32_t  s_lastDriftSec    = 0;       // intern − RTC (+ = intern voraus)
static uint32_t s_lastResyncDay   = 0;       // UTC-Tag des letzten Resyncs
static uint32_t s_lastHealthMs    = 0;

// ── Zeitzonenfreie Epoch <-> Y/M/D (Howard Hinnant; 1970+) ────
// Nur für DS3231-Roh-BCD (immer UTC). Anzeige-Offset läuft über libc.
static uint32_t ymd_to_epoch(int Y, int Mo, int D, int h, int m, int s) {
    long y = Y; y -= (Mo <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp  = (Mo > 2 ? Mo - 3 : Mo + 9);
    unsigned doy = (153 * mp + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (uint32_t)(days * 86400L + h * 3600L + m * 60L + s);
}
static void epoch_to_ymd(uint32_t e, int& Y, int& Mo, int& D,
                         int& h, int& m, int& s) {
    s = e % 60; e /= 60;
    m = e % 60; e /= 60;
    h = e % 24; e /= 24;
    long days = (long)e + 719468L;
    long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;
    unsigned d   = doy - (153 * mp + 2) / 5 + 1;
    unsigned mon = mp < 10 ? mp + 3 : mp - 9;
    y += (mon <= 2);
    Y = (int)y; Mo = (int)mon; D = (int)d;
}

static inline uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// ── DS3231 lesen → UTC-Epoch. false wenn fehlend/OSF/unplausibel ──
static bool ds3231_read_epoch(uint32_t& epochOut) {
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(DS3231_REG_STATUS);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(DS3231_ADDR, 1) != 1) return false;
    uint8_t status = Wire.read();
    if (status & 0x80) {
        Serial.println("[CLOCK] DS3231 OSF gesetzt — Zeit ungültig (Batterie?)");
        return false;
    }
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(DS3231_REG_TIME);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(DS3231_ADDR, 7) != 7) return false;

    uint8_t ss = Wire.read(), mm = Wire.read(), hh = Wire.read();
    Wire.read();                          // Wochentag — ungenutzt
    uint8_t dd = Wire.read(), mo = Wire.read(), yy = Wire.read();

    int sec = bcd2bin(ss & 0x7F);
    int min = bcd2bin(mm & 0x7F);
    int hour;
    if (hh & 0x40) { hour = bcd2bin(hh & 0x1F) % 12; if (hh & 0x20) hour += 12; }
    else           { hour = bcd2bin(hh & 0x3F); }
    int day  = bcd2bin(dd & 0x3F);
    int mon  = bcd2bin(mo & 0x1F);
    int year = 2000 + bcd2bin(yy);

    if (year < 2024 || year > 2099 || mon < 1 || mon > 12 ||
        day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        Serial.printf("[CLOCK] DS3231 unplausibel: %04d-%02d-%02d %02d:%02d:%02d\n",
                      year, mon, day, hour, min, sec);
        return false;
    }
    epochOut = ymd_to_epoch(year, mon, day, hour, min, sec);
    return true;
}

// ── UTC-Epoch in DS3231 schreiben + OSF löschen ───────────────
static bool ds3231_write_epoch(uint32_t epoch) {
    int Y, Mo, D, h, m, s;
    epoch_to_ymd(epoch, Y, Mo, D, h, m, s);
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(DS3231_REG_TIME);
    Wire.write(bin2bcd(s));
    Wire.write(bin2bcd(m));
    Wire.write(bin2bcd(h));               // Bit6=0 → 24h-Modus
    Wire.write(bin2bcd(1));               // Wochentag (egal)
    Wire.write(bin2bcd(D));
    Wire.write(bin2bcd(Mo));              // Bit7 (Century)=0
    Wire.write(bin2bcd(Y - 2000));
    if (Wire.endTransmission() != 0) {
        Serial.println("[CLOCK] DS3231 Schreiben fehlgeschlagen");
        return false;
    }
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(DS3231_REG_STATUS);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(DS3231_ADDR, 1) == 1) {
        uint8_t st = Wire.read();
        Wire.beginTransmission(DS3231_ADDR);
        Wire.write(DS3231_REG_STATUS);
        Wire.write(st & 0x7F);            // OSF löschen
        Wire.endTransmission();
    }
    s_rtcValid = true;
    Serial.printf("[CLOCK] DS3231 gestellt: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  Y, Mo, D, h, m, s);
    return true;
}

// ── DS3231 Temperatur lesen (impliziter Present-Check) ────────
static bool ds3231_read_temp(float& tOut) {
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(DS3231_REG_TEMP);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(DS3231_ADDR, 2) != 2) return false;
    int8_t  msb = (int8_t)Wire.read();
    uint8_t lsb = Wire.read();
    tOut = (float)msb + ((lsb >> 6) * 0.25f);
    return true;
}

static uint32_t build_epoch() {
    static const char* M = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {}; int d = 1, y = 2025, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char* p = strstr(M, mon);
    int mi = p ? (int)((p - M) / 3) : 0;
    return ymd_to_epoch(y, mi + 1, d, hh, mm, ss);
}

static void set_base_from_epoch(uint32_t epoch) {
    taskENTER_CRITICAL(&s_clock_mux);
    s_baseUs = (uint64_t)epoch * 1000000ULL - esp_timer_get_time();
    taskEXIT_CRITICAL(&s_clock_mux);
}

// ── POSIX-TZ in die libc übernehmen (setenv + tzset) ──────────
static void apply_tz(const char* tz) {
    setenv("TZ", tz, 1);
    tzset();
}

void clock_init() {
    Wire.begin(I2C_RTC_SDA, I2C_RTC_SCL);
    Wire.setClock(100000);
    delay(100);                  // I2C-Block + DS3231 settlen lassen

    // Bus-Mutex erzeugen, BEVOR irgendein Task auf den I2C-Bus zugreift.
    // (Boot-Pfad hier ist single-threaded — Schutz greift erst zur Laufzeit.)
    if (!g_i2cMutex) g_i2cMutex = xSemaphoreCreateMutex();

    cprefs.begin("clock", false);

    // Zeitzone aus NVS laden (Default Europe/Berlin) und an libc übergeben.
    // Muss VOR jeder localtime_r-Nutzung passieren.
    String tz = cprefs.getString("tz", DEFAULT_TZ);
    apply_tz(tz.c_str());
    Serial.printf("[CLOCK] TZ: %s\n", tz.c_str());

    uint32_t nvsBase = cprefs.getULong("epoch", 0);
    uint32_t be      = build_epoch();

    // Erster RTC-Read mit Retry: direkt nach Wire.begin() gibt der
    // S3-I2C-Block gelegentlich Error 263 (Timeout), bevor er bereit
    // ist. Bis zu 3 Versuche mit kurzer Pause.
    uint32_t rtcEpoch = 0;
    bool gotRtc = false;
    for (int i = 0; i < 3 && !gotRtc; i++) {
        gotRtc = ds3231_read_epoch(rtcEpoch);
        if (!gotRtc) delay(50);
    }

    uint32_t base;
    if (gotRtc) {
        base       = (rtcEpoch >= be) ? rtcEpoch : be;
        s_rtcValid = true;
        s_synced   = true;
        Serial.printf("[CLOCK] Zeitquelle: DS3231 RTC (epoch %lu)\n",
                      (unsigned long)rtcEpoch);
    } else {
        base       = (nvsBase >= be) ? nvsBase : be;
        s_rtcValid = false;
        s_synced   = false;
        Serial.println("[CLOCK] Zeitquelle: NVS/Build (DS3231 nicht verfügbar)");
    }

    set_base_from_epoch(base);
    s_hasTime = true;
    s_lastPersistEpoch = base;

    // Health-Cache initial füllen + Resync-Tag (UTC) setzen (kein Sofort-Resync)
    float t;
    s_rtcPresentLive = ds3231_read_temp(t);
    if (s_rtcPresentLive) s_rtcTempC = t;
    s_lastResyncDay  = clock_now() / 86400UL;     // UTC-Tag

    Serial.printf("[CLOCK] Start UTC ~%lu (lokal: ~%lu)\n",
                  (unsigned long)base,
                  (unsigned long)(base + clock_local_offset_at(base)));
}

bool clock_set_epoch(uint32_t epoch) {
    if (epoch < 1704067200UL) {
        Serial.printf("[CLOCK] Abgelehnt: epoch=%lu\n", (unsigned long)epoch);
        return false;
    }
    set_base_from_epoch(epoch);
    s_hasTime = true;
    s_synced  = true;
    cprefs.putULong("epoch", epoch);
    s_lastPersistEpoch = epoch;
    // Kann aus dem AsyncTCP-Handler (/api/time) kommen → Bus-Mutex Pflicht.
    if (g_i2cMutex && xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ds3231_write_epoch(epoch);        // Hardware-Uhr stellen
        xSemaphoreGive(g_i2cMutex);
    }
    s_lastResyncDay = clock_now() / 86400UL;  // heute (UTC) kein Resync mehr
    Serial.printf("[CLOCK] Sync UTC=%lu lokal=%lu\n",
                  (unsigned long)epoch,
                  (unsigned long)(epoch + clock_local_offset_at(epoch)));
    return true;
}

bool     clock_is_synced()   { return s_synced; }
bool     clock_rtc_present() { return s_rtcPresentLive; }

uint32_t clock_now() {
    taskENTER_CRITICAL(&s_clock_mux);
    uint64_t base = s_baseUs;
    taskEXIT_CRITICAL(&s_clock_mux);
    return (uint32_t)((base + esp_timer_get_time()) / 1000000ULL);
}

// ── Lokaler Offset (s) für einen UTC-Zeitpunkt — DST-korrekt ───
// localtime_r füllt tm_gmtoff gemäß gesetztem POSIX-TZ (inkl. Sommerzeit).
int32_t clock_local_offset_at(uint32_t utc) {
    time_t t = (time_t)utc;
    struct tm lt;
    localtime_r(&t, &lt);
    return (int32_t)lt.tm_gmtoff;
}

uint32_t clock_now_local() {
    uint32_t u = clock_now();
    return u + clock_local_offset_at(u);
}

// ── Zeitzone setzen / abfragen ────────────────────────────────
void clock_set_tz(const char* tz) {
    if (!tz || !*tz) return;
    cprefs.putString("tz", tz);
    apply_tz(tz);
    Serial.printf("[CLOCK] TZ gesetzt: %s\n", tz);
}

String clock_tz() {
    return cprefs.getString("tz", DEFAULT_TZ);
}

String clock_tz_abbr() {
    time_t t = (time_t)clock_now();
    struct tm lt;
    localtime_r(&t, &lt);
    char z[8] = {0};
    strftime(z, sizeof(z), "%Z", &lt);   // z. B. "CET" / "CEST"
    return String(z);
}

// ── 24h-Resync bei UTC-Tageswechsel (nur Normalbetrieb) ───────
static void rtc_midnight_resync() {
    uint32_t today = clock_now() / 86400UL;   // UTC-Tag (TZ-unabhängig)
    if (today == s_lastResyncDay) return;      // noch kein neuer Tag

    uint32_t rtcEpoch;
    bool got = false;
    if (g_i2cMutex && xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        got = ds3231_read_epoch(rtcEpoch);
        xSemaphoreGive(g_i2cMutex);
    }
    if (got) {
        int32_t drift  = (int32_t)(clock_now() - rtcEpoch);  // intern − RTC
        s_lastDriftSec = drift;
        s_haveDrift    = true;
        set_base_from_epoch(rtcEpoch);        // interne Uhr auf RTC ziehen
        cprefs.putULong("epoch", rtcEpoch);
        s_lastPersistEpoch = rtcEpoch;
        Serial.printf("[CLOCK] 24h-Resync: Drift intern-RTC = %+ld s\n",
                      (long)drift);
    } else {
        Serial.println("[CLOCK] 24h-Resync: DS3231 nicht lesbar - übersprungen");
    }
    s_lastResyncDay = today;                  // pro Tag nur einmal versuchen
}

// ── RTC-Health-Cache (present + Temp), throttled ──────────────
static void rtc_health_update() {
    if ((uint32_t)(millis() - s_lastHealthMs) < 10000) return;
    s_lastHealthMs = millis();
    float t;
    bool got = false;
    if (g_i2cMutex && xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        got = ds3231_read_temp(t);
        xSemaphoreGive(g_i2cMutex);
    }
    if (got) { s_rtcTempC = t; s_rtcPresentLive = true; }
    else       s_rtcPresentLive = false;
}

void clock_persist(bool force) {
    if (!s_hasTime) return;
    if (!force) {                             // Notfallpfad überspringt I2C
        rtc_midnight_resync();
        rtc_health_update();
    }
    uint32_t now_epoch = clock_now();
    if (!force) {
        int32_t diff = (int32_t)(now_epoch - s_lastPersistEpoch);
        if (diff < 600 && diff > -60) return;
    }
    cprefs.putULong("epoch", now_epoch);
    s_lastPersistEpoch = now_epoch;
}

// ── RTC-Status als JSON-Fragment (nur Cache, kein I2C) ────────
String clock_rtc_json() {
    String s = "{\"present\":";
    s += s_rtcPresentLive ? "true" : "false";
    s += ",\"valid\":";
    s += s_rtcValid ? "true" : "false";
    s += ",\"drift\":";
    s += s_haveDrift ? String(s_lastDriftSec) : "null";
    s += ",\"temp\":";
    s += s_rtcPresentLive ? String(s_rtcTempC, 2) : "null";
    s += "}";
    return s;
}
