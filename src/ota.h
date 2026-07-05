// ============================================================
//  ota.h — Womo Energy Core v5.6.0
//  v5.6.0: ota_schedule_reboot() exportiert — sicherer Neustart-
//  Pfad (Ringpuffer-Sicherung) auch für andere Module (BLE-Toggle).
//  Web-OTA: Firmware- und Dashboard-Update per Browser-Upload
//
//  Zwei Update-Typen (POST /api/ota?type=…):
//    fw : Firmware-Binary  → inaktive App-Partition (U_FLASH)
//    fs : LittleFS-Image   → spiffs-Partition       (U_SPIFFS)
//
//  Ablauf:
//    Upload läuft im AsyncTCP-Task (ota_handle_upload, chunked).
//    Nach Erfolg antwortet ota_handle_request und setzt einen
//    Deferred-Reboot (~1,5 s) — ausgeführt in ota_tick() aus
//    loop(), NIE im AsyncTCP-Handler. Vor dem Neustart wird der
//    Ringpuffer per logger_emergency_back_up() auf SD gesichert.
//
//  Voraussetzung: Dual-App-Partitionstabelle (ota_0/ota_1/otadata)
//  — siehe partitions_16mb.csv v5.4.1.
// ============================================================
#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Boot-Diagnose: laufende Partition + OTA-Fähigkeit loggen.
void ota_init();

// GET /api/ota — Version, laufende Partition, Größen als JSON.
String ota_to_json();

// POST /api/ota — Upload-Callback (5. Argument von server.on()).
// Läuft chunked im AsyncTCP-Task; schreibt direkt in die Flash-
// Zielpartition (Update.write).
void ota_handle_upload(AsyncWebServerRequest* req, String filename,
                       size_t index, uint8_t* data, size_t len, bool final);

// POST /api/ota — onRequest-Callback (nach Upload-Abschluss).
// Sendet die JSON-Antwort und plant bei Erfolg den Neustart.
void ota_handle_request(AsyncWebServerRequest* req);

// Aus loop() aufrufen: führt den geplanten Neustart aus
// (Ringpuffer-Sicherung + ESP.restart()).
void ota_tick();

// v5.6.0: Deferred-Reboot von außen planen (z. B. BLE-Toggle).
// Ausführung wie beim OTA-Reboot in ota_tick() aus loop() —
// darf aus AsyncTCP-Handlern aufgerufen werden (setzt nur Flag).
void ota_schedule_reboot(uint32_t delayMs);
