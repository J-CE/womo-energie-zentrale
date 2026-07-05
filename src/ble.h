// ============================================================
//  ble.h — Womo Energy Core v5.6.0
//  BLE GATT-Server: Nordic UART Service (NUS), NimBLE-Arduino 1.4.x
//
//  Protokoll (newline-delimited JSON, beide Richtungen):
//    TX (Notify, alle 2s + auf Anforderung):
//      Live-JSON wie WebSocket, zusätzlich "type":"live"
//      Antworten: {"type":"resp","cmd":"…","ok":true|false[,"err":"…"]}
//    RX (Write, authentifiziert):
//      {"cmd":"live"}                                       → Sofort-Push
//      {"cmd":"manual","actuator":"dplus|gel|wr","mode":"auto|on|off"}
//      {"cmd":"params_get"}                                 → {"type":"params",…}
//      {"cmd":"params_set","data":{…}}                      → wie POST /api/params
//    NICHT über BLE: OTA, SD-Streaming, WLAN-Konfig, Zeitzone.
//
//  Sicherheit: Bonding + MITM + Secure Connections, fester
//  6-stelliger Passkey (BLE_PASSKEY aus secrets.h, DISPLAY_ONLY).
//  RX-Writes werden vom Stack nur verschlüsselt+authentifiziert
//  akzeptiert (WRITE_ENC|WRITE_AUTHEN); TX-Notify nur an Clients,
//  deren Subscribe über eine authentifizierte Verbindung kam.
//
//  Fragmentierung: Chunks von (ausgehandelte MTU − 3) Byte,
//  Frames mit '\n' terminiert — Empfänger sammelt bis '\n'.
//  setMTU(517): schnellste Variante, sofern der Client (Android:
//  requestMtu(517)) eine große MTU aushandelt.
//
//  Kontext-Regeln (analog AsyncTCP-Disziplin):
//   • NimBLE-Callbacks (Host-Task) verarbeiten NICHTS — RX-Zeilen
//     gehen in eine FreeRTOS-Queue.
//   • Ausführung + ALLE Sendungen (Broadcast, Antworten) laufen
//     ausschließlich im ws_task (ble_tick / ble_notify_live).
//     → genau ein Sende-Kontext, keine Locks nötig.
//
//  Ein-/Aus-Schalter: NVS-Namespace "ble", Key "en" (Default 1).
//  Änderung via POST /api/ble → deferred Reboot (ota_schedule_reboot),
//  weil sauberes De-Init des BLE-Stacks zur Laufzeit fehleranfällig ist.
// ============================================================
#pragma once
#include <Arduino.h>

// Liest NVS-Schalter; falls aktiviert: NimBLE-Stack, Security,
// NUS-Service und Advertising starten. Vor den Tasks aufrufen.
void ble_init();

// NVS-Schalter lesen/setzen. Setzen wirkt erst nach Neustart.
bool ble_enabled();
void ble_set_enabled(bool en);

// Läuft der Stack (init erfolgreich + aktiviert)?
bool ble_active();

// Client verbunden? / TX-Subscribe (authentifiziert) aktiv?
bool ble_connected();
bool ble_subscribed();

// Live-JSON an abonnierten Client senden (ws_task-Kontext!).
// Hängt '\n' an und fragmentiert nach ausgehandelter MTU.
void ble_notify_live(const String& json);

// RX-Queue abarbeiten: Kommandos ausführen, Antworten senden.
// Ausschließlich aus dem ws_task aufrufen (2s-Tick).
void ble_tick();

// Status als JSON für GET /api/ble:
// {"en":…,"active":…,"connected":…,"subscribed":…,"name":"…"}
String ble_to_json();
