// ============================================================
//  http_server.cpp — Womo Energy Core v5.5.1
//
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
#include <WiFi.h>
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
#define WIFI_STA_SLOTS            3
#define WIFI_RESCAN_INTERVAL_MS   60000     // Rescan-Backoff wenn getrennt
static Preferences  wprefs;
static bool         s_staEnabled = false;   // mind. ein Slot konfiguriert?
// POST setzt nur dieses Flag; die eigentliche WiFi-Rekonfiguration läuft
// im ws_task-Kontext (webserver_broadcast), NICHT im AsyncTCP-Handler.
static volatile bool s_wifiReapply = false;
// Scan-Zustandsmaschine — ausschließlich ws_task schreibt/liest:
enum StaScanState : uint8_t { STA_IDLE = 0, STA_SCANNING };
static StaScanState s_staScan     = STA_IDLE;
static uint32_t     s_staNextScan = 0;      // millis()-Marke für Rescan

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
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, false /*keine Hidden-SSIDs*/);
    s_staScan     = STA_SCANNING;
    s_staNextScan = millis() + WIFI_RESCAN_INTERVAL_MS;
    Serial.printf("[WEB] STA: %u Netze konfiguriert — Scan läuft ...\n", (unsigned)n);
}

// v5.5.1: Scan auswerten + Rescan-Backoff. NUR aus webserver_broadcast
// (ws_task) aufrufen — gleiche Kontext-Regel wie wifi_apply_sta.
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
        s_staScan     = STA_IDLE;
        s_staNextScan = millis() + WIFI_RESCAN_INTERVAL_MS;
        if (best >= 0) {
            WiFi.setAutoReconnect(true);
            WiFi.begin(bestSsid.c_str(), bestPass.c_str());
            Serial.printf("[WEB] STA: stärkstes bekanntes Netz \"%s\" (%d dBm, Slot %u)\n",
                          bestSsid.c_str(), (int)bestRssi, (unsigned)(best + 1));
        } else {
            Serial.println("[WEB] STA: kein bekanntes Netz in Reichweite — Rescan in 60s");
        }
        return;
    }

    // Getrennt + mehrere Kandidaten → periodischer Rescan (Heimkehr/
    // Roaming zwischen bekannten Netzen). Bei genau einem Slot erledigt
    // das der Arduino-AutoReconnect wie bisher.
    if (!WiFi.isConnected() && slots_configured() > 1 &&
        (int32_t)(millis() - s_staNextScan) >= 0) {
        WiFi.scanDelete();
        WiFi.scanNetworks(true, false);
        s_staScan     = STA_SCANNING;
        s_staNextScan = millis() + WIFI_RESCAN_INTERVAL_MS;
    }
}

static void on_ws_event(AsyncWebSocket* s, AsyncWebSocketClient* c,
                        AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT || type == WS_EVT_DISCONNECT)
        ws.cleanupClients();
}

static String build_live_json() {
    char hdr[160];
    String tzab = clock_tz_abbr();
    snprintf(hdr, sizeof(hdr),
        "{\"ts\":%lu,\"epoch\":%lu,\"epoch_mez\":%lu,\"tz\":\"%s\",\"synced\":%s,",
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
       + "\",\"rssi\":"           + String(staUp?WiFi.RSSI():0) + "},";
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
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, body)) {
        free(body);
        req->send(400, "application/json", "{\"error\":\"JSON ungültig\"}");
        return;
    }
    free(body);
    bool ok = true;
    // v5.5: Parameterbereinigung — socDPlusHigh/pvThresholdOff/socGelHigh/
    // socWROn entfallen, pvThresholdOn→pvDPlusMinW, socGelOff neu.
    if (doc.containsKey("socDPlusOn"))          ok &= params_set_soc_dplus_on         (doc["socDPlusOn"]);
    if (doc.containsKey("socDPlusOff"))         ok &= params_set_soc_dplus_off        (doc["socDPlusOff"]);
    if (doc.containsKey("pvDPlusMinW"))         ok &= params_set_pv_dplus_min_w       (doc["pvDPlusMinW"]);
    if (doc.containsKey("socGelOn"))            ok &= params_set_soc_gel_on           (doc["socGelOn"]);
    if (doc.containsKey("socGelOff"))           ok &= params_set_soc_gel_off          (doc["socGelOff"]);
    if (doc.containsKey("pvGelMinW"))           ok &= params_set_pv_gel_min_w         (doc["pvGelMinW"]);
    if (doc.containsKey("socWROff"))            ok &= params_set_soc_wr_off           (doc["socWROff"]);
    if (doc.containsKey("relayDebounceCycles")) ok &= params_set_relay_debounce_cycles(doc["relayDebounceCycles"]);
    if (doc.containsKey("logIntervalMs"))       ok &= params_set_log_interval_ms      (doc["logIntervalMs"]);
    if (doc.containsKey("manualTimeoutMin"))    ok &= params_set_manual_timeout_min   (doc["manualTimeoutMin"]);
    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"Wert außerhalb Grenzen\"}");
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

static void handle_buffer(AsyncWebServerRequest* req) {
    uint32_t offset = req->hasParam("offset") ? req->getParam("offset")->value().toInt() : 0;
    uint32_t count  = req->hasParam("count")  ? req->getParam("count") ->value().toInt() : 900;
    uint32_t step   = req->hasParam("step")   ? req->getParam("step")  ->value().toInt() : 1;
    if (count > 2000) count = 2000;
    if (step  < 1)    step  = 1;

    LogEntry* entries = (LogEntry*)ps_malloc(count * sizeof(LogEntry));
    if (!entries) { req->send(503,"application/json","{\"error\":\"OOM PSRAM\"}"); return; }
    uint32_t n = logger_snapshot(offset, count, step, entries);

    // H-3: JSON komplett in PSRAM aufbauen (nicht in den Internal-RAM-Stream),
    // danach chunked streamen. ~240 B/Eintrag reichlich bemessen.
    size_t cap = (size_t)n * 240 + 16;
    char*  out = (char*)ps_malloc(cap);
    if (!out) { free(entries); req->send(503,"application/json","{\"error\":\"OOM PSRAM\"}"); return; }

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
    // Chunked aus PSRAM streamen; send_psram_json übernimmt free(out) via shared_ptr.
    send_psram_json(req, out, off);
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
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
    Serial.printf("[WEB] AP: %s  IP: %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    wifi_apply_sta();                // STA aus NVS starten (falls SSID gesetzt)

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);

    server.on("/api/live",    HTTP_GET,  handle_live);
    server.on("/api/params",  HTTP_GET,  handle_params_get);
    server.on("/api/reset",   HTTP_POST, handle_reset);
    server.on("/api/manual", HTTP_POST,
        post_empty_guard, nullptr, handle_manual_post);
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

    if (ws.count() == 0) return;
    String json = build_live_json();

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