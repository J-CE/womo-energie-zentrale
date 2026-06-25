// ============================================================
//  http_server.cpp — Womo Energy Core v5.0
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
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>

static AsyncWebServer server(WEBSERVER_PORT);
static AsyncWebSocket ws("/ws");

static void on_ws_event(AsyncWebSocket* s, AsyncWebSocketClient* c,
                        AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT || type == WS_EVT_DISCONNECT)
        ws.cleanupClients();
}

static String build_live_json() {
    char hdr[128];
    snprintf(hdr, sizeof(hdr),
        "{\"ts\":%lu,\"epoch\":%lu,\"epoch_mez\":%lu,\"synced\":%s,",
        (unsigned long)(millis()/1000),
        (unsigned long)clock_now(),
        (unsigned long)clock_now_local(),
        clock_is_synced()?"true":"false");
    String j = hdr;
    j += "\"bms\":"   + bms_to_json()      + ",";
    j += "\"mppt\":"  + mppt_to_json()     + ",";
    j += "\"io\":"    + io_to_json()       + ",";
    j += "\"logic\":" + logic_status_json()+ ",";
    j += "\"inv\":"   + inverter_to_json() + ",";
    j += "\"sys\":{\"heap\":"     + String(ESP.getFreeHeap())
       + ",\"min_heap\":"         + String(ESP.getMinFreeHeap())
       + ",\"psram\":"            + String(ESP.getFreePsram()) + "},";
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
    req->send(ok?200:400, "application/json",
              ok?"{\"ok\":true}":"{\"error\":\"Wert außerhalb Grenzen\"}");
}

static void handle_reset(AsyncWebServerRequest* req) {
    params_reset();
    req->send(200, "application/json", "{\"ok\":true}");
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
            (unsigned long)(e.timestamp + CLOCK_MEZ_OFFSET_SEC),
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

    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
    Serial.printf("[WEB] AP: %s  IP: %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);

    server.on("/api/live",    HTTP_GET,  handle_live);
    server.on("/api/params",  HTTP_GET,  handle_params_get);
    server.on("/api/reset",   HTTP_POST, handle_reset);
    server.on("/api/buffer",  HTTP_GET,  handle_buffer);
    server.on("/api/sdfiles", HTTP_GET,  handle_sdfiles);
    server.on("/api/sddata",  HTTP_GET,  handle_sddata);
    server.on("/api/params", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_params_post);
    server.on("/api/time", HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handle_time);

    // Statischer Catch-all ZULETZT — sonst fängt er /api/* ab
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest* r){ r->send(404,"text/plain","404"); });
    server.begin();
    Serial.println("[WEB] Port " + String(WEBSERVER_PORT));
}

void webserver_broadcast() {
    if (ws.count() == 0) return;
    String json = build_live_json();
    AsyncWebSocketMessageBuffer* buf = ws.makeBuffer(json.length());
    if (!buf) return;
    memcpy(buf->get(), json.c_str(), json.length());
    ws.textAll(buf);
}
