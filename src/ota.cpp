// ============================================================
//  ota.cpp — Womo Energy Core v5.5.1
//  Web-OTA: Firmware- und Dashboard-Update per Browser-Upload
//
//  Konzeption:
//   • Arduino <Update.h> übernimmt Partitionswahl, Erase, Write
//     und das Umschalten der otadata-Boot-Auswahl.
//   • type=fw : U_FLASH  → inaktive App-Partition (ota_0/ota_1).
//     Magic-Byte-Prüfung (0xE9) am ersten Byte verhindert das
//     versehentliche Flashen von littlefs.bin als Firmware.
//   • type=fs : U_SPIFFS → spiffs-Partition (LittleFS-Image).
//     LittleFS wird VOR dem Schreiben ausgehängt (LittleFS.end()),
//     da die Partition darunter komplett überschrieben wird.
//     Schlägt das fs-Update fehl, bleibt die im Browser geladene
//     Seite funktionsfähig → Upload einfach wiederholen.
//   • Kein Neustart im AsyncTCP-Kontext: ota_handle_request setzt
//     nur s_rebootAtMs; ota_tick() (loop, Core 1) sichert den
//     Ringpuffer und ruft dann ESP.restart().
//
//  Zustandshaltung (Lehren aus dem v5.4-Review angewandt):
//   • Per-Request-Zustand (OtaReqState) liegt in req->_tempObject.
//     WICHTIG: MUSS heap-alloziert sein — der Request-Destruktor
//     ruft free(_tempObject) (verifiziert im esp32async-Fork
//     v3.7.0, WebRequest.cpp Z.98). Dadurch kein Leak, auch wenn
//     onRequest nie läuft (Client-Abbruch), und keine Interferenz
//     zwischen zwei zeitgleich eintreffenden Uploads.
//   • Global nur: Claim (s_inProgress + s_activeReq, unter
//     Spinlock, AsyncTCP-Task vs. loop) und s_rebootAtMs
//     (volatile uint32_t, 32-bit-atomar auf Xtensa).
//   • onDisconnect feuert bei JEDEM TCP-Trennen — auch nach
//     erfolgreicher Antwort. Abgebrochen wird daher nur, wenn
//     dieser Request den Claim noch hält.
// ============================================================
#include "ota.h"
#include "config.h"
#include "logger.h"          // logger_emergency_back_up()
#include <Update.h>
#include <LittleFS.h>
#include <esp_ota_ops.h>

// ── Per-Request-Zustand (liegt in req->_tempObject) ───────────
// Destruktor des Requests gibt den Speicher per free() frei.
struct OtaReqState {
    bool   claimed;      // dieser Request hat den globalen Claim erhalten
    bool   success;      // Update.end() erfolgreich
    int    cmd;          // U_FLASH | U_SPIFFS
    size_t written;
    char   err[100];     // leer = kein Fehler
};

// ── Globaler Claim: genau EIN Upload gleichzeitig ─────────────
static portMUX_TYPE s_otaMux = portMUX_INITIALIZER_UNLOCKED;
static bool                    s_inProgress = false;
static AsyncWebServerRequest*  s_activeReq  = nullptr;  // nur Vergleich, nie Dereferenz außerhalb Callbacks
static volatile uint32_t       s_rebootAtMs = 0;        // 0 = kein Neustart geplant

static OtaReqState* req_state(AsyncWebServerRequest* req, bool create) {
    if (!req->_tempObject && create)
        req->_tempObject = calloc(1, sizeof(OtaReqState));
    return (OtaReqState*)req->_tempObject;
}

static void release_claim(AsyncWebServerRequest* req) {
    portENTER_CRITICAL(&s_otaMux);
    if (s_activeReq == req) { s_inProgress = false; s_activeReq = nullptr; }
    portEXIT_CRITICAL(&s_otaMux);
}

// ── Init: Boot-Diagnose ───────────────────────────────────────
void ota_init() {
    const esp_partition_t* run  = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (run)
        Serial.printf("[OTA] Laufende Partition: %s @ 0x%06x\n",
                      run->label, (unsigned)run->address);
    if (next)
        Serial.printf("[OTA] Update-Ziel: %s @ 0x%06x (%u kB)\n",
                      next->label, (unsigned)next->address,
                      (unsigned)(next->size / 1024));
    else
        Serial.println("[OTA] WARNUNG: Keine OTA-Partition — "
                       "Partitionstabelle prüfen (ota_0/ota_1/otadata)!");
}

// ── GET /api/ota ──────────────────────────────────────────────
String ota_to_json() {
    const esp_partition_t* run  = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"partition\":\"%s\",\"next\":\"%s\","
        "\"app_max\":%u,\"fs_total\":%u,\"fs_used\":%u}",
        FW_VERSION,
        run  ? run->label  : "?",
        next ? next->label : "-",
        next ? (unsigned)next->size : 0,
        (unsigned)LittleFS.totalBytes(),
        (unsigned)LittleFS.usedBytes());
    return String(buf);
}

// ── Upload-Callback (AsyncTCP-Task, chunked) ──────────────────
void ota_handle_upload(AsyncWebServerRequest* req, String filename,
                       size_t index, uint8_t* data, size_t len, bool final) {
    OtaReqState* st = req_state(req, index == 0);
    if (!st) return;   // OOM bzw. Chunk ohne Startzustand → onRequest antwortet generisch

    if (index == 0) {
        // ── Claim: genau ein Upload gleichzeitig (Test-and-Set) ──
        bool busy;
        portENTER_CRITICAL(&s_otaMux);
        busy = s_inProgress;
        if (!busy) { s_inProgress = true; s_activeReq = req; }
        portEXIT_CRITICAL(&s_otaMux);
        if (busy || s_rebootAtMs != 0) {
            snprintf(st->err, sizeof(st->err), "Update läuft bereits");
            return;
        }
        st->claimed = true;

        // Typ aus Query-Parameter: fw (Default) | fs
        String type = req->hasParam("type") ? req->getParam("type")->value() : "fw";
        st->cmd = (type == "fs") ? U_SPIFFS : U_FLASH;

        // Plausibilität: Firmware-Images beginnen mit ESP-Magic 0xE9.
        // Schützt vor vertauschten Dateien (littlefs.bin als Firmware
        // würde sonst eine nicht bootende App flashen).
        if (st->cmd == U_FLASH && (len == 0 || data[0] != 0xE9)) {
            snprintf(st->err, sizeof(st->err),
                     "Keine ESP32-Firmware (Magic-Byte fehlt) — falsche Datei?");
            release_claim(req);
            return;
        }

        // LittleFS vor dem Überschreiben der Partition aushängen.
        // serveStatic liefert danach bis zum Neustart nichts mehr —
        // die bereits geladene Dashboard-Seite läuft aber weiter.
        if (st->cmd == U_SPIFFS) LittleFS.end();

        Serial.printf("[OTA] Start: %s (%s)\n", filename.c_str(),
                      st->cmd == U_SPIFFS ? "Dashboard/LittleFS" : "Firmware");

        // Content-Length enthält Multipart-Overhead → Größe unbekannt.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, st->cmd)) {
            snprintf(st->err, sizeof(st->err), "Update.begin: %s", Update.errorString());
            release_claim(req);
            Serial.printf("[OTA] FEHLER: %s\n", st->err);
            return;
        }

        // Client-Abbruch (WLAN weg, Tab zu): Update sauber verwerfen,
        // sonst bliebe der Claim dauerhaft gesetzt. Feuert auch nach
        // ERFOLGREICHER Antwort (normales TCP-Close) → nur abbrechen,
        // wenn dieser Request den Claim noch hält.
        req->onDisconnect([req]() {
            bool held;
            portENTER_CRITICAL(&s_otaMux);
            held = s_inProgress && (s_activeReq == req);
            if (held) { s_inProgress = false; s_activeReq = nullptr; }
            portEXIT_CRITICAL(&s_otaMux);
            if (held) {
                Update.abort();
                Serial.println("[OTA] Abbruch: Client getrennt — Update verworfen");
            }
        });
    }

    // Abgelehnter/fehlgeschlagener Request → restliche Chunks ignorieren
    if (st->err[0]) return;
    // Sicherheitsnetz: nur der Claim-Inhaber schreibt
    if (!st->claimed || s_activeReq != req) return;

    if (len) {
        if (Update.write(data, len) != len) {
            snprintf(st->err, sizeof(st->err), "Update.write: %s", Update.errorString());
            Update.abort();
            release_claim(req);
            Serial.printf("[OTA] FEHLER: %s\n", st->err);
            return;
        }
        st->written += len;
        if ((st->written & 0x1FFFF) < len)   // ~alle 128 kB loggen
            Serial.printf("[OTA] %u kB geschrieben\n", (unsigned)(st->written / 1024));
    }

    if (final) {
        if (Update.end(true)) {              // true: Größe = bisher geschrieben
            st->success = true;
            Serial.printf("[OTA] Fertig: %u kB — Neustart folgt\n",
                          (unsigned)(st->written / 1024));
        } else {
            snprintf(st->err, sizeof(st->err), "Update.end: %s", Update.errorString());
            Serial.printf("[OTA] FEHLER: %s\n", st->err);
        }
        release_claim(req);
    }
}

// ── onRequest-Callback (nach Upload-Abschluss) ────────────────
void ota_handle_request(AsyncWebServerRequest* req) {
    OtaReqState* st = req_state(req, false);
    if (!st) {
        // POST ohne Datei-Upload (leerer Body) oder OOM beim Zustand
        req->send(400, "application/json",
                  "{\"error\":\"Keine Datei empfangen\"}");
        return;
    }
    if (st->success) {
        req->send(200, "application/json",
                  "{\"ok\":true,\"reboot\":true,\"written\":" +
                  String((unsigned)st->written) + "}");
        s_rebootAtMs = millis() + 1500;      // Antwort erst ausliefern lassen
    } else {
        // JSON-Sonderzeichen im Fehlertext escapen (minimal: \ und ")
        String e = st->err[0] ? String(st->err) : String("Update fehlgeschlagen");
        e.replace("\\", "\\\\");
        e.replace("\"", "\\\"");
        req->send(400, "application/json", "{\"error\":\"" + e + "\"}");
    }
    // st NICHT freigeben: der Request-Destruktor ruft free(_tempObject).
}

// ── Deferred-Reboot (loop-Kontext, Core 1) ────────────────────
void ota_tick() {
    if (s_rebootAtMs == 0) return;
    if ((int32_t)(millis() - s_rebootAtMs) < 0) return;
    s_rebootAtMs = 0;
    Serial.println("[OTA] Neustart — Ringpuffer wird gesichert …");
    Serial.flush();
    logger_emergency_back_up("OTA_REBOOT");   // Datenverlust vermeiden
    ESP.restart();
}
