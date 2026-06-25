# Womo Energy Core v5.1

Eigenentwickeltes Energiemanagement-System für ein Wohnmobil, basierend auf einem ESP32-S3. Überwacht BMS und MPPT-Laderegler, steuert Verbraucher/Lader automatisch nach Ladezustand und Solarleistung, loggt historische Daten und liefert ein komplett offline-fähiges Web-Dashboard.

> Privates Hobby-Projekt im Aufbau — aktuell in der Hardware-Inbetriebnahme. Siehe [Status](#status--roadmap) unten.

## Hardware

| Komponente | Modell | Anbindung |
|---|---|---|
| MCU | ESP32-S3 DevKitC-1 N16R8 (16MB Flash, 8MB PSRAM) | — |
| BMS | JK B2A8S20P, 4S LFP 280Ah | RS485 (UART1) — optional CAN |
| MPPT-Laderegler | Victron MPPT 100/30 | VE.Direct, bidirektional (UART2) |
| RTC | DS3231 | I2C |
| Speicher | SD-Karte 16GB FAT32 | SPI |
| Kühlschrank (D+) | Joy-it COM-MOSFET (IRF9540N) | GPIO, High-Side |
| Starterbatterie-Lader | Joy-it COM-MOSFET (IRF9540N) | GPIO, High-Side |
| Wechselrichter-Fernsteuerung | PC817-Optokoppler | GPIO |
| Landstrom-Erkennung | PC817-Optokoppler | GPIO, Input |
| Status | On-board WS2812 RGB-LED + externe Status-LED | GPIO |

Die vollständige GPIO-Belegung steht in `src/config.h` (P-SW03 im Lastenheft).

## Funktionsumfang

- **Automatische Schaltlogik** für Kühlschrank (D+), Starterbatterie-Lader (Gel) und Wechselrichter-Fernsteuerung, jeweils nach SoC- und PV-Schwellwerten mit Hysterese und Flatter-Schutz
- **Landstrom-Erkennung** schaltet Verbraucher bei Netzbetrieb sofort ab
- **Live-Webdashboard** (WLAN-Access-Point, komplett offline) mit Echtzeit-Werten, Live-Graph und Verlaufsansicht aus SD-Logs
- **DS3231-Hardware-Uhr** als führende Zeitquelle, übersteht Stromausfälle eigenständig
- **48h-Ringpuffer im PSRAM** + Minuten-gemittelte CSV-Logs auf SD
- **Temperaturkompensiertes Laden**: BMS-Zelltemperatur wird per VE.Direct-HEX an den MPPT gesendet
- **Zweistufiger Watchdog** (Hardware + Software) mit Fail-Safe-GPIO-Zuständen
- **RGB-Status-LED** zeigt BMS-/MPPT-Fehler, SoC, Landstrom und alle Aktoren gleichzeitig in einem Rundlauf-Muster an
- **JK-BMS-Anbindung wahlweise über RS485 oder CAN** (Compile-Zeit-Umschaltung, identischer Datenoutput)

Die vollständige, nummerierte Anforderungsliste steht in [`Software_Lasten_Pflichtenheft.txt`](./Software_Lasten_Pflichtenheft.txt).

## Projektstruktur

```
├── platformio.ini          # Build-Konfiguration (gepinnte Library-Versionen)
├── partitions_16mb.csv     # Eigene Partitionstabelle (kein OTA)
├── src/
│   ├── config.h            # GPIO-Pins, Default-Parameter, Tuning-Konstanten
│   ├── secrets.h           # WLAN-Zugangsdaten (NICHT im Repo, siehe unten)
│   ├── main.cpp            # Setup, FreeRTOS-Tasks, Hardware-Watchdog
│   ├── bms.h / .cpp        # JK-BMS RS485-Parser
│   ├── bmscan.h / .cpp     # JK-BMS CAN-Parser (Alternative zu bms.cpp)
│   ├── mppt.h / .cpp       # VE.Direct Text-Parser + HEX-TX
│   ├── inverter.h / .cpp   # Wechselrichter-Sniffer (Stub, Phase 2)
│   ├── io.h / .cpp         # GPIO-Aktoren, Landstrom-Sensor, RGB-LED
│   ├── logic.h / .cpp      # Schaltlogik
│   ├── logger.h / .cpp     # PSRAM-Ringpuffer + SD-Logging
│   ├── clock.h / .cpp      # Zeitdienst (DS3231-RTC)
│   ├── watchdog.h / .cpp   # Software-Modulüberwachung
│   └── http_server.h / .cpp# Webserver, WebSocket, REST-API
└── data/
    └── index.html          # Dashboard (LittleFS, offline-fähig)
```

## Build & Flash

Voraussetzung: [PlatformIO](https://platformio.org/) (über VS Code oder CLI).

**1. WLAN-Zugangsdaten anlegen** (einmalig, wird nicht versioniert):

```cpp
// src/secrets.h
#pragma once
#define WIFI_AP_SSID     "DeinSSID"
#define WIFI_AP_PASSWORD "DeinPasswort"
```

und in `config.h` per `#include "secrets.h"` einbinden.

**2. Firmware bauen und flashen:**

```bash
pio run -t upload
```

> ⚠️ Bei Boot-Loop (Endlos-Reset ohne App-Ausgabe): Das ESP32-S3 N16R8 benötigt sowohl `board_upload.flash_size = 16MB` als auch `board_build.flash_size = 16MB` in `platformio.ini`. Fehlt einer der beiden Schlüssel, verwirft der Bootloader die 16MB-Partitionstabelle. Nach einer Änderung an den Partitionen: `pio run -t erase` vor dem nächsten Upload.

**3. Dashboard-Dateisystem separat flashen:**

```bash
pio run -t uploadfs
```

**4. Serieller Monitor:**

```bash
pio device monitor
```

Nach dem Flashen ggf. Reset-Taste drücken (USB-CDC verliert beim Upload die Verbindung).

**5. Optional: CAN statt RS485 für die BMS-Anbindung** — in `platformio.ini`:

```ini
build_flags = -D BMS_USE_CAN
build_src_filter = +<*> -<bms.cpp>
```

Details siehe Lastenheft, Abschnitt P-SW17.

## Status / Roadmap

**Läuft bereits:** WLAN-AP, Webserver/Dashboard, LittleFS, SD-Karten-Logging, DS3231-RTC, RGB-Status-LED.

**Aktueller Hardware-Meilenstein:** Physische RS485-Verkabelung zwischen MAX485-Modul und JK-BMS (3-Pin A/B/GND am CAN/RS485-Port). Bis dahin ist ein Red-Blink-Fehler am BMS-Kanal erwartetes Verhalten.

**Geplant (Phase 2):**
- Edecoa-Wechselrichter-Integration (wartet auf Hardware mit zugänglichem RS485/Modbus- oder EG8010-Interface)
- Remote-Telemetrie (SIM7080G oder Walter-Board, MQTT über TLS)
- Eigene Delphi-FireMonkey-App (Android/Windows)

Details und vollständiger Änderungsverlauf: [`Software_Lasten_Pflichtenheft.txt`](./Software_Lasten_Pflichtenheft.txt).

## Lizenz

Privates Projekt, keine Lizenz für die Weiterverwendung vergeben. Alle Rechte beim Autor.
