// ============================================================
//  http_server.cpp — Womo Energy Core v5.6.1
//  v5.6.1: webserver_buffer_json() — Ringpuffer-Array-Builder als
//  gemeinsamer Pfad HTTP+BLE (handle_buffer nutzt ihn jetzt auch).
//  v5.6.0: BLE-Anbindung — Broadcast an WS+BLE (ein JSON-Build),
//  ble_tick() im ws_task-Kontext, /api/ble (Schalter), "type":"live".
//
//  v5.5.3: NTP-Sync-Status im Live-JSON (net.ntp = UTC-Epoch des
//          letzten erfolgreichen NTP-Syncs, 0 = nie) für die
//          Dashboard-Anzeige im Zeitzone-Tab.
//  v5.5.2: mDNS (womo.local, AP+STA) + NTP-Zeitsync über STA.
//          NTP nutzt die rohe esp_sntp-API (IDF 4.4) — KEIN configTime(),
//          damit der POSIX-TZ des clock-Moduls unangetastet bleibt. Der
//          SNTP-Callback setzt nur ein Flag; gestellt wird im ws_task
//          über clock_set_epoch() (UTC) — derselbe Pfad wie der
//          Browser-Sync, inkl. dessen 5-s-Hysterese. mDNS/NTP werden
//          per Flag aus on_wifi_event heraus im ws_task ausgelöst
//          (gleiche Kontext-Regel wie s_wifiReapply).
//  v5.4.1: Web-OTA (/api/ota GET+POST) — Logik im Modul ota.cpp.
//  v5.0: 12 Parameter (socDPlusHigh/socGelHigh neu,
//  socGelOff/pvWRThreshold* entfernt).
//  Alle anderen Fixes aus v4.3 bleiben erhalten.
// ============================================================
#include "http_server.h"
#include "config.h"
#include "params.h"
#include "bms.h"
#include "mppt.h"
#include "io.h"
#include "logic.h"
#include "logger.h"
#include "inverter.h"
#include "clock.h"
#include "level.h"
#include "ota.h"
#include "ble.h"       // v5.6.0: BLE-Broadcast + /api/ble
#include <WiFi.h>
#include <ESPmDNS.h>    // v5.5.2: womo.local (Core-Bibliothek, kein lib_dep)
#include "esp_sntp.h"   // v5.5.2: NTP über rohe SNTP-API (IDF 4.4)
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <Preferences.h>
#include <memory>       // std::shared_ptr — Lebensdauer der Chunked-Stream-Puffer (H-3)

static AsyncWebServer server(WEBSERVER_PORT);
static AsyncWebSocket ws("/ws");

// ── Heim-WLAN (STA, v5.5.1: bis zu 3 Netze) ──────────────────
// Credentials liegen im NVS-Namespace "wifi" (getrennt von clock/params),
// Schlüssel ssid1/pass1 … ssid3/pass3. Passwort wird per /api/wifi NIE
// zurückgegeben. Bei mehr als einem konfigurierten Netz wählt ein
// asynchroner Scan das STÄRKSTE bekannte Netz (Auswertung in wifi_tick,
// ws_task-Kontext — vollständig non-blocking, AP bleibt aktiv). Bei
// genau einem Netz: Direktverbindung wie bisher (kein Scan). Bewusst
// KEINE Arduino-WiFiMulti-Klasse — deren blockierendes run() verträgt
// sich nicht mit AP+STA-Dualmodus und unserer Reconnect-Strategie.
#define WIFI_STA_SLOTS              3
#define WIFI_RESCAN_INTERVAL_MS     60000    // Rescan-Backoff-Basis wenn getrennt
#define WIFI_RESCAN_INTERVAL_MAX_MS 900000   // Backoff-Deckel: max. alle 15 Min (v5.6.6)
#define WIFI_AP_SCAN_DEFER_MS       5000     // Scan-Aufschub, solange AP-Client(s) verbunden (v5.6.6)
static Preferences  wprefs;
static bool         s_staEnabled = false;   // mind. ein Slot konfiguriert?
// POST setzt nur dieses Flag; die eigentliche WiFi-Rekonfiguration läuft
// im ws_task-Kontext (webserver_broadcast), NICHT im AsyncTCP-Handler.
static volatile bool s_wifiReapply = false;
// Scan-Zustandsmaschine — ausschließlich ws_task schreibt/liest:
enum StaScanState : uint8_t { STA_IDLE = 0, STA_SCANNING };
static StaScanState s_staScan          = STA_IDLE;
static uint32_t     s_staNextScan      = 0;   // millis()-Marke für nächsten Rescan
// v5.6.6: wächst bei erfolglosem Scan (kein bekanntes Netz gefunden) bis
// WIFI_RESCAN_INTERVAL_MAX_MS, damit dauerhaft abwesende Heimnetze nicht
// minütlich per Aktiv-Scan das AP-Radio stören (siehe wifi_tick()).
static uint32_t     s_staRescanIntervalMs = WIFI_RESCAN_INTERVAL_MS;

// ── mDNS / NTP (v5.5.2) ──────────────────────────────────────
// on_wifi_event (WiFi-Event-Task) darf kein I2C/keine blockierenden Ops
// ausführen → es setzt nur s_staGotIp. Auswertung (mDNS-Reannounce +
// NTP-Start) erfolgt in webserver_broadcast (ws_task, Core 0). Der
// SNTP-Callback läuft im lwip/SNTP-Task und setzt nur s_ntpSynced; das
// eigentliche Stellen der Uhr (clock_set_epoch → DS3231/I2C) passiert
// ebenfalls im ws_task.
static volatile bool s_staGotIp = false;   // STA-GOT-IP signalisiert
static volatile bool s_ntpSynced = false;  // SNTP hat frische Zeit geliefert
static bool          s_sntpInit  = false;  // SNTP genau einmal initialisiert
static uint32_t      s_ntpLastSync = 0;    // v5.5.3: UTC-Epoch letzter NTP-Sync (0 = nie)

static String slot_key (const char* base, uint8_t i) {
    char k[8]; snprintf(k, sizeof(k), "%s%u", base, (unsigned)(i + 1)); return String(k);
}
static String slot_ssid(uint8_t i) { return wprefs.getString(slot_key("ssid", i).c_str(), ""); }
static String slot_pass(uint8_t i) { return wprefs.getString(slot_key("pass", i).c_str(), ""); }
static uint8_t slots_configured() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < WIFI_STA_SLOTS; i++) if (slot_ssid(i).length()) n++;
    return n;
}

static void on_wifi_event(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[WEB] STA verbunden: %s  (RSSI %d dBm, Kanal %d)\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.RSSI(), WiFi.channel());
            // v5.5.2: mDNS-Reannounce + NTP-Start deferred im ws_task,
            // NICHT hier (Event-Task, kein I2C/Blocking).
            s_staGotIp = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // Normalfall, wenn das WoMo nicht zu Hause steht — AP läuft weiter.
            Serial.println("[WEB] STA getrennt — AP weiter aktiv, Reconnect aktiv");
            break;
        default: break;
    }
}

// Startet (oder beendet) den STA-Teil anhand der konfigurierten Slots.
// Non-blocking: weder WiFi.begin() noch der asynchrone Scan blockieren
// Boot/Webserver. 0 Slots → STA aus. 1 Slot → Direktverbindung
// (AutoReconnect, exakt das Verhalten bis v5.5). >1 Slot → Scan
// anstoßen; die Auswahl trifft wifi_tick() nach Scan-Ende.
static void wifi_apply_sta() {
    uint8_t n = slots_configured();
    s_staEnabled = (n > 0);
    s_staScan    = STA_IDLE;
    if (!s_staEnabled) {
        // Alle Slots leer → STA abschalten, AP bleibt bestehen (eraseap=true).
        WiFi.disconnect(false, true);
        Serial.println("[WEB] STA deaktiviert (keine SSID konfiguriert)");
        return;
    }
    if (n == 1) {
        for (uint8_t i = 0; i < WIFI_STA_SLOTS; i++) {
            String ssid = slot_ssid(i);
            if (!ssid.length()) continue;
            WiFi.setAutoReconnect(true);
            WiFi.begin(ssid.c_str(), slot_pass(i).c_str());   // non-blocking
            Serial.printf("[WEB] STA: suche \"%s\" (Slot %u) ...\n",
                          ssid.c_str(), (unsigned)(i + 1));
            break;
        }
        return;
    }
    // Mehrere Kandidaten → asynchroner Scan (AP bleibt aktiv).
    // Neu-Konfiguration (Nutzer hat gerade gespeichert) setzt den Backoff
    // zurück — der erste Scan nach einer Änderung soll nicht künstlich
    // verzögert sein.
    s_staRescanIntervalMs = WIFI_RESCAN_INTERVAL_MS;
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, false /*keine Hidden-SSIDs*/);
    s_staScan     = STA_SCANNING;
    s_staNextScan = millis() + s_staRescanIntervalMs;
    Serial.printf("[WEB] STA: %u Netze konfiguriert — Scan läuft ...\n", (unsigned)n);
}

// v5.5.1: Scan auswerten + Rescan-Backoff. NUR aus webserver_broadcast
// (ws_task) aufrufen — gleiche Kontext-Regel wie wifi_apply_sta.
// v5.6.6: Backoff wächst exponentiell (Basis WIFI_RESCAN_INTERVAL_MS,
// Deckel WIFI_RESCAN_INTERVAL_MAX_MS) solange kein bekanntes Netz
// gefunden wird, und der Scan wird zurückgestellt, solange AP-Clients
// verbunden sind (Kanal-Scan stört sonst deren Verbindung — Symptom war
// ein "instabiles WLAN", wenn dauerhaft kein Heimnetz erreichbar war).
static void wifi_tick() {
    if (!s_staEnabled) return;

    if (s_staScan == STA_SCANNING) {
        int16_t r = WiFi.scanComplete();
        if (r == WIFI_SCAN_RUNNING) return;          // noch nicht fertig
        int8_t  best = -1; int32_t bestRssi = -128;
        String  bestSsid, bestPass;
        for (int16_t k = 0; k < r; k++) {            // r<=0 → Schleife leer
            String found = WiFi.SSID(k);
            if (!found.length()) continue;
            for (uint8_t i = 0; i < WIFI_STA_SLOTS; i++) {
                if (found == slot_ssid(i) && WiFi.RSSI(k) > bestRssi) {
                    bestRssi = WiFi.RSSI(k);
                    bestSsid = found; bestPass = slot_pass(i); best = (int8_t)i;
                }
            }
        }
        WiFi.scanDelete();
        s_staScan = STA_IDLE;
        if (best >= 0) {
            // Bekanntes Netz gefunden → Backoff zurücksetzen, damit ein
            // späteres Verschwinden des Netzes wieder mit dem schnellen
            // Basis-Intervall neu versucht wird.
            s_staRescanIntervalMs = WIFI_RESCAN_INTERVAL_MS;
            WiFi.setAutoReconnect(true);
            WiFi.begin(bestSsid.c_str(), bestPass.c_str());
            Serial.printf("[WEB] STA: stärkstes bekanntes Netz \"%s\" (%d dBm, Slot %u)\n",
                          bestSsid.c_str(), (int)bestRssi, (unsigned)(best + 1));
        } else {
            // v5.6.6: kein bekanntes Netz in Reichweite → Backoff verdoppeln
            // (Deckel WIFI_RESCAN_INTERVAL_MAX_MS). Verhindert, dass ein
            // dauerhaft abwesendes Heimnetz das AP-Radio minütlich für den
            // Kanal-Scan verlässt und damit verbundene Dashboard-Clients
            // stört.
            s_staRescanIntervalMs = min(s_staRescanIntervalMs * 2,
                                         (uint32_t)WIFI_RESCAN_INTERVAL_MAX_MS);
            Serial.printf("[WEB] STA: kein bekanntes Netz in Reichweite — nächster Scan in %us\n",
                          (unsigned)(s_staRescanIntervalMs / 1000));
        }
        s_staNextScan = millis() + s_staRescanIntervalMs;
        return;
    }

    // Getrennt + mehrere Kandidaten → periodischer Rescan (Heimkehr/
    // Roaming zwischen bekannten Netzen). Bei genau einem Slot erledigt
    // das der Arduino-AutoReconnect wie bisher.
    if (!WiFi.isConnected() && slots_configured() > 1 &&
        (int32_t)(millis() - s_staNextScan) >= 0) {
        // v5.6.6: Aktiver Scan zwingt das (einzige) Radio kurz vom
        // AP-Kanal weg → verbundene AP-Clients (Dashboard/Handy) verlieren
        // dabei kurz die Verbindung. Solange jemand am AP hängt, Scan
        // zurückstellen statt die Session zu stören; Backoff-Timer bleibt
        // dabei unverändert, es wird nur in kurzen Abständen erneut
        // geprüft, ob der AP inzwischen wieder frei ist.
        if (WiFi.softAPgetStationNum() > 0) {
            s_staNextScan = millis() + WIFI_AP_SCAN_DEFER_MS;
            return;
        }
        WiFi.scanDelete();
        WiFi.scanNetworks(true, false);
        s_staScan     = STA_SCANNING;
        s_staNextScan = millis() + s_staRescanIntervalMs;
    }
}

// ── mDNS (v5.5.2) ────────────────────────────────────────────
// (Neu-)Ankündigung von "<MDNS_HOSTNAME>.local" mit HTTP-Dienst auf dem
// Webserver-Port. Idempotent: MDNS.end() vor jedem begin(), damit der
// Name nach einer STA-Verbindung sauber auf beiden Interfaces (AP+STA)
// neu propagiert wird. Nur aus ws_task/webserver_init aufrufen.
static void mdns_announce() {
    MDNS.end();
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", WEBSERVER_PORT);
        Serial.printf("[MDNS] http://%s.local aktiv\n", MDNS_HOSTNAME);
    } else {
        Serial.println("[MDNS] Start fehlgeschlagen");
    }
}

// ── NTP (v5.5.2) ─────────────────────────────────────────────
// Callback aus dem lwip/SNTP-Task: KEIN I2C, kein clock_set_epoch hier —
// nur Flag setzen. SNTP hat zu diesem Zeitpunkt die System-UTC-Uhr
// (settimeofday) bereits gestellt; das Übertragen in die DS3231-geführte
// Zeitbasis erledigt der ws_task.
static void ntp_time_cb(struct timeval* /*tv*/) {
    s_ntpSynced = true;
}

// SNTP starten (genau einmal) bzw. bei erneuter STA-Verbindung frischen
// Sync anstoßen. POLL-Modus pollt danach selbstständig periodisch —
// jeder erfolgreiche Poll feuert ntp_time_cb → Re-Sync ohne eigenen
// Timer. Nur aus ws_task aufrufen. KEIN configTime() (TZ-Schutz).
static void ntp_start() {
    if (s_sntpInit) { sntp_restart(); return; }
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, NTP_SERVER);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);   // sofort stellen, nicht glätten
    sntp_set_time_sync_notification_cb(ntp_time_cb);
    sntp_init();
    s_sntpInit = true;
    Serial.printf("[NTP] SNTP gestartet (Server %s)\n", NTP_SERVER);
}

static void on_ws_event(AsyncWebSocket* s, AsyncWebSocketClient* c,
                        AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT || type == WS_EVT_DISCONNECT)
        ws.cleanupClients();
}

static String build_live_json() {
    char hdr[160];
    String tzab = clock_tz_abbr();
    // v5.6.0: "type":"live" — Frame-Unterscheidung für BLE-Clients
    // (resp/params/live auf einem Kanal). WS-Dashboard ignoriert das Feld.
    snprintf(hdr, sizeof(hdr),
        "{\"type\":\"live\",\"ts\":%lu,\"epoch\":%lu,\"epoch_mez\":%lu,\"tz\":\"%s\",\"synced\":%s,",
        (unsigned long)(millis()/1000),
        (unsigned long)clock_now(),
        (unsigned long)clock_now_local(),
        tzab.c_str(),
        clock_is_synced()?"true":"false");
    String j;
    j.reserve(1200);   // H-4: Reallokationen beim 2s-Broadcast vermeiden
    j = hdr;
    j += "\"bms\":"   + bms_to_json()      + ",";
    j += "\"mppt\":"  + mppt_to_json()     + ",";
    j += "\"io\":"    + io_to_json()       + ",";
    j += "\"logic\":" + logic_status_json()+ ",";
    j += "\"inv\":"   + inverter_to_json() + ",";
    j += "\"sys\":{\"heap\":"     + String(ESP.getFreeHeap())
       + ",\"min_heap\":"         + String(ESP.getMinFreeHeap())
       + ",\"psram\":"            + String(ESP.getFreePsram()) + "},";
    bool staUp = WiFi.isConnected();
    j += "\"net\":{\"sta_en\":"   + String(s_staEnabled?"true":"false")
       + ",\"sta\":"              + String(staUp?"true":"false")
       + ",\"ssid\":\""           + (staUp?WiFi.SSID():String(""))
       + "\",\"ip\":\""           + (staUp?WiFi.localIP().toString():String(""))
       + "\",\"rssi\":"           + String(staUp?WiFi.RSSI():0)
       + ",\"ntp\":"              + String(s_ntpLastSync) + "},";
    j += "\"rtc\":"   + clock_rtc_json() + ",";     
    j += "\"sd\":"    + String(logger_sd_available()?"true":"false") + ",";
    j += "\"buf\":"   + String(g_log_count);
    j += "}";
    return j;
}

// Multi-Chunk POST-Body-Akkumulation
static char* collect_body_chunk(AsyncWebServerRequest* req,
                                uint8_t* data, size_t len,
                                size_t index, size_t total) {
    if (total > 2048) {
        if (index == 0)
            req->send(413, "application/json", "{\"error\":\"Body zu groß\"}");
        return nullptr;
    }
    if (index == 0) {
        if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
        req->_tempObject = malloc(total+1);
        if (!req->_tempObject) { req->send(500,"application/json","{\"error\":\"OOM\"}"); return nullptr; }
    }
    if (!req->_tempObject) return nullptr;
    memcpy((uint8_t*)req->_tempObject + index, data, len);
    if (index + len < total) return nullptr;
    char* body = (char*)req->_tempObject;
    req->_tempObject = nullptr;
    body[total] = '\0';
    return body;
}

// ── H-2: Leerer POST (Content-Length 0) → definierte 400-Antwort ──────
// AsyncWebServer ruft bei leerem Body NUR onRequest (nicht den Body-Callback,
// in dem sonst req->send() steckt) → sonst bliebe der Request unbeantwortet
// hängen (DoS-Vektor). Guard läuft als onRequest ALLER POST-Routen: bei Body
// (contentLength>0) hat der Body-Handler bereits geantwortet → hier No-Op;
// nur bei Content-Length==0 antworten wir. Bewusst über contentLength(),
// NICHT über _tempObject (ist im Erfolgsfall bereits nullptr).
static void post_empty_guard(AsyncWebServerRequest* req) {
    if (req->contentLength() == 0)
        req->send(400, "application/json", "{\"error\":\"leerer Body\"}");
}

// ── H-3: Antwort aus einem PSRAM-Puffer chunked streamen ──────────────
// Ersetzt beginResponseStream() für große Antworten (/api/buffer, /api/sddata):
// beginResponseStream puffert die KOMPLETTE Antwort (~120–440 KB) im internen
// Heap → OOM-Reboot. Hier liegt die fertig serialisierte JSON in PSRAM; der
// Filler kopiert nur ein Fenster [index, index+maxLen) je AsyncTCP-Tick heraus
// (kein großer Internal-RAM-Puffer). Der Puffer wird per shared_ptr an die
// Response gebunden → Freigabe erfolgt automatisch bei Abschluss ODER
// Client-Disconnect (kein Leak). `buf` MUSS mit malloc/ps_malloc alloziert sein.
struct PsStreamBuf {
    char*  data;
    size_t len;
    ~PsStreamBuf() { if (data) free(data); }
};
static void send_psram_json(AsyncWebServerRequest* req, char* buf, size_t len) {
    if (!buf) { req->send(503, "application/json", "{\"error\":\"OOM PSRAM\"}"); return; }
    auto st = std::make_shared<PsStreamBuf>();
    st->data = buf;
    st->len  = len;
    AsyncWebServerResponse* resp = req->beginChunkedResponse("application/json",
        [st](uint8_t* out, size_t maxLen, size_t index) -> size_t {
            if (index >= st->len) return 0;              // fertig
            size_t take = st->len - index;
            if (take > maxLen) take = maxLen;
            memcpy(out, st->data + index, take);
            return take;
        });
    if (!resp) { req->send(503, "application/json", "{\"error\":\"OOM Response\"}"); return; }
    req->send(resp);
}

static void handle_live(AsyncWebServerRequest* req) {
    req->send(200, "application/json", build_live_json());
}

static void handle_params_get(AsyncWebServerRequest* req) {
    req->send(200, "application/json", params_to_json());
}

static void handle_params_post(AsyncWebServerRequest* req, uint8_t* data,
                               size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    // v5.6.0: Anwendung + Validierung im gemeinsamen Pfad mit BLE
    // ({"cmd":"params_set"}) — params_apply_json(), s. params.h.
    // Semantik unverändert (nur bekannte Schlüssel, Setter-Grenzen).
    bool ok = params_apply_json(body);
    free(body);
    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"JSON ungültig oder Wert außerhalb Grenzen\"}");
}

static void handle_reset(AsyncWebServerRequest* req) {
    params_reset();
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── Manueller Aktor-Override (Webinterface, v5.4/v5.5) ────────
// {"actuator":"dplus|gel|wr","mode":"auto|on|off"}
// "auto" schaltet sofort zurück in die Automatik (löscht auch ein
// persistentes AUS). "on": Manuell-EIN — D+/Gel mit Deadman-Timer,
// WR ohne Timer. "off": Manuell-AUS, dauerhaft + NVS-persistent.
// Details: logic_set_manual/logic.cpp (v5.5-Semantik).
static void handle_manual_post(AsyncWebServerRequest* req, uint8_t* data,
                               size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, body)) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    String act  = String((const char*)(doc["actuator"] | ""));
    String mode = String((const char*)(doc["mode"]     | ""));
    free(body);

    ManualActuator a;
    if      (act == "dplus") a = MANUAL_DPLUS;
    else if (act == "gel")   a = MANUAL_GEL;
    else if (act == "wr")    a = MANUAL_WR;
    else { req->send(400, "application/json", "{\"error\":\"unbekannter Aktor\"}"); return; }

    bool ok;
    if      (mode == "auto") ok = logic_set_manual(a, false, false);
    else if (mode == "on")   ok = logic_set_manual(a, true,  true);
    else if (mode == "off")  ok = logic_set_manual(a, true,  false);
    else { req->send(400, "application/json", "{\"error\":\"unbekannter Modus\"}"); return; }

    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"fehlgeschlagen\"}");
}

static void handle_time(AsyncWebServerRequest* req, uint8_t* data,
                        size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, body)) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    uint32_t epoch = doc["epoch"] | 0UL;
    free(body);
    bool ok = clock_set_epoch(epoch);
    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"Epoch unplausibel (< 2024)\"}");
}

static void handle_tz_get(AsyncWebServerRequest* req) {
    char buf[160];
    String tz = clock_tz();
    String ab = clock_tz_abbr();
    snprintf(buf, sizeof(buf),
        "{\"tz\":\"%s\",\"abbr\":\"%s\",\"offset\":%ld}",
        tz.c_str(), ab.c_str(),
        (long)clock_local_offset_at(clock_now()));
    req->send(200, "application/json", buf);
}

static void handle_tz_post(AsyncWebServerRequest* req, uint8_t* data,
                           size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, body)) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    const char* tz = doc["tz"] | "";
    size_t n = strlen(tz);
    bool ok = (n >= 2 && n < 64);          // POSIX-TZ Plausibilität (Länge)
    if (ok) clock_set_tz(tz);
    free(body);
    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"TZ ungültig (2..63 Zeichen)\"}");
}

// ── Heim-WLAN-Status / -Konfiguration (v5.5.1: 3 Slots) ───────
static void handle_wifi_get(AsyncWebServerRequest* req) {
    bool staUp = WiFi.isConnected();
    // Passwörter werden bewusst NICHT ausgeliefert; "set" je Slot zeigt
    // nur, ob eine SSID hinterlegt ist. "ssid" (top-level) = aktuell
    // verbundenes Netz.
    String j = "{\"slots\":[";
    for (uint8_t i = 0; i < WIFI_STA_SLOTS; i++) {
        String ssid = slot_ssid(i);
        if (i) j += ",";
        j += "{\"ssid\":\"" + ssid + "\",\"set\":" + (ssid.length() ? "true" : "false") + "}";
    }
    j += "],\"connected\":" + String(staUp ? "true" : "false")
       + ",\"ssid\":\""     + (staUp ? WiFi.SSID() : String(""))
       + "\",\"ip\":\""    + (staUp ? WiFi.localIP().toString() : String(""))
       + "\",\"rssi\":"     + String(staUp ? WiFi.RSSI() : 0) + "}";
    req->send(200, "application/json", j);
}

static void handle_wifi_post(AsyncWebServerRequest* req, uint8_t* data,
                             size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body)) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    // v5.5.1: {"slot":1..3,"ssid":..,"pass":..} — fehlender "slot" fällt
    // auf 1 zurück (Abwärtskompatibilität zum v5.1-Einzel-SSID-Client).
    uint8_t slot = doc["slot"] | 1;
    String ssid = String((const char*)(doc["ssid"] | ""));
    // Leeres/fehlendes "pass" = vorhandenes Passwort beibehalten.
    // Explizit löschen über {"slot":N,"ssid":"","pass":""}.
    bool hasPass = doc.containsKey("pass");
    String pass = String((const char*)(doc["pass"] | ""));
    free(body);

    if (slot < 1 || slot > WIFI_STA_SLOTS) {
        req->send(400, "application/json", "{\"error\":\"Slot 1..3\"}");
        return;
    }
    if (ssid.length() > 32) {
        req->send(400, "application/json", "{\"error\":\"SSID > 32 Zeichen\"}");
        return;
    }
    if (hasPass && pass.length() > 0 && pass.length() < 8) {
        req->send(400, "application/json", "{\"error\":\"WPA-Passwort < 8 Zeichen\"}");
        return;
    }

    uint8_t i = slot - 1;
    wprefs.putString(slot_key("ssid", i).c_str(), ssid);
    if (ssid.length() == 0)  wprefs.putString(slot_key("pass", i).c_str(), "");   // SSID gelöscht → PW auch
    else if (hasPass)        wprefs.putString(slot_key("pass", i).c_str(), pass); // nur bei mitgesendetem PW

    Serial.printf("[WEB] STA-Config geändert: Slot %u SSID=\"%s\"\n",
                  (unsigned)slot, ssid.c_str());
    // Keine WiFi-Calls hier (AsyncTCP-Task) — nur Flag setzen. Die
    // Rekonfiguration erfolgt im ws_task, nachdem dieser Response geflusht ist.
    s_wifiReapply = true;
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── Lagesensor (Wasserwaage, optional) ────────────────────────
static void handle_level_get(AsyncWebServerRequest* req) {
    req->send(200, "application/json", level_to_json());
}

static void handle_levelcfg_get(AsyncWebServerRequest* req) {
    req->send(200, "application/json", level_cfg_to_json());
}

static void handle_levelcfg_post(AsyncWebServerRequest* req, uint8_t* data,
                                 size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body)) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    bool ok = true;
    if (doc.containsKey("track"))     ok &= level_set_track    (doc["track"]);
    if (doc.containsKey("wheelbase")) ok &= level_set_wheelbase(doc["wheelbase"]);
    if (doc.containsKey("rot"))       ok &= level_set_rot      (doc["rot"]);
    // invRoll/invPitch teilen sich einen Setter — beide gemeinsam anwenden,
    // fehlender Wert fällt auf false zurück (Dashboard sendet ohnehin immer beide).
    if (doc.containsKey("invRoll") || doc.containsKey("invPitch")) {
        bool invR = doc["invRoll"]  | false;
        bool invP = doc["invPitch"] | false;
        ok &= level_set_invert(invR, invP);
    }
    if (doc.containsKey("flipZ"))     ok &= level_set_flipz    (doc["flipZ"]);   // v5.5 Überkopf-Einbau
    if (doc.containsKey("enabled"))   ok &= level_set_enabled  (doc["enabled"]);
    free(body);
    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"Wert außerhalb Grenzen\"}");
}

static void handle_levelcalib_post(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, body)) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    bool reset = doc["reset"] | false;
    free(body);
    bool ok = level_calibrate(reset);
    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"Keine gültige Messung zum Kalibrieren\"}");
}

// v5.6.1: Ringpuffer-Array-Builder — gemeinsamer Pfad für
// GET /api/buffer (HTTP) und {"cmd":"buffer"} (BLE, s. P-SW21).
// Baut das JSON-Array in PSRAM; Aufrufer übernimmt den Puffer
// (free() bzw. send_psram_json via shared_ptr). Zeilenformat und
// Clamps (count<=2000, step>=1) unverändert aus v5.4/H-3/F-18.
// Rückgabe nullptr bei PSRAM-OOM.
char* webserver_buffer_json(uint32_t offset, uint32_t count,
                            uint32_t step, size_t* outLen) {
    *outLen = 0;
    if (count > 2000) count = 2000;
    if (step  < 1)    step  = 1;

    LogEntry* entries = (LogEntry*)ps_malloc(count * sizeof(LogEntry));
    if (!entries) return nullptr;
    uint32_t n = logger_snapshot(offset, count, step, entries);

    // H-3: JSON komplett in PSRAM aufbauen (nicht in den Internal-RAM-Stream),
    // danach chunked streamen. ~240 B/Eintrag reichlich bemessen.
    size_t cap = (size_t)n * 240 + 16;
    char*  out = (char*)ps_malloc(cap);
    if (!out) { free(entries); return nullptr; }

    size_t off = 0;
    out[off++] = '[';
    // F-18: clock_local_offset_at() (gmtime_r+mktime) ist teuer und wurde pro
    // Eintrag (~700–900×) im AsyncTCP-Task aufgerufen. Der lokale Offset ist
    // innerhalb einer UTC-Stunde konstant; DST-Wechsel liegen exakt auf
    // Stundengrenzen → Cache pro UTC-Stunde ist korrekt und spart ~mktime/Eintrag.
    int32_t  offCache = 0;
    uint32_t offHour  = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < n; i++) {
        const LogEntry& e = entries[i];
        uint32_t hour = e.timestamp / 3600UL;
        if (hour != offHour) { offCache = clock_local_offset_at(e.timestamp); offHour = hour; }
        int w = snprintf(out + off, cap - off,
            "%s{\"ts\":%lu,\"ts_mez\":%lu,\"v\":%.2f,\"i\":%.1f,\"soc\":%d,"
            "\"tt\":%d,\"ts1\":%d,\"ts2\":%d,\"tmos\":%d,"
            "\"rem\":%.1f,\"ppv\":%d,\"vpv\":%.1f,\"ipv\":%.2f,"
            "\"cs\":%d,\"err\":%d,\"fl\":%d}",
            i?",":"",
            (unsigned long)e.timestamp,
            (unsigned long)(e.timestamp + offCache),
            e.bmsVoltage/100.0f, e.bmsCurrent/10.0f, e.soc,
            e.tempTube, e.tempSensor1, e.tempSensor2, e.tempMOS,
            e.remainAh10/10.0f, e.pvPower,
            e.pvVoltage10/10.0f, e.pvCurrent100/100.0f,
            e.mpptState, e.mpptError, e.flags);
        if (w < 0 || (size_t)w >= cap - off) break;   // Schutz (sollte nie greifen)
        off += (size_t)w;
    }
    free(entries);
    if (off + 2 <= cap) out[off++] = ']';
    *outLen = off;
    return out;
}

static void handle_buffer(AsyncWebServerRequest* req) {
    uint32_t offset = req->hasParam("offset") ? req->getParam("offset")->value().toInt() : 0;
    uint32_t count  = req->hasParam("count")  ? req->getParam("count") ->value().toInt() : 900;
    uint32_t step   = req->hasParam("step")   ? req->getParam("step")  ->value().toInt() : 1;
    size_t len = 0;
    char* out = webserver_buffer_json(offset, count, step, &len);   // v5.6.1: gemeinsamer Builder
    // Chunked aus PSRAM streamen; send_psram_json übernimmt free(out) via
    // shared_ptr und deckt auch out==nullptr (OOM → 503) ab.
    send_psram_json(req, out, len);
}

static void handle_sdfiles(AsyncWebServerRequest* req) {
    if (!logger_sd_available()) {
        req->send(503,"application/json","{\"error\":\"SD nicht verfügbar\"}"); return;
    }
    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        req->send(503,"application/json","{\"error\":\"SD belegt\"}"); return;
    }
    File root = SD.open("/");
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    resp->print("[");
    bool first = true;
    if (root) {
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String name = f.name();
                if (name.endsWith(".csv") || name.endsWith(".log")) {
                    char entry[80];
                    snprintf(entry, sizeof(entry),
                        "%s{\"name\":\"%s\",\"size\":%lu}",
                        first?"":",", name.c_str(), (unsigned long)f.size());
                    resp->print(entry);
                    first = false;
                }
            }
            f = root.openNextFile();
        }
        root.close();
    }
    xSemaphoreGive(g_sdMutex);
    resp->print("]");
    req->send(resp);
}

static void handle_sddata(AsyncWebServerRequest* req) {
    if (!logger_sd_available()) {
        req->send(503,"application/json","{\"error\":\"SD nicht verfügbar\"}"); return;
    }
    String   file  = req->hasParam("file")  ? req->getParam("file") ->value() : "";
    uint32_t skip  = req->hasParam("skip")  ? req->getParam("skip") ->value().toInt() : 0;
    uint32_t limit = req->hasParam("limit") ? req->getParam("limit")->value().toInt() : 500;
    if (limit > 2000) limit = 2000;
    if (file.length() < 2 || file[0] != '/' || file.indexOf("..") >= 0) {
        req->send(400,"application/json","{\"error\":\"Ungültiger Pfad\"}"); return;
    }
    // H-3: Zeilen unter g_sdMutex in einen PSRAM-Puffer serialisieren, dann
    // Mutex + Datei freigeben und aus PSRAM chunked streamen. Kein
    // Internal-RAM-Response-Puffer mehr (vermeidet OOM bei limit=2000).
    size_t cap = (size_t)limit * 120 + 16;
    char*  out = (char*)ps_malloc(cap);
    if (!out) { req->send(503,"application/json","{\"error\":\"OOM PSRAM\"}"); return; }

    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(out);
        req->send(503,"application/json","{\"error\":\"SD belegt\"}"); return;
    }
    File f = SD.open(file.c_str(), FILE_READ);
    if (!f) {
        xSemaphoreGive(g_sdMutex);
        free(out);
        req->send(404,"application/json","{\"error\":\"Datei nicht gefunden\"}"); return;
    }
    size_t off = 0;
    out[off++] = '[';
    bool first = true;
    uint32_t line_nr = 0, added = 0;
    char line[256];
    while (f.available() && added < limit) {
        size_t llen = 0;
        while (f.available() && llen < sizeof(line)-1) {
            char c = (char)f.read();
            if (c == '\n') break;
            if (c != '\r') line[llen++] = c;
        }
        line[llen] = '\0';
        if (llen == 0) continue;
        line_nr++;
        if (line_nr == 1) continue;
        if (line_nr <= skip+1) continue;
        char* cols[20] = {};
        uint8_t col = 0;
        char* p = line;
        cols[col++] = p;
        while (*p && col < 20) { if (*p == ',') { *p = '\0'; cols[col++] = p+1; } p++; }
        if (col < 12) continue;
        if (atol(cols[0]) <= 0) continue;
        int w = snprintf(out + off, cap - off,
            "%s{\"ts\":%s,\"v\":%s,\"i\":%s,\"soc\":%s,\"ppv\":%s}",
            first?"":"," , cols[0],cols[1],cols[2],cols[3],cols[11]);
        if (w < 0 || (size_t)w >= cap - off) break;   // Puffer voll → sauber stoppen
        off += (size_t)w;
        first = false;
        added++;
    }
    f.close();
    xSemaphoreGive(g_sdMutex);
    if (off + 2 <= cap) out[off++] = ']';
    send_psram_json(req, out, off);   // übernimmt free(out) via shared_ptr
}

// ── v5.6.0: Live-JSON für andere Module (BLE {"cmd":"live"}) ──
String webserver_live_json() {
    return build_live_json();
}

// ── v5.6.0: BLE-Schalter ──────────────────────────────────────
// GET  /api/ble  → {"en":…,"active":…,"connected":…,"subscribed":…,"name":…}
// POST /api/ble  {"en":0|1} → NVS setzen; bei Änderung deferred
// Reboot über den sicheren OTA-Pfad (Ringpuffer-Sicherung). Der
// Handler läuft im AsyncTCP-Task — ble_set_enabled schreibt nur
// NVS, ota_schedule_reboot setzt nur ein Flag: beides zulässig.
static void handle_ble_get(AsyncWebServerRequest* req) {
    req->send(200, "application/json", ble_to_json());
}
static void handle_ble_post(AsyncWebServerRequest* req, uint8_t* data,
                            size_t len, size_t index, size_t total) {
    char* body = collect_body_chunk(req, data, len, index, total);
    if (!body) return;
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, body) || !doc.containsKey("en")) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    bool en = doc["en"].as<int>() != 0;
    free(body);
    if (en == ble_enabled()) {                  // keine Änderung → kein Reboot
        req->send(200, "application/json", "{\"ok\":true,\"reboot\":false}");
        return;
    }
    ble_set_enabled(en);
    ota_schedule_reboot(1500);                  // Antwort erst ausliefern lassen
    req->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
}

void webserver_init() {
    if (!LittleFS.begin(true)) Serial.println("[WEB] LittleFS FEHLER!");
    else                        Serial.println("[WEB] LittleFS OK");

    wprefs.begin("wifi", false);
    // v5.5.1-Migration: alten Einzel-Key "ssid"/"pass" nach Slot 1 heben.
    if (wprefs.isKey("ssid")) {
        String oSsid = wprefs.getString("ssid", "");
        String oPass = wprefs.getString("pass", "");
        if (oSsid.length() && !wprefs.isKey("ssid1")) {
            wprefs.putString("ssid1", oSsid);
            wprefs.putString("pass1", oPass);
            Serial.println("[WEB] WLAN-Migration: SSID nach Slot 1 übernommen");
        }
        wprefs.remove("ssid");
        if (wprefs.isKey("pass")) wprefs.remove("pass");
    }
    WiFi.onEvent(on_wifi_event);
    WiFi.mode(WIFI_AP_STA);          // AP dauerhaft + optional STA (Heimnetz)
    // v5.6.7: DHCP-Hostname für die STA-Seite — die Zentrale erscheint im
    // Router (FRITZ!Box) als "womo" statt als anonymes espressif-Gerät.
    // Reihenfolge zwingend NACH WiFi.mode() (STA-netif existiert erst dann)
    // und VOR dem ersten WiFi.begin() aus wifi_apply_sta(); der Name haftet
    // am STA-netif und übersteht so auch die Reconnects/Rescan-begin()s im
    // wifi_tick() — das netif wird im Betrieb nie neu angelegt (Mode bleibt
    // fix AP_STA, disconnect() lässt es bestehen). Core-2.x-konform.
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
    Serial.printf("[WEB] AP: %s  IP: %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    wifi_apply_sta();                // STA aus NVS starten (falls SSID gesetzt)
    mdns_announce();                 // v5.5.2: womo.local ab Boot (AP-Modus);
                                     // Reannounce nach STA-Verbindung im ws_task

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);

    server.on("/api/live",    HTTP_GET,  handle_live);
    server.on("/api/params",  HTTP_GET,  handle_params_get);
    server.on("/api/reset",   HTTP_POST, handle_reset);
    server.on("/api/manual", HTTP_POST,
        post_empty_guard, nullptr, handle_manual_post);
    server.on("/api/ble",     HTTP_GET,  handle_ble_get);      // v5.6.0
    server.on("/api/ble", HTTP_POST,
        post_empty_guard, nullptr, handle_ble_post);           // v5.6.0
    server.on("/api/buffer",  HTTP_GET,  handle_buffer);
    server.on("/api/sdfiles", HTTP_GET,  handle_sdfiles);
    server.on("/api/sddata",  HTTP_GET,  handle_sddata);
    server.on("/api/params", HTTP_POST,
        post_empty_guard, nullptr, handle_params_post);
    server.on("/api/time", HTTP_POST,
        post_empty_guard, nullptr, handle_time);
    server.on("/api/tz",      HTTP_GET,  handle_tz_get);
    server.on("/api/tz", HTTP_POST,
        post_empty_guard, nullptr, handle_tz_post);
    server.on("/api/wifi",    HTTP_GET,  handle_wifi_get);
    server.on("/api/wifi", HTTP_POST,
        post_empty_guard, nullptr, handle_wifi_post);
    server.on("/api/level",      HTTP_GET,  handle_level_get);
    server.on("/api/levelcfg",   HTTP_GET,  handle_levelcfg_get);
    server.on("/api/levelcfg", HTTP_POST,
        post_empty_guard, nullptr, handle_levelcfg_post);
    server.on("/api/levelcalib", HTTP_POST,
        post_empty_guard, nullptr, handle_levelcalib_post);
    // Web-OTA (v5.4.1): GET = Info, POST = Multipart-Datei-Upload.
    // 4. Argument ist der onUpload-Callback (NICHT onBody wie bei den
    // JSON-POSTs); ota_handle_request antwortet nach Upload-Abschluss
    // und deckt auch den Leerer-Body-Fall ab (kein post_empty_guard nötig).
    server.on("/api/ota", HTTP_GET, [](AsyncWebServerRequest* r){
        r->send(200, "application/json", ota_to_json());
    });
    server.on("/api/ota", HTTP_POST, ota_handle_request, ota_handle_upload);

    // Statischer Catch-all ZULETZT — sonst fängt er /api/* ab
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest* r){ r->send(404,"text/plain","404"); });
    server.begin();
    Serial.println("[WEB] Port " + String(WEBSERVER_PORT));
}

void webserver_broadcast() {
    // Deferred-Apply: WiFi-Rekonfiguration im ws_task-Kontext (Core 0),
    // ausgelöst per /api/wifi-POST. Bewusst VOR dem ws.count()-Return,
    // damit es auch ohne verbundenen WebSocket-Client greift.
    if (s_wifiReapply) {
        s_wifiReapply = false;
        WiFi.disconnect(false, true);     // alte STA-Config verwerfen, AP bleibt
        wifi_apply_sta();                 // neue Credentials anwenden (non-blocking)
    }
    wifi_tick();   // v5.5.1: Scan-Auswertung / Rescan-Backoff (Multi-SSID)

    // v5.5.2: STA verbunden → mDNS auf STA-IP neu ankündigen + NTP anstoßen.
    if (s_staGotIp) {
        s_staGotIp = false;
        mdns_announce();
        ntp_start();
    }
    // v5.5.2: SNTP hat frische UTC geliefert → in die DS3231-Zeitbasis
    // übernehmen. Derselbe Pfad wie der Browser-Sync (/api/time): time()
    // liefert UTC (TZ-unabhängig), clock_set_epoch() greift mit 5-s-
    // Hysterese und stellt bei Bedarf DS3231 + NVS.
    if (s_ntpSynced) {
        s_ntpSynced = false;
        time_t utc = time(nullptr);
        if (utc > 1704067200) {          // Plausibilität (>= 2024-01-01)
            if (clock_set_epoch((uint32_t)utc)) {
                s_ntpLastSync = (uint32_t)utc;   // v5.5.3: für Dashboard-Anzeige
                Serial.printf("[NTP] Zeit gestellt: UTC=%ld\n", (long)utc);
            }
        }
    }

    ble_tick();   // v5.6.0: BLE-RX-Kommandos ausführen + Antworten senden
                  // (gleicher Kontext wie WS-Broadcast → ein Sende-Kontext)

    // v5.6.0: JSON EINMAL bauen, an WS und BLE verteilen.
    bool wantWs  = ws.count() > 0;
    bool wantBle = ble_subscribed();
    if (!wantWs && !wantBle) return;
    String json = build_live_json();
    if (wantBle) ble_notify_live(json);
    if (!wantWs) return;

    // H-1: ws.textAll() iteriert die Client-Liste UNTER dem internen
    // _ws_clients_lock (verifiziert im esp32async-Fork ^3.7.0, AsyncWebSocket.cpp:
    // textAll() Z.1206, cleanupClients()/_addClient()/_handleDisconnect() nehmen
    // denselben Lock). Damit ist der Broadcast aus dem ws_task race-frei gegen
    // Connect/Disconnect im AsyncTCP-Task. Das frühere manuelle Loop über
    // ws.getClients() iterierte die std::list OHNE diesen Lock → Iterator-
    // Invalidierung / Use-after-free, wenn ein Client sich während des
    // Broadcasts trennte (K-2 war damit nur teilbehoben). textAll() serialisiert
    // die Message per ref-gezähltem SharedBuffer und liefert Backpressure über
    // die client-eigene Sende-Queue.
    ws.textAll(json);
    ws.cleanupClients();   // getrennte Clients aufräumen (ebenfalls unter Lock)
}