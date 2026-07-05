// ============================================================
//  ble.cpp — Womo Energy Core v5.6.0
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
// Fragmentiert in Chunks von (Peer-MTU − 3) Byte. NimBLE 1.4.x:
// notify() liefert void (kein Congestion-Feedback) — daher festes
// Pacing zwischen Chunks; die NimBLE-interne Queue puffert. Bei
// MTU 517 sind es ~3 Chunks je Live-Frame, bei MTU 23 ~70 → mit
// 5 ms Pacing ≈ 350 ms, unkritisch im 2-s-Tick des ws_task.
static bool ble_send_raw(const char* data, size_t len) {
    if (!s_active || !s_subscribed || !s_txChar || !s_server) return false;
    uint16_t mtu = s_server->getPeerMTU(s_connHandle);
    if (mtu < 23) mtu = 23;
    const size_t chunk = (size_t)mtu - 3;
    size_t off = 0;
    while (off < len) {
        if (!s_subscribed) return false;             // Disconnect während Send
        size_t take = len - off;
        if (take > chunk) take = chunk;
        s_txChar->setValue((const uint8_t*)(data + off), take);
        s_txChar->notify();
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
