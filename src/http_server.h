// ============================================================
//  http_server.h — Womo Energy Core v5.6.0
//  AsyncWebServer-Interface: REST-API + WebSocket-Broadcast
//
//  Endpunkte:
//    GET  /api/live      Live-JSON (BMS, MPPT, IO, Logic)
//    GET  /api/params    NVS-Parameter
//    POST /api/params    Parameter setzen
//    POST /api/manual    Manueller Aktor-Override (Auto/Ein/Aus, v5.4)
//    POST /api/time      Browser-Zeit-Sync (UTC-Epoch)
//    POST /api/reset     Fabrik-Reset Parameter
//    GET  /api/buffer    PSRAM-Ringpuffer (bis 2000 Einträge)
//    GET  /api/sdfiles   SD-Dateiliste
//    GET  /api/sddata    SD-CSV-Streaming
//    GET  /api/tz        Zeitzone (POSIX-TZ-String + Abkürzung)
//    POST /api/tz        Zeitzone setzen
//    GET  /api/wifi      Heim-WLAN-Status (SSID/verbunden/IP/RSSI, ohne PW)
//    POST /api/wifi      Heim-WLAN setzen (ssid/pass → NVS, sofort aktiv)
//    GET  /api/level      Lagesensor-Live-Zustand (Neigung, Keile)
//    GET  /api/levelcfg   Lagesensor-Konfiguration
//    POST /api/levelcfg   Lagesensor-Konfiguration setzen
//    POST /api/levelcalib Lagesensor kalibrieren (Null-Offset setzen/löschen)
//    GET  /api/ble       BLE-Status (en/active/connected/…)   (v5.6.0)
//    POST /api/ble       BLE ein-/ausschalten {"en":0|1} → Reboot (v5.6.0)
//    GET  /api/ota       OTA-Info (Version, Partition, Größen) (v5.4.1)
//    POST /api/ota       Web-OTA-Upload: ?type=fw|fs (Firmware/Dashboard)
//    WS   /ws            Push alle 2s
// ============================================================
#pragma once
#include <Arduino.h>

void webserver_init();
void webserver_broadcast();

// v5.6.0: Live-JSON (identisch zum WS-Broadcast, inkl. "type":"live")
// für andere Module — konkret BLE {"cmd":"live"} (ble.cpp).
String webserver_live_json();
