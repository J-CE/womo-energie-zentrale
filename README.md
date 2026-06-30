# Womo Energy Core v5.3

Eigenentwickeltes Energiemanagement-System für ein Wohnmobil, basierend auf einem ESP32-S3. Überwacht BMS und MPPT-Laderegler, steuert Verbraucher/Lader automatisch nach Ladezustand und Solarleistung, loggt historische Daten und liefert ein komplett offline-fähiges Web-Dashboard.

> Privates Hobby-Projekt im Aufbau — aktuell in der Hardware-Inbetriebnahme. Siehe [Status](#status--roadmap) unten.

## Hardware

| Komponente | Modell | Anbindung |
|---|---|---|
| MCU | ESP32-S3 DevKitC-1 N16R8 (16MB Flash, 8MB PSRAM) | — |
| BMS | JK B2A8S20P, 4S LFP 280Ah | UART-TTL direkt am GPS-Port (UART1) — optional CAN |
| MPPT-Laderegler | Victron MPPT 100/30 | VE.Direct, bidirektional (UART2) |
| RTC | DS3231 | I2C |
| Speicher | SD-Karte 16GB FAT32 | SPI |
| Kühlschrank (D+) | Joy-it COM-MOSFET (IRF9540N) | GPIO, High-Side |
| Starterbatterie-Lader | Joy-it COM-MOSFET (IRF9540N) | GPIO, High-Side |
| Wechselrichter | Renogy 12V/2000W mit NVS (Netzvorrangschaltung) | Fernbedienungsport: Power-/Alarm-LED als GPIO-Eingang, Schalterkontakt als GPIO-Ausgang (Optokoppler) |
| Landstrom-Erkennung | Spannungsteiler 2k/1,5k von 5V | GPIO, Input |
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
- **JK-BMS-Anbindung wahlweise über UART-TTL (GPS-Port) oder CAN** (Compile-Zeit-Umschaltung, identischer Datenoutput)

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
│   ├── inverter.h / .cpp   # Wechselrichter-Status (Renogy NVS, 2 LED-Eingänge)
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

**Aktueller Hardware-Meilenstein:** JK-BMS-Anbindung umgestellt von RS485 (3-Pin JST-GH-Stecker nicht beschaffbar) auf direkte UART-TTL-Verdrahtung am GPS-Port (4-Pin, vorhandener Steckertyp) — kein MAX485 mehr nötig. Pinpegel am Gerät verifiziert (Pin 2/3 ~2,5V, kein VBAT). Offen: erste Kommunikationsverifikation; FW V11.287H liegt im Bereich, in dem manche Geräte über den GPS-Port keine Antwort mehr liefern — Fallback wäre die CAN-Variante (bmscan.cpp) oder eine MAX485-Doppelbrücke.

**Geplant (Phase 2):**
- Renogy-Wechselrichter: Status-Eingänge (Power/Alarm) eingebaut — Pinbelegung/Spannungsteiler noch am realen Gerät zu verifizieren
- Remote-Telemetrie (SIM7080G oder Walter-Board, MQTT über TLS)
- Eigene Delphi-FireMonkey-App (Android/Windows)

Details und vollständiger Änderungsverlauf: [`Software_Lasten_Pflichtenheft.txt`](./Software_Lasten_Pflichtenheft.txt).

## Lizenz

Der Quellcode in diesem Repository ist urheberrechtlich geschützt. Das Repository ist auf GitHub öffentlich einsehbar, **es wird jedoch keine Lizenz zur Weiterverwendung vergeben** — alle Rechte verbleiben beim Autor. Ansehen, Klonen und Forken über GitHub ist möglich; eine Weiterverbreitung, Veränderung oder (kommerzielle) Nutzung des eigenen Codes über die GitHub-Standardrechte hinaus ist ohne ausdrückliche Zustimmung des Autors nicht gestattet.

### Drittanbieter-Bibliotheken

Die Firmware nutzt folgende Open-Source-Bibliotheken (unverändert, dynamisch über PlatformIO `lib_deps` eingebunden, nicht Teil dieses Repos):

| Bibliothek | Lizenz | Quelle |
|---|---|---|
| ESPAsyncWebServer (ESP32Async-Fork) | LGPL-3.0 | github.com/ESP32Async/ESPAsyncWebServer |
| AsyncTCP (ESP32Async-Fork) | LGPL-3.0 | github.com/ESP32Async/AsyncTCP |
| ArduinoJson | MIT | github.com/bblanchon/ArduinoJson |
| FS / SD / LittleFS | LGPL-2.1 (Teil von arduino-esp32) | github.com/espressif/arduino-esp32 |

Es gelten jeweils die Originallizenzen der Bibliotheken; sie werden hier nur referenziert, nicht im Repo mitgeliefert oder verändert.
