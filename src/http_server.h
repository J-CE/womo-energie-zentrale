// ============================================================
//  http_server.h — Womo Energy Core v5.0
//  AsyncWebServer-Interface: REST-API + WebSocket-Broadcast
//
//  Endpunkte:
//    GET  /api/live      Live-JSON (BMS, MPPT, IO, Logic)
//    GET  /api/params    12 NVS-Parameter
//    POST /api/params    Parameter setzen
//    POST /api/time      Browser-Zeit-Sync (UTC-Epoch)
//    POST /api/reset     Fabrik-Reset Parameter
//    GET  /api/buffer    PSRAM-Ringpuffer (bis 2000 Einträge)
//    GET  /api/sdfiles   SD-Dateiliste
//    GET  /api/sddata    SD-CSV-Streaming
//    GET  /api/wifi      Heim-WLAN-Status (SSID/verbunden/IP/RSSI, ohne PW)
//    POST /api/wifi      Heim-WLAN setzen (ssid/pass → NVS, sofort aktiv)
//    GET  /api/level      Lagesensor-Live-Zustand (Neigung, Keile)
//    GET  /api/levelcfg   Lagesensor-Konfiguration
//    POST /api/levelcfg   Lagesensor-Konfiguration setzen
//    POST /api/levelcalib Lagesensor kalibrieren (Null-Offset setzen/löschen)
//    WS   /ws            Push alle 2s
// ============================================================
#pragma once
#include <Arduino.h>

void webserver_init();
void webserver_broadcast();