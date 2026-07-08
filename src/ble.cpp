// ============================================================
//  ble.cpp — Womo Energy Core v5.6.4
//  v5.6.4 BUGFIX (der eigentliche Grund für "Live/System/Laufzeit tot,
//  Lage/Einstellungen ok"): Chunk-Größe war MTU−3 = 514 B bei MTU 517.
//  Android kappt seit Android 13 EMPFANGENE Notify-Werte OS-intern hart
//  bei GATT_MAX_ATTR_LEN = 512 B (github.com/espressif/esp-idf #10206:
//  "2 bytes are being silently dropped"; Android-Verhaltensänderung ab
//  API 34 bestätigt dieselbe Grenze). Der Drop passiert NACH erfolg-
//  reicher Funkübertragung im Android-Stack — der v5.6.3-rc-Check half
//  hier prinzipbedingt nicht, da ble_gatts_notify_custom() rc=0 meldet
//  und die Kappung erst beim Client stattfindet. Jeder volle 514-B-
//  Chunk verlor die letzten 2 B mitten in der JSON-Zeile → korrupt →
//  _bleRx-JSON.parse scheitert lautlos → Live/System/Laufzeit blieben
//  leer. Frames ≤512 B (params/level/resp) waren nie betroffen — exakt
//  das beobachtete Muster. Fix: ble_send_raw chunked jetzt IMMER auf
//  min(MTU−3, BLE_MAX_NOTIFY_LEN=512), unabhängig von der MTU.
//  v5.6.3 BUGFIX: NimBLECharacteristic::notify() (void) verschluckte
//  JEDEN Sendefehler. Verifiziert in 1.4.3: bei msys-Pool-Erschöpfung
//  (Default 12 × 292 B — unser Chunk-Burst hält Pakete bis zum
//  nächsten Connection-Event im Host) liefert ble_hs_mbuf_from_flat()
//  NULL; notify() reicht das NULL ungeprüft an ble_gatts_notify_custom
//  weiter, und NULL heißt dort "lies den Attributwert-Store und sende
//  DEN" (ble_gattc.c Z. 4172ff.) → leerer/stehengebliebener Frame
//  mitten in der Zeile. Zusätzlich verwarfen ENOMEM-Fälle tiefer im
//  Stack (ble_att_cmd_get/ble_att_tx) Chunks still. Beides zerstörte
//  die '\n'-Assemblierung deterministisch bei Mehrchunk-Frames
//  (Live/Buffer); Ein-Chunk-Frames (resp/params) blieben unauffällig.
//  Fix: ble_notify_chunk() — mbuf selbst allozieren, rc auswerten,
//  bei ENOMEM/EAGAIN mit Backoff wiederholen; bei Abbruch '\n' als
//  Framing-Reset. Flankierend msys-Pool → 30 Blöcke (platformio.ini).
//  v5.6.3 NEU: {"cmd":"level"} → {"type":"level","data":{…}} —
//  Lagesensor-Zustand für den Lage-Tab der App (identisch /api/level;
//  Konfiguration/Kalibrierung bleibt bewusst WLAN-only, L-SW24).
//  v5.6.2 BUGFIX: TX-Chunks >512 B wurden still verworfen (Attribut-
//  wert-Store, BLE_ATT_ATTR_MAX_LEN) — notify() sendete stale Daten;
//  Live-/Buffer-Frames bei MTU 517 korrupt. Fix: Direkt-Notify-
//  Überladung notify(data,len) umgeht den Store (s. ble_send_raw).
//  v5.6.1: {"cmd":"buffer"} — PSRAM-Historie über BLE (Vertrag:
//  {"type":"buffer","data":[…]} / {"type":"buffer","error":"…"};
//  Array + Parameter identisch GET /api/buffer, gemeinsamer Builder
//  webserver_buffer_json). MTU-Guard gegen ws_task-Blockade.
//  BLE GATT-Server (NUS) — Implementierung, s. ble.h.
//
//  NimBLE-Arduino GEPINNT auf 1.4.x: 2.x setzt IDF 5 voraus und
//  kollidiert mit Arduino-Core 2.x / IDF 4.4 (platformio.ini).
//
//  Thread-Modell:
//   • onWrite/onSubscribe/onConnect laufen im NimBLE-Host-Task.
//     Dort nur: Zeilenpuffer füllen, komplette Zeile per
//     xQueueSend (Timeout 0, bei voller Queue verwerfen) ablegen,
//     Flags setzen. KEINE NVS-Writes, KEINE Sends, KEIN Blocking.
//   • ble_tick()/ble_notify_live() laufen im ws_task (Core 0) —
//     einziger Sende-Kontext. logic_set_manual/params-Setter
//     (NVS-Writes) werden hier ausgeführt, identisch zur
//     bisherigen Deferred-Disziplin (WiFi-Reapply, NTP).
// ============================================================
#include "ble.h"
#include "config.h"
#include "secrets.h"
#include "params.h"
#include "logic.h"
#include "http_server.h"   // webserver_live_json()
#include "level.h"         // v5.6.3: level_to_json() für {"cmd":"level"}
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// ── Nordic UART Service UUIDs ─────────────────────────────────
static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX_UUID      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // Client → Server (Write)
static const char* NUS_TX_UUID      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // Server → Client (Notify)

// ── Zustand ───────────────────────────────────────────────────
static Preferences           blePrefs;
static bool                  s_enabled    = true;   // NVS "ble"/"en"
static bool                  s_active     = false;  // Stack läuft
static NimBLEServer*         s_server     = nullptr;
static NimBLECharacteristic* s_txChar     = nullptr;
static volatile bool         s_connected  = false;
static volatile bool         s_subscribed = false;  // nur nach authentifiziertem Subscribe
static volatile uint16_t     s_connHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool         s_pushNow    = false;  // {"cmd":"live"} → Sofort-Push im nächsten Tick

// ── RX-Zeilenassemblierung (NimBLE-Host-Task) → Queue ─────────
// Feste Elementgröße: Kommandos sind klein (<256 B). Überlange
// Zeilen werden verworfen (Puffer-Reset), volle Queue verwirft
// die neue Zeile (Backpressure statt Blockieren im Host-Task).
static QueueHandle_t s_rxQueue = nullptr;           // Elemente: char[BLE_RX_LINE_MAX]
static char          s_lineBuf[BLE_RX_LINE_MAX];
static size_t        s_lineLen = 0;

static void rx_feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_lineLen > 0) {
                s_lineBuf[s_lineLen] = '\0';
                if (s_rxQueue && xQueueSend(s_rxQueue, s_lineBuf, 0) != pdTRUE)
                    Serial.println("[BLE] RX-Queue voll — Kommando verworfen");
                s_lineLen = 0;
            }
            continue;
        }
        if (s_lineLen >= BLE_RX_LINE_MAX - 1) {      // Überlauf → Zeile verwerfen
            Serial.println("[BLE] RX-Zeile zu lang — verworfen");
            s_lineLen = 0;
            continue;
        }
        s_lineBuf[s_lineLen++] = c;
    }
}

// ── NimBLE-Callbacks (Host-Task: nur Flags/Queue!) ────────────
class WomoServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
        s_connected  = true;
        s_connHandle = desc->conn_handle;
        s_lineLen    = 0;                            // RX-Assembler zurücksetzen
        Serial.println("[BLE] Client verbunden");
        // Advertising stoppt automatisch; Neustart übernimmt
        // advertiseOnDisconnect(true) beim Trennen.
    }
    void onDisconnect(NimBLEServer*) override {
        s_connected  = false;
        s_subscribed = false;
        s_connHandle = BLE_HS_CONN_HANDLE_NONE;
        Serial.println("[BLE] Client getrennt — Advertising läuft wieder");
    }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        Serial.printf("[BLE] Auth: enc=%d auth=%d bond=%d\n",
                      desc->sec_state.encrypted,
                      desc->sec_state.authenticated,
                      desc->sec_state.bonded);
        if (!desc->sec_state.encrypted) {
            // Pairing fehlgeschlagen/abgelehnt → Verbindung beenden
            NimBLEDevice::getServer()->disconnect(desc->conn_handle);
        }
    }
    uint32_t onPassKeyRequest() override {           // DISPLAY_ONLY: wir zeigen, Client tippt
        return BLE_PASSKEY;
    }
};

class WomoTxCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc,
                     uint16_t subValue) override {
        // Notify-Abo nur über verschlüsselte UND authentifizierte
        // Verbindung akzeptieren — sonst bleibt s_subscribed false
        // und es wird schlicht nichts gesendet (Telemetrie-Schutz).
        bool secure = desc->sec_state.encrypted && desc->sec_state.authenticated;
        s_subscribed = (subValue & 0x0001) && secure;
        Serial.printf("[BLE] Subscribe=%u secure=%d → TX %s\n",
                      subValue, secure, s_subscribed ? "aktiv" : "inaktiv");
    }
};

class WomoRxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        // Stack erzwingt WRITE_ENC|WRITE_AUTHEN — hier kommen nur
        // authentifizierte Writes an. Nur Zeilenassemblierung!
        NimBLEAttValue v = c->getValue();
        rx_feed(v.data(), v.length());
    }
};

// ── Senden (ausschließlich ws_task-Kontext) ───────────────────
// Fragmentiert in Chunks von (Peer-MTU − 3) Byte, newline-Framing.
//
// v5.6.3: rc-GEPRÜFTER Direktversand über die NimBLE-Host-API statt
// NimBLECharacteristic::notify(). notify() ist void und verschluckt
// in 1.4.3 zwei Fehlerpfade STILL (im Quellcode verifiziert):
//  1) ble_hs_mbuf_from_flat() → NULL bei msys-Pool-Erschöpfung
//     (Default 12 × 292 B ≈ 3,5 KB; ein Live-Frame-Burst von 3–4
//     Chunks à 514 B belegt ~2 Blöcke je Chunk, und die Pakete
//     bleiben bis zum nächsten Connection-Event von 30–50 ms im
//     Host gepuffert).
//  2) notify() reicht NULL ungeprüft an ble_gatts_notify_custom()
//     weiter — NULL bedeutet dort "Attributwert-Store lesen und
//     senden" (ble_gattc.c Z. 4172ff.): ein leerer/stehengebliebener
//     Frame wird MITTEN in die Zeile injiziert. Weitere ENOMEM tiefer
//     im Stack (ble_att_cmd_get/ble_att_tx) verwerfen Chunks still.
// Folge: '\n'-Assemblierung bei Mehrchunk-Frames (Live/Buffer)
// deterministisch zerstört; Ein-Chunk-Frames (resp/params ≤ 512 B)
// unauffällig — exakt das Fehlerbild "nur Einstellungen-Tab geht".
//
// ble_notify_chunk(): mbuf selbst allozieren (NIE NULL weiterreichen),
// rc auswerten, bei ENOMEM/EAGAIN mit Backoff wiederholen — der Pool
// leert sich mit jedem Connection-Event. Diagnose über Retry-/Fail-
// Zähler im Serial-Log: steht dort "alle Chunks rc=0", ist der
// Firmware-TX-Pfad beweisbar sauber.
static uint32_t s_txRetries = 0;   // kumulierte Wiederholungen (Diagnose)
static uint32_t s_txFails   = 0;   // abgebrochene Frames (Diagnose)

static int ble_notify_chunk(const uint8_t* p, size_t n) {
    const uint16_t handle = s_txChar->getHandle();
    for (int attempt = 0; attempt < BLE_TX_RETRY_MAX; attempt++) {
        if (!s_subscribed) return BLE_HS_ENOTCONN;   // Disconnect → sofort raus
        os_mbuf* om = ble_hs_mbuf_from_flat(p, n);
        int rc = om ? ble_gatts_notify_custom(s_connHandle, handle, om)
                    : BLE_HS_ENOMEM;                 // NIE NULL weiterreichen!
        if (rc == 0) return 0;
        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EAGAIN)
            return rc;                               // harter Fehler → Abbruch
        s_txRetries++;
        vTaskDelay(pdMS_TO_TICKS(BLE_TX_RETRY_DELAY_MS));
    }
    return BLE_HS_ETIMEOUT;                          // Retries erschöpft
}

static bool ble_send_raw(const char* data, size_t len) {
    if (!s_active || !s_subscribed || !s_txChar || !s_server) return false;
    uint16_t mtu = s_server->getPeerMTU(s_connHandle);
    if (mtu < 23) mtu = 23;
    // v5.6.4: NICHT einfach mtu-3 verwenden — Android kappt empfangene
    // Notify-Werte OS-seitig hart bei BLE_MAX_NOTIFY_LEN (512 B), unab-
    // hängig von der ausgehandelten MTU (s. Define-Kommentar config.h).
    // Bei MTU 517 wäre mtu-3=514 > 512 → Android verwirft die letzten
    // 2 B jedes vollen Chunks STILL, nach erfolgreicher Übertragung —
    // für uns unsichtbar, kein rc-Fehler. Daher immer das Minimum.
    size_t chunk = (size_t)mtu - 3;
    if (chunk > BLE_MAX_NOTIFY_LEN) chunk = BLE_MAX_NOTIFY_LEN;
    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > chunk) take = chunk;
        int rc = ble_notify_chunk((const uint8_t*)(data + off), take);
        if (rc != 0) {
            s_txFails++;
            Serial.printf("[BLE] TX-Abbruch rc=%d @ %u/%u B "
                          "(Frames verworfen: %lu, Retries ges.: %lu)\n",
                          rc, (unsigned)off, (unsigned)len,
                          (unsigned long)s_txFails, (unsigned long)s_txRetries);
            // Framing-Reset (Best-Effort): einzelnes '\n', damit die App
            // die halbe Zeile verwirft, statt sie mit dem nächsten Frame
            // zu verketten. rc bewusst ignoriert — mehr geht hier nicht.
            static const uint8_t nl = '\n';
            ble_notify_chunk(&nl, 1);
            return false;
        }
        off += take;
        if (off < len) vTaskDelay(pdMS_TO_TICKS(5)); // Pacing (kleine MTU)
    }
    return true;
}

static void ble_send_line(const String& s) {
    if (!s_subscribed) return;
    String out = s;
    out += '\n';
    ble_send_raw(out.c_str(), out.length());
}

void ble_notify_live(const String& json) {
    ble_send_line(json);
}

// ── Kommando-Ausführung (ws_task-Kontext) ─────────────────────
static void resp(const char* cmd, bool ok, const char* err = nullptr) {
    String r = "{\"type\":\"resp\",\"cmd\":\"";
    r += cmd; r += "\",\"ok\":"; r += ok ? "true" : "false";
    if (err && err[0]) { r += ",\"err\":\""; r += err; r += "\""; }
    r += "}";
    ble_send_line(r);
}

static void exec_line(const char* line) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, line)) { resp("?", false, "JSON ungültig"); return; }
    const char* cmd = doc["cmd"] | "";

    if (!strcmp(cmd, "live")) {
        s_pushNow = true;                            // Push direkt nach Queue-Drain
        return;                                      // Live-Frame IST die Antwort
    }
    if (!strcmp(cmd, "manual")) {
        // Identische Semantik zu POST /api/manual (v5.4/v5.5).
        String act  = String((const char*)(doc["actuator"] | ""));
        String mode = String((const char*)(doc["mode"]     | ""));
        ManualActuator a;
        if      (act == "dplus") a = MANUAL_DPLUS;
        else if (act == "gel")   a = MANUAL_GEL;
        else if (act == "wr")    a = MANUAL_WR;
        else { resp(cmd, false, "unbekannter Aktor"); return; }
        bool ok;
        if      (mode == "auto") ok = logic_set_manual(a, false, false);
        else if (mode == "on")   ok = logic_set_manual(a, true,  true);
        else if (mode == "off")  ok = logic_set_manual(a, true,  false);
        else { resp(cmd, false, "unbekannter Modus"); return; }
        resp(cmd, ok, ok ? nullptr : "fehlgeschlagen");
        s_pushNow = true;                            // Zustand sofort spiegeln
        return;
    }
    if (!strcmp(cmd, "params_get")) {
        String p = "{\"type\":\"params\",\"data\":";
        p += params_to_json();
        p += "}";
        ble_send_line(p);
        return;
    }
    if (!strcmp(cmd, "params_set")) {
        JsonVariantConst data = doc["data"];
        if (data.isNull()) { resp(cmd, false, "data fehlt"); return; }
        String raw;
        serializeJson(data, raw);
        bool ok = params_apply_json(raw.c_str());    // gemeinsamer Pfad mit POST /api/params
        resp(cmd, ok, ok ? nullptr : "Wert außerhalb Grenzen");
        return;
    }
    if (!strcmp(cmd, "level")) {
        // v5.6.3: Lagesensor-Zustand für den Lage-Tab der App.
        // Antwort-Objekt identisch GET /api/level (~280 B = 1 Chunk);
        // level_to_json() ist bereits cross-task-sicher (AsyncTCP-
        // Handler nutzt denselben Pfad). Konfiguration/Kalibrierung
        // bleibt bewusst WLAN-only (L-SW24).
        String p = "{\"type\":\"level\",\"data\":";
        p += level_to_json();
        p += "}";
        ble_send_line(p);
        return;
    }
    if (!strcmp(cmd, "buffer")) {
        // v5.6.1: PSRAM-Historie — Vertrag mit der App:
        //   Erfolg: {"type":"buffer","data":[…]}   (Array identisch /api/buffer)
        //   Fehler: {"type":"buffer","error":"…"}
        // Parameter-Semantik/Clamps identisch GET /api/buffer
        // (offset Default 0, count Default 900 / max 2000, step min 1).
        // MTU-Guard: bei Default-MTU 23 wären ~120 KB ≈ 6000 Chunks
        // × 5 ms Pacing ≈ 30+ s Blockade des ws_task → abgelehnt.
        // Die App verhandelt 517 (requestMtu), real nie ein Problem.
        uint16_t mtu = s_server ? s_server->getPeerMTU(s_connHandle) : 0;
        if (mtu < BLE_BUFFER_MIN_MTU) {
            ble_send_line("{\"type\":\"buffer\",\"error\":\"MTU zu klein\"}");
            return;
        }
        uint32_t offset = doc["offset"] | 0;
        uint32_t count  = doc["count"]  | 900;
        uint32_t step   = doc["step"]   | 1;
        size_t   len    = 0;
        char* arr = webserver_buffer_json(offset, count, step, &len);   // PSRAM
        if (!arr) {
            ble_send_line("{\"type\":\"buffer\",\"error\":\"OOM PSRAM\"}");
            return;
        }
        // Streaming-Versand ohne Umkopieren: Präfix + Array (PSRAM)
        // + Suffix'\n' als eine '\n'-terminierte Zeile in drei Sends —
        // das Framing hängt nur am '\n', nicht an Notify-Grenzen.
        static const char pfx[] = "{\"type\":\"buffer\",\"data\":";
        bool ok = ble_send_raw(pfx, sizeof(pfx) - 1)
               && ble_send_raw(arr, len)
               && ble_send_raw("}\n", 2);
        free(arr);
        if (!ok) Serial.println("[BLE] buffer-Versand abgebrochen (Disconnect?)");
        return;
    }
    resp(cmd[0] ? cmd : "?", false, "unbekanntes Kommando");
}

void ble_tick() {
    if (!s_active || !s_rxQueue) return;
    char line[BLE_RX_LINE_MAX];
    // Max. Queue-Tiefe pro Tick abarbeiten (obere Schranke, kein Endlos-Drain)
    for (int i = 0; i < BLE_RX_QUEUE_LEN; i++) {
        if (xQueueReceive(s_rxQueue, line, 0) != pdTRUE) break;
        exec_line(line);
    }
    if (s_pushNow && s_subscribed) {
        s_pushNow = false;
        ble_notify_live(webserver_live_json());
    }
}

// ── Schalter (NVS "ble"/"en") ─────────────────────────────────
bool ble_enabled()  { return s_enabled; }
bool ble_active()   { return s_active; }
bool ble_connected(){ return s_connected; }
bool ble_subscribed(){ return s_subscribed; }

void ble_set_enabled(bool en) {
    s_enabled = en;
    blePrefs.putUChar("en", en ? 1 : 0);
    Serial.printf("[BLE] Schalter → %s (wirkt nach Neustart)\n", en ? "AN" : "AUS");
}

String ble_to_json() {
    String j = "{\"en\":";
    j += s_enabled ? "true" : "false";
    j += ",\"active\":";     j += s_active     ? "true" : "false";
    j += ",\"connected\":";  j += s_connected  ? "true" : "false";
    j += ",\"subscribed\":"; j += s_subscribed ? "true" : "false";
    j += ",\"name\":\"" BLE_DEVICE_NAME "\"}";
    return j;
}

// ── Init ──────────────────────────────────────────────────────
void ble_init() {
    blePrefs.begin("ble", false);
    if (!blePrefs.isKey("en")) blePrefs.putUChar("en", DEFAULT_BLE_ENABLED);
    s_enabled = blePrefs.getUChar("en", DEFAULT_BLE_ENABLED) != 0;
    if (!s_enabled) {
        Serial.println("[BLE] Deaktiviert (NVS ble/en=0)");
        return;
    }

    s_rxQueue = xQueueCreate(BLE_RX_QUEUE_LEN, BLE_RX_LINE_MAX);
    if (!s_rxQueue) { Serial.println("[BLE] FEHLER: RX-Queue (Heap?)"); return; }

    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(BLE_PREF_MTU);              // 517 — Client handelt herunter
    // Bonding + MITM + Secure Connections; fester Passkey, wir "zeigen" ihn
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityPasskey(BLE_PASSKEY);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(new WomoServerCallbacks());
    s_server->advertiseOnDisconnect(true);

    NimBLEService* svc = s_server->createService(NUS_SERVICE_UUID);
    s_txChar = svc->createCharacteristic(NUS_TX_UUID,
                    NIMBLE_PROPERTY::NOTIFY);
    s_txChar->setCallbacks(new WomoTxCallbacks());
    NimBLECharacteristic* rx = svc->createCharacteristic(NUS_RX_UUID,
                    NIMBLE_PROPERTY::WRITE    | NIMBLE_PROPERTY::WRITE_NR |
                    NIMBLE_PROPERTY::WRITE_ENC| NIMBLE_PROPERTY::WRITE_AUTHEN);
    rx->setCallbacks(new WomoRxCallbacks());
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setScanResponse(true);                      // Name passt in Scan-Response
    adv->start();

    s_active = true;
    Serial.printf("[BLE] GATT-Server '%s' aktiv (NUS, Passkey-Pairing)\n",
                  BLE_DEVICE_NAME);
    Serial.printf("[BLE] Heap nach Init: %u B\n", ESP.getFreeHeap());
}
