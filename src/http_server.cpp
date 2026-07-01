// ============================================================
//  http_server.cpp — Womo Energy Core v5.4
//
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
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <Preferences.h>

static AsyncWebServer server(WEBSERVER_PORT);
static AsyncWebSocket ws("/ws");

// ── Heim-WLAN (STA) ───────────────────────────────────────────
// Credentials liegen im NVS-Namespace "wifi" (getrennt von clock/params).
// Passwort wird per /api/wifi NIE zurückgegeben.
static Preferences  wprefs;
static bool         s_staEnabled = false;   // SSID konfiguriert?
// POST setzt nur dieses Flag; die eigentliche WiFi-Rekonfiguration läuft
// im ws_task-Kontext (webserver_broadcast), NICHT im AsyncTCP-Handler.
static volatile bool s_wifiReapply = false;

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

// Liest SSID/PW aus NVS und startet (oder beendet) den STA-Teil.
// Non-blocking: WiFi.begin() blockiert den Boot/Webserver nicht.
static void wifi_apply_sta() {
    String ssid = wprefs.getString("ssid", DEFAULT_WIFI_STA_SSID);
    String pass = wprefs.getString("pass", DEFAULT_WIFI_STA_PASSWORD);

    s_staEnabled = (ssid.length() > 0);
    if (s_staEnabled) {
        WiFi.setAutoReconnect(true);
        WiFi.begin(ssid.c_str(), pass.c_str());   // non-blocking
        Serial.printf("[WEB] STA: suche \"%s\" ...\n", ssid.c_str());
    } else {
        // SSID leer → STA abschalten, AP bleibt bestehen (eraseap=true).
        WiFi.disconnect(false, true);
        Serial.println("[WEB] STA deaktiviert (keine SSID konfiguriert)");
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
       + ",\"ip\":\""             + (staUp?WiFi.localIP().toString():String(""))
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
    if (doc.containsKey("socDPlusOn"))          ok &= params_set_soc_dplus_on         (doc["socDPlusOn"]);
    if (doc.containsKey("socDPlusOff"))         ok &= params_set_soc_dplus_off        (doc["socDPlusOff"]);
    if (doc.containsKey("socDPlusHigh"))        ok &= params_set_soc_dplus_high       (doc["socDPlusHigh"]);
    if (doc.containsKey("pvThresholdOn"))       ok &= params_set_pv_threshold_on      (doc["pvThresholdOn"]);
    if (doc.containsKey("pvThresholdOff"))      ok &= params_set_pv_threshold_off     (doc["pvThresholdOff"]);
    if (doc.containsKey("socGelOn"))            ok &= params_set_soc_gel_on           (doc["socGelOn"]);
    if (doc.containsKey("socGelHigh"))          ok &= params_set_soc_gel_high         (doc["socGelHigh"]);
    if (doc.containsKey("pvGelMinW"))           ok &= params_set_pv_gel_min_w         (doc["pvGelMinW"]);
    if (doc.containsKey("socWROn"))             ok &= params_set_soc_wr_on            (doc["socWROn"]);
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

// ── Manueller Aktor-Override (Webinterface, v5.4) ─────────────
// {"actuator":"dplus|gel|wr","mode":"auto|on|off"}
// "auto" schaltet sofort zurück in die Automatik. "on"/"off" setzt
// Manual-Modus + Deadman-Timer (siehe logic_set_manual/logic.cpp).
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

// ── Heim-WLAN-Status / -Konfiguration ─────────────────────────
static void handle_wifi_get(AsyncWebServerRequest* req) {
    bool staUp = WiFi.isConnected();
    String ssid = wprefs.getString("ssid", DEFAULT_WIFI_STA_SSID);
    char buf[200];
    // Passwort wird bewusst NICHT ausgeliefert; "set" zeigt nur, ob hinterlegt.
    snprintf(buf, sizeof(buf),
        "{\"ssid\":\"%s\",\"set\":%s,\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d}",
        ssid.c_str(),
        ssid.length() ? "true" : "false",
        staUp ? "true" : "false",
        staUp ? WiFi.localIP().toString().c_str() : "",
        staUp ? WiFi.RSSI() : 0);
    req->send(200, "application/json", buf);
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
    String ssid = String((const char*)(doc["ssid"] | ""));
    // Leeres/fehlendes "pass" = vorhandenes Passwort beibehalten.
    // Explizit löschen über {"ssid":"","pass":""}.
    bool hasPass = doc.containsKey("pass");
    String pass = String((const char*)(doc["pass"] | ""));
    free(body);

    if (ssid.length() > 32) {
        req->send(400, "application/json", "{\"error\":\"SSID > 32 Zeichen\"}");
        return;
    }
    if (hasPass && pass.length() > 0 && pass.length() < 8) {
        req->send(400, "application/json", "{\"error\":\"WPA-Passwort < 8 Zeichen\"}");
        return;
    }

    wprefs.putString("ssid", ssid);
    if (ssid.length() == 0)  wprefs.putString("pass", "");      // SSID gelöscht → PW auch
    else if (hasPass)        wprefs.putString("pass", pass);    // nur bei mitgesendetem PW

    Serial.printf("[WEB] STA-Config geändert: SSID=\"%s\"\n", ssid.c_str());
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

    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    resp->print("[");
    char tmp[220];
    for (uint32_t i = 0; i < n; i++) {
        const LogEntry& e = entries[i];
        snprintf(tmp, sizeof(tmp),
            "%s{\"ts\":%lu,\"ts_mez\":%lu,\"v\":%.2f,\"i\":%.1f,\"soc\":%d,"
            "\"tt\":%d,\"ts1\":%d,\"ts2\":%d,\"tmos\":%d,"
            "\"rem\":%.1f,\"ppv\":%d,\"vpv\":%.1f,\"ipv\":%.2f,"
            "\"cs\":%d,\"err\":%d,\"fl\":%d}",
            i?",":"",
            (unsigned long)e.timestamp,
            (unsigned long)(e.timestamp + clock_local_offset_at(e.timestamp)),
            e.bmsVoltage/100.0f, e.bmsCurrent/10.0f, e.soc,
            e.tempTube, e.tempSensor1, e.tempSensor2, e.tempMOS,
            e.remainAh10/10.0f, e.pvPower,
            e.pvVoltage10/10.0f, e.pvCurrent100/100.0f,
            e.mpptState, e.mpptError, e.flags);
        resp->print(tmp);
    }
    resp->print("]");
    free(entries);
    req->send(resp);
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
    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        req->send(503,"application/json","{\"error\":\"SD belegt\"}"); return;
    }
    File f = SD.open(file.c_str(), FILE_READ);
    if (!f) {
        xSemaphoreGive(g_sdMutex);
        req->send(404,"application/json","{\"error\":\"Datei nicht gefunden\"}"); return;
    }
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    resp->print("[");
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
        char entry[128];
        snprintf(entry, sizeof(entry),
            "%s{\"ts\":%s,\"v\":%s,\"i\":%s,\"soc\":%s,\"ppv\":%s}",
            first?"":"," , cols[0],cols[1],cols[2],cols[3],cols[11]);
        resp->print(entry);
        first = false;
        added++;
    }
    f.close();
    xSemaphoreGive(g_sdMutex);
    resp->print("]");
    req->send(resp);
}

void webserver_init() {
    if (!LittleFS.begin(true)) Serial.println("[WEB] LittleFS FEHLER!");
    else                        Serial.println("[WEB] LittleFS OK");

    wprefs.begin("wifi", false);
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
        [](AsyncWebServerRequest* r){}, nullptr, handle_manual_post);
    server.on("/api/buffer",  HTTP_GET,  handle_buffer);
    server.on("/api/sdfiles", HTTP_GET,  handle_sdfiles);
    server.on("/api/sddata",  HTTP_GET,  handle_sddata);
    server.on("/api/params", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_params_post);
    server.on("/api/time", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_time);
    server.on("/api/tz",      HTTP_GET,  handle_tz_get);
    server.on("/api/tz", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_tz_post);
    server.on("/api/wifi",    HTTP_GET,  handle_wifi_get);
    server.on("/api/wifi", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_wifi_post);
    server.on("/api/level",      HTTP_GET,  handle_level_get);
    server.on("/api/levelcfg",   HTTP_GET,  handle_levelcfg_get);
    server.on("/api/levelcfg", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_levelcfg_post);
    server.on("/api/levelcalib", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_levelcalib_post);

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

    if (ws.count() == 0) return;
    String json = build_live_json();

    // K-2: NICHT ws.textAll() aus diesem Task (ws_task, Core 0) — das iteriert
    // die Client-Liste, die der AsyncTCP-Task bei Connect/Disconnect verändert
    // → Use-after-free, wenn sich ein Client während des Broadcasts trennt.
    // Stattdessen je Client prüfen (verbunden + Sende-Queue frei) und einzeln
    // senden. Reduziert das Race-Fenster deutlich und liefert Backpressure.
    // Restrisiko bleibt (echte Absicherung nur im AsyncTCP-Kontext möglich).
    for (auto& c : ws.getClients()) {
        if (c.status() == WS_CONNECTED && c.canSend())
            c.text(json.c_str(), json.length());
    }
}