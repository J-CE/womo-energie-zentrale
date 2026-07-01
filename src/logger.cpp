// ============================================================
//  logger.cpp — Womo Energy Core v5.4
//  PSRAM-Ringpuffer + SD-CSV-Logging
//
//  Locking-Strategie:
//    s_mutex:    schützt Ringpuffer (head/count/buf)
//    g_sdMutex:  schützt alle SD-Operationen (shared mit http_server)
//    g_bmsMutex / g_mpptMutex: Snapshot unter Mutex, dann frei
//  Notfallsicherung: watchdog_emergency_save() → kein Mutex-Wait,
//    SD-Take mit 500ms Timeout, danach write ohne Mutex (letzter Ausweg)
//  cellMinMV/cellMaxMV: Felder reserviert (Zellspannungen in v5.0
//    nicht mehr erfasst — 0 gesetzt, Struct-Größe 32B bleibt stabil)
// ============================================================
#include "logger.h"
#include "bms.h"
#include "mppt.h"
#include "io.h"
#include "config.h"
#include "params.h"
#include "clock.h"
#include <SPI.h>
#include <SD.h>

// S3: FSPI oder HSPI
static SPIClass sdSPI(FSPI);   

// ── Globaler SD-Mutex ─────────────────────────────────────────
SemaphoreHandle_t g_sdMutex = nullptr;

// ── PSRAM Ringpuffer ─────────────────────────────────────────
static LogEntry* s_buf   = nullptr;
static uint32_t  s_head  = 0;
static uint32_t  s_count = 0;
static SemaphoreHandle_t s_mutex = nullptr;

uint32_t g_log_count = 0;
uint32_t g_log_head  = 0;

// ── Minuten-Akkumulator ───────────────────────────────────────
struct MinuteAcc {
    int64_t  v, i, soc, tt, s1, s2, mos;
    int64_t  cvmin, cvmax, rem, ppv, vpv, cpv;
    uint32_t count;
    uint8_t  mpptState, mpptError, flags;
    uint32_t lastTs;
};
static MinuteAcc s_min = {};

#define MIN_ROWS_MAX  60
static LogEntry  s_rows[MIN_ROWS_MAX];
static uint8_t   s_rowCount = 0;

static uint32_t  s_lastMinuteMs = 0;
static uint32_t  s_lastFlushMs  = 0;
static bool      s_sd_ok        = false;
static uint32_t  s_sdRetryMs    = 0;      // B-15: nächster Remount-Versuch

// K-4: Batch-Puffer NICHT auf dem Stack (60×32=1920 B) — je einer pro Task,
// da logger_flush_sd (logger_task) und logger_emergency_back_up (wdt_task)
// gleichzeitig laufen können (Watchdog feuert mitten im Flush).
static LogEntry  s_flushBuf[MIN_ROWS_MAX];   // nur logger_task
static LogEntry  s_emergBuf[MIN_ROWS_MAX];   // nur wdt_task

static const char* CSV_HEADER =
    "ts,v,i,soc,t_mos,t_s1,t_s2,t_mos2,"
    "cv_min,cv_max,rem_ah,"
    "pv_w,pv_v,pv_a,"
    "mppt_cs,mppt_err,"
    "landstrom,dplus,gel,wr\n";

// ── Minuten-Mittelwert ────────────────────────────────────────
static LogEntry minute_average(const MinuteAcc& a) {
    LogEntry e = {};
    uint32_t n = a.count ? a.count : 1;
    e.timestamp    = a.lastTs;
    e.bmsVoltage   = (uint16_t)(a.v   / n);
    e.bmsCurrent   = (int16_t) (a.i   / n);
    e.soc          = (uint8_t) (a.soc / n);
    e.tempTube     = (int8_t)  (a.tt  / n);
    e.tempSensor1  = (int8_t)  (a.s1  / n);
    e.tempSensor2  = (int8_t)  (a.s2  / n);
    e.tempMOS      = (int8_t)  (a.mos / n);
    e.cellMinMV    = (uint16_t)(a.cvmin / n);
    e.cellMaxMV    = (uint16_t)(a.cvmax / n);
    e.remainAh10   = (uint16_t)(a.rem / n);
    e.pvPower      = (uint16_t)(a.ppv / n);
    e.pvVoltage10  = (uint16_t)(a.vpv / n);
    e.pvCurrent100 = (uint16_t)(a.cpv / n);
    e.mpptState    = a.mpptState;
    e.mpptError    = a.mpptError;
    e.flags        = a.flags;
    return e;
}

static void finalize_minute_locked() {
    if (s_min.count == 0) return;
    if (s_rowCount < MIN_ROWS_MAX)
        s_rows[s_rowCount++] = minute_average(s_min);
    memset(&s_min, 0, sizeof(s_min));
    s_lastMinuteMs = millis();
}

// ── SD-Schreiben (immer unter g_sdMutex aufrufen!) ────────────
// B-14: Öffnet die Tagesdatei EINMAL pro Batch (statt 60× open/close pro
// Zeile) — verkürzt die g_sdMutex-Haltezeit drastisch, sodass ein parallel
// laufender Web-SD-Zugriff den Flush nicht mehr in den Timeout treibt.
// Rückgabe false = Öffnen fehlgeschlagen (→ Aufrufer setzt s_sd_ok=false).
static bool write_batch_to_sd(const LogEntry* rows, uint8_t n) {
    if (!n) return true;
    bool     ok     = true;
    uint32_t curDay = 0xFFFFFFFFu;
    File     f;
    for (uint8_t k = 0; k < n; k++) {
        uint32_t day = rows[k].timestamp / 86400;
        if (day != curDay) {
            if (f) f.close();
            char fname[24];
            // Dateiname nach UTC-Tag (Storage rein UTC, DST-unabhängig stabil).
            snprintf(fname, sizeof(fname), "/log_%05lu.csv", (unsigned long)day);
            bool new_file = !SD.exists(fname);
            f = SD.open(fname, FILE_APPEND);
            if (!f) { Serial.printf("[LOG] SD Schreib-Fehler: %s\n", fname); ok = false; break; }
            if (new_file) f.print(CSV_HEADER);
            curDay = day;
        }
        f.print(logger_entry_to_csv(rows[k]));
    }
    if (f) f.close();
    if (ok) Serial.printf("[LOG] SD-Batch: %u Minutenzeilen\n", n);
    return ok;
}

// ── Init ─────────────────────────────────────────────────────
void logger_init() {
    g_sdMutex = xSemaphoreCreateMutex();
    s_mutex   = xSemaphoreCreateMutex();

    s_buf = (LogEntry*)ps_malloc((size_t)LOG_BUFFER_SIZE * sizeof(LogEntry));
    if (!s_buf) {
        Serial.println("[LOG] FEHLER: PSRAM-Allokierung fehlgeschlagen!");
        Serial.printf ("[LOG] Benötigt: %u Byte\n",
                       (unsigned)(LOG_BUFFER_SIZE * sizeof(LogEntry)));
    } else {
        memset(s_buf, 0, (size_t)LOG_BUFFER_SIZE * sizeof(LogEntry));
        Serial.printf("[LOG] PSRAM: %u × %u Byte = %.1f MB (48h @ 2s)\n",
            LOG_BUFFER_SIZE, (unsigned)sizeof(LogEntry),
            (float)(LOG_BUFFER_SIZE * sizeof(LogEntry)) / 1048576.0f);
    }

    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        sdSPI.begin(SPI_SD_CLK, SPI_SD_MISO, SPI_SD_MOSI, SPI_SD_CS);
        if (SD.begin(SPI_SD_CS, sdSPI)) {        
            s_sd_ok = true;
            Serial.printf("[LOG] SD-Karte OK - %lluMB\n", SD.cardSize() / (1024*1024));
        } else {
            Serial.println("[LOG] WARNUNG: SD-Karte nicht gefunden - nur RAM-Logging");
        }
        xSemaphoreGive(g_sdMutex);
    }

    memset(&s_min, 0, sizeof(s_min));
    s_rowCount     = 0;
    s_lastMinuteMs = millis();
    s_lastFlushMs  = millis();
}

// B-15: SD zur Laufzeit neu einhängen (Karte gezogen/Wackler/voll).
// Nur aus logger_task aufrufen. Hält g_sdMutex kurz.
static bool sd_try_remount() {
    bool ok = false;
    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        SD.end();
        ok = SD.begin(SPI_SD_CS, sdSPI);
        s_sd_ok = ok;
        xSemaphoreGive(g_sdMutex);
    }
    Serial.println(ok ? "[LOG] SD-Remount OK" : "[LOG] SD-Remount fehlgeschlagen");
    return ok;
}

bool logger_sd_available() { return s_sd_ok; }

// ── 2s-Rohwert: Ringpuffer + Minuten-Akku ────────────────────
void logger_append() {
    if (!s_buf) return;

    LogEntry e = {};
    e.timestamp = clock_now();

    if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        e.bmsVoltage  = (uint16_t)(g_bms.totalVoltage * 100.0f);
        e.bmsCurrent  = (int16_t) (g_bms.current      * 10.0f);
        e.soc         = g_bms.soc;
        e.tempTube    = (int8_t)g_bms.tempMOS;   // 0x80 MOS-Temp (tempTube=LogEntry-Feld)
        e.tempSensor1 = (int8_t)g_bms.tempSensor1;
        e.tempSensor2 = (int8_t)g_bms.tempSensor2;
        e.tempMOS     = (int8_t)g_bms.tempMOS;
        e.cellMinMV   = 0;   // Zellspannungen in v5.0 nicht mehr erfasst
        e.cellMaxMV   = 0;   // (LogEntry-Felder bleiben für Struct-Größe 32B)
        e.remainAh10  = (uint16_t)(g_bms.remainingCapacityAh * 10.0f);
        xSemaphoreGive(g_bmsMutex);
    }

    if (xSemaphoreTake(g_mpptMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        e.pvPower      = g_mppt.panelPower;
        e.pvVoltage10  = (uint16_t)(g_mppt.panelVoltage  * 10.0f);
        e.pvCurrent100 = (uint16_t)(g_mppt.chargeCurrent * 100.0f);
        e.mpptState    = g_mppt.chargeState;
        e.mpptError    = g_mppt.errorCode;
        xSemaphoreGive(g_mpptMutex);
    }

    e.flags  = (g_io.landstrom  ? 0x01 : 0);
    e.flags |= (g_io.relayDPlus ? 0x02 : 0);
    e.flags |= (g_io.mosfetGel  ? 0x04 : 0);
    e.flags |= (g_io.wrRemote   ? 0x08 : 0);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_buf[s_head] = e;
        s_head = (s_head + 1) % LOG_BUFFER_SIZE;
        if (s_count < LOG_BUFFER_SIZE) s_count++;
        g_log_head  = s_head;
        g_log_count = s_count;

        s_min.v     += e.bmsVoltage;
        s_min.i     += e.bmsCurrent;
        s_min.soc   += e.soc;
        s_min.tt    += e.tempTube;
        s_min.s1    += e.tempSensor1;
        s_min.s2    += e.tempSensor2;
        s_min.mos   += e.tempMOS;
        s_min.cvmin += e.cellMinMV;
        s_min.cvmax += e.cellMaxMV;
        s_min.rem   += e.remainAh10;
        s_min.ppv   += e.pvPower;
        s_min.vpv   += e.pvVoltage10;
        s_min.cpv   += e.pvCurrent100;
        s_min.mpptState = e.mpptState;
        s_min.mpptError = e.mpptError;
        s_min.flags     = e.flags;
        s_min.lastTs    = e.timestamp;
        s_min.count++;

        if ((uint32_t)(millis() - s_lastMinuteMs) >= 60000)
            finalize_minute_locked();

        xSemaphoreGive(s_mutex);
    }
}

// ── SD-Batch schreiben ────────────────────────────────────────
void logger_flush_sd() {
    clock_persist();

    // B-15: SD zur Laufzeit weg? Alle 60 s Remount versuchen, sonst raus.
    if (!s_sd_ok) {
        if ((uint32_t)(millis() - s_sdRetryMs) >= 60000) {
            s_sdRetryMs = millis();
            sd_try_remount();
        }
        return;
    }

    bool due  = (uint32_t)(millis() - s_lastFlushMs) >= g_params.logIntervalMs;
    bool full = (s_rowCount >= MIN_ROWS_MAX);
    if (!due && !full) return;

    uint8_t  n = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    n = s_rowCount;
    if (n) memcpy(s_flushBuf, s_rows, n * sizeof(LogEntry));   // K-4: Off-Stack
    s_rowCount = 0;
    xSemaphoreGive(s_mutex);

    s_lastFlushMs = millis();
    if (n == 0) return;

    // SD-Mutex für den ganzen Batch halten (write_batch_to_sd braucht es).
    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        bool ok = write_batch_to_sd(s_flushBuf, n);
        xSemaphoreGive(g_sdMutex);
        if (!ok) {                       // B-15: Schreibfehler → Remount anstoßen
            s_sd_ok     = false;
            s_sdRetryMs = millis();
        }
    } else {
        Serial.println("[LOG] SD-Mutex Timeout — Batch verworfen");
    }
}

// ── Notfallsicherung (Watchdog vor Reboot) ───────────────────
void logger_emergency_back_up(const char* reason) {
    clock_persist(true);
    if (!s_sd_ok) {
        Serial.println("[LOG] Notfallsicherung: keine SD-Karte");
        return;
    }

    uint8_t  n = 0;

    // Timeout-Variante: Bei Mutex-Blockade trotzdem fortfahren.
    // K-4: s_emergBuf (Off-Stack), eigener Puffer nur für diesen wdt_task-Pfad.
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        finalize_minute_locked();
        n = s_rowCount;
        if (n) memcpy(s_emergBuf, s_rows, n * sizeof(LogEntry));
        s_rowCount = 0;
        xSemaphoreGive(s_mutex);
    } else {
        // Mutex nicht bekommen — rohen Zeiger-Zustand lesen (best-effort)
        n = s_rowCount;
        if (n > MIN_ROWS_MAX) n = MIN_ROWS_MAX;
        if (n) memcpy(s_emergBuf, s_rows, n * sizeof(LogEntry));
        s_rowCount = 0;
    }

    // SD-Mutex mit Timeout — notfalls proceed anyway (Watchdog läuft ab!)
    bool sd_ok = (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(500)) == pdTRUE);
    if (n) write_batch_to_sd(s_emergBuf, n);
    File f = SD.open("/crash.log", FILE_APPEND);
    if (f) {
        f.printf("ts=%lu reason=%s rows=%u\n",
                 (unsigned long)clock_now(), reason ? reason : "?", n);
        f.close();
    }
    if (sd_ok) xSemaphoreGive(g_sdMutex);
    Serial.printf("[LOG] Notfallsicherung: %u Zeilen + Marker (%s)\n",
                  n, reason ? reason : "?");
}

// ── Snapshot-API für Webserver-Streaming ─────────────────────
// Kurzer Lock: Indizes + Anzahl lesen, dann Einträge kopieren.
uint32_t logger_snapshot(uint32_t offset, uint32_t count,
                         uint32_t step, LogEntry* out) {
    if (!s_buf || !out || count == 0 || step == 0) return 0;

    uint32_t emitted = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

    uint32_t available = (s_count < LOG_BUFFER_SIZE) ? s_count : LOG_BUFFER_SIZE;
    if (offset < available) {
        for (uint32_t i = 0; emitted < count; i++) {
            uint64_t back = (uint64_t)offset + (uint64_t)i * step;
            if (back >= available) break;
            uint32_t idx = (uint32_t)((s_head + LOG_BUFFER_SIZE - 1 - (uint32_t)back)
                                      % LOG_BUFFER_SIZE);
            out[emitted++] = s_buf[idx];
        }
    }
    xSemaphoreGive(s_mutex);
    return emitted;
}

// ── CSV-Serialisierung ────────────────────────────────────────
String logger_entry_to_csv(const LogEntry& e) {
    char buf[160];
    snprintf(buf, sizeof(buf),
        "%lu,%.2f,%.1f,%d,%d,%d,%d,%d,"
        "%.3f,%.3f,%.1f,"
        "%d,%.1f,%.2f,"
        "%d,%d,"
        "%d,%d,%d,%d\n",
        (unsigned long)e.timestamp,
        e.bmsVoltage  / 100.0f,
        e.bmsCurrent  / 10.0f,
        e.soc,
        e.tempTube, e.tempSensor1, e.tempSensor2, e.tempMOS,
        e.cellMinMV   / 1000.0f,
        e.cellMaxMV   / 1000.0f,
        e.remainAh10  / 10.0f,
        e.pvPower,
        e.pvVoltage10  / 10.0f,
        e.pvCurrent100 / 100.0f,
        e.mpptState, e.mpptError,
        (e.flags & 0x01)?1:0, (e.flags & 0x02)?1:0,
        (e.flags & 0x04)?1:0, (e.flags & 0x08)?1:0);
    return String(buf);
}
