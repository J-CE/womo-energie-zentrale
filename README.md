# Womo Energy Core v5.6.5

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
| Lagesensor (Wasserwaage) | MMA8452Q, 3-Achs-Beschleunigungssensor | I2C, gemeinsamer Bus mit DS3231 (optional) |
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
- **Elektronische Wasserwaage** (optional, MMA8452Q): Neigungsmessung (Roll/Pitch), automatische Keilhöhen-Berechnung pro Rad, eigener Dashboard-Tab mit Libellen-Anzeige und Kalibrierung
- **Multi-SSID Heim-WLAN** (v5.5.1): bis zu 3 Heimnetze speicherbar — das Modul verbindet sich per Scan mit dem stärksten bekannten Netz, AP bleibt parallel aktiv
- **mDNS + NTP** (v5.5.2): Dashboard ohne IP unter `http://womo.local` erreichbar (AP wie Heimnetz); bei Heimnetz-Verbindung stellt sich die Uhr automatisch per NTP — der Browser-Sync bleibt als Fallback im AP-Betrieb erhalten. Der letzte NTP-Sync wird im Zeitzone-Tab angezeigt (v5.5.3)
- **Schaltkriterien v5.5**: D+/Gel schalten nur noch über harte Bedingungen ab (Landstrom, BMS ungültig, SoC-Schwelle) — MPPT-Ausfälle und PV-Einbrüche schalten nichts mehr ab; EIN bei genug PV **oder** MPPT-Float. Wechselrichter: Einschalten nur manuell, Automatik nur als Schutz-Abschaltung. Manuelles AUS ist dauerhaft und reboot-fest (NVS)
- **Web-OTA** (v5.4.1): Firmware- und Dashboard-Updates direkt aus dem Browser (System-Tab), ohne PC/USB — Dual-App-Partitionslayout, Upload mit Fortschrittsanzeige, automatischer Neustart mit Ringpuffer-Sicherung
- **Bluetooth (BLE)** (v5.6.0): GATT-Server `WomoEnergy` mit Nordic UART Service — Live-Daten alle 2 s als newline-terminiertes JSON (identisch zum WebSocket, plus `"type":"live"`) und Kommandos (Sofort-Push, manueller Aktor-Override, Parameter lesen/schreiben). Passkey-Pairing (Bonding + MITM + Secure Connections, Schlüssel in `secrets.h`); abschaltbar im Einstellungen-Tab (NVS, Neustart). Seit v5.6.1 zusätzlich `buffer`-Kommando: PSRAM-Historie über BLE (Array + Parameter identisch `/api/buffer`, Antwort `{"type":"buffer","data":[…]}`). Seit v5.6.3: rc-geprüfter TX-Pfad (Raw-Host-API mit Backoff-Retry statt fehlerblindem `notify()`, msys-Pool 30 Blöcke) — behebt still verlorene/korrupte Mehrchunk-Frames — sowie `level`-Kommando (Lagesensor-Zustand für den Lage-Tab der App, identisch `/api/level`; Lage-Konfiguration/Kalibrierung bleibt WLAN-only). Seit v5.6.4: Chunk-Größe hart auf 512 B gedeckelt (statt MTU−3=514 B) — Android kappt empfangene Notify-Werte seit Android 13 OS-intern bei genau diesem Limit und verwarf so lautlos 2 B je vollem Chunk, was Mehrchunk-Frames (Live/Buffer) korrumpierte, während Einchunk-Frames (params/level) unauffällig blieben. Seit v5.6.5 zeigt das Dashboard den WLAN(STA)-Status (SSID/IP/RSSI) auch im BLE-Betrieb an — die Daten kamen bereits vorher transportunabhängig aus demselben Live-JSON, es fehlte nur die Anzeige. WLAN bleibt für Konfiguration, OTA und SD-Verlauf zuständig
- **BMS-Frame-Parsing v5.6.5**: Framelänge/-ende werden jetzt deterministisch aus dem protokollkonformen LENGTH-Feld berechnet (Quelle: offizielles JK/JiKong-Protokoll-PDF) statt über mehrere Kandidaten geraten — verhindert seltene, aber reproduzierbare Fantasiewerte (z. B. Strom/Leistung) durch fälschlich als gültig akzeptierte Alt/Neu-Frame-Vermischungen. Zusätzliche Plausibilitätsprüfung (Spannung/Strom/SoC/Temperatur) als zweite, unabhängige Schutzschicht vor der Übernahme in den Live-Zustand — dieselbe Plausibilitätsprüfung ist auch in der CAN-Alternative (`bmscan.cpp`) aktiv; der Framelängen-Bug selbst betrifft nur RS485, da CAN bereits vollständige, hardware-CRC-geprüfte Frames liefert

Die vollständige, nummerierte Anforderungsliste steht in [`Software_Lasten_Pflichtenheft.txt`](./Software_Lasten_Pflichtenheft.txt).

## Projektstruktur

```
├── platformio.ini          # Build-Konfiguration (gepinnte Library-Versionen)
├── partitions_16mb.csv     # Eigene Partitionstabelle (Dual-App OTA)
├── src/
│   ├── config.h            # GPIO-Pins, Default-Parameter, Tuning-Konstanten
│   ├── secrets.h           # WLAN-Zugangsdaten + BLE-Passkey (NICHT im Repo, s. unten)
│   ├── main.cpp            # Setup, FreeRTOS-Tasks, Hardware-Watchdog
│   ├── bms.h / .cpp        # JK-BMS RS485-Parser
│   ├── bmscan.h / .cpp     # JK-BMS CAN-Parser (Alternative zu bms.cpp)
│   ├── mppt.h / .cpp       # VE.Direct Text-Parser + HEX-TX
│   ├── inverter.h / .cpp   # Wechselrichter-Status (Renogy NVS, 2 LED-Eingänge)
│   ├── level.h / .cpp      # Lagesensor / Wasserwaage (MMA8452Q, optional)
│   ├── io.h / .cpp         # GPIO-Aktoren, Landstrom-Sensor, RGB-LED
│   ├── logic.h / .cpp      # Schaltlogik
│   ├── logger.h / .cpp     # PSRAM-Ringpuffer + SD-Logging
│   ├── clock.h / .cpp      # Zeitdienst (DS3231-RTC)
│   ├── watchdog.h / .cpp   # Software-Modulüberwachung
│   ├── ota.h / .cpp        # Web-OTA (Firmware- & Dashboard-Update per Browser)
│   ├── ble.h / .cpp        # BLE GATT-Server (NUS, Live-Push + Kommandos)
│   └── http_server.h / .cpp# Webserver, WebSocket, REST-API
└── data/
    └── index.html          # Dashboard (LittleFS, offline-fähig)
```

## Build & Flash

Voraussetzung: [PlatformIO](https://platformio.org/) (über VS Code oder CLI).

**1. Zugangsdaten anlegen** (einmalig, wird nicht versioniert):

```cpp
// src/secrets.h
#pragma once
#define WIFI_AP_SSID     "DeinSSID"
#define WIFI_AP_PASSWORD "DeinPasswort"
#define BLE_PASSKEY      123456        // 6-stellig — beim BLE-Pairing abgefragt (v5.6.0)
```

und in `config.h` per `#include "secrets.h"` einbinden. Vorlage: `secrets_h.example`.

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

**6. Folge-Updates per Browser (Web-OTA, ab v5.4.1):**

Nach der Erstinstallation sind USB-Flashvorgänge nicht mehr nötig. Im Dashboard unter **System → Firmware-Update**:

| Update-Typ | Datei | Erzeugen mit |
|---|---|---|
| Firmware | `.pio/build/esp32-s3-devkitc-1/firmware.bin` | `pio run` |
| Dashboard (LittleFS) | `.pio/build/esp32-s3-devkitc-1/littlefs.bin` | `pio run -t buildfs` |

Nach erfolgreichem Upload sichert das Gerät den Ringpuffer auf SD und startet automatisch neu (~10 s). Bei einem fehlgeschlagenen Firmware-Update bootet weiterhin die alte, unveränderte Partition — das System bleibt funktionsfähig. Bei einem fehlgeschlagenen Dashboard-Update den Upload einfach aus der noch geladenen Seite wiederholen.

> ⚠️ **Einmalige Migration von Versionen < v5.4.1** (Single-App-Partitionslayout): Der Wechsel auf das Dual-App-Layout erfordert ein letztes Mal USB: `pio run -t erase` → `pio run -t upload` → `pio run -t uploadfs`. Der Erase löscht **alle** NVS-Namespaces — vorher Schaltparameter, Heim-WLAN, Zeitzone und Lage-Kalibrierung notieren und danach neu einrichten.

## Status / Roadmap

**Läuft bereits:** WLAN-AP, Webserver/Dashboard, LittleFS, SD-Karten-Logging, DS3231-RTC, RGB-Status-LED, Lagesensor (MMA8452Q verbaut, Firmware inkl. REST-API vollständig, ab v5.5 inkl. Überkopf-Einbau), Web-OTA (Firmware + Dashboard, v5.4.1), mDNS `womo.local` + NTP-Zeitsync inkl. Sync-Statusanzeige (v5.5.2/v5.5.3), BLE GATT-Server mit Passkey-Pairing inkl. Historie-Abruf und Lage-Kommando (v5.6.0–v5.6.3 — Funkverifikation mit realer App steht aus).

**Aktueller Hardware-Meilenstein:** JK-BMS-Anbindung umgestellt von RS485 (3-Pin JST-GH-Stecker nicht beschaffbar) auf direkte UART-TTL-Verdrahtung am GPS-Port (4-Pin, vorhandener Steckertyp) — kein MAX485 mehr nötig. Pinpegel am Gerät verifiziert (Pin 2/3 ~2,5V, kein VBAT). Offen: erste Kommunikationsverifikation; FW V11.287H liegt im Bereich, in dem manche Geräte über den GPS-Port keine Antwort mehr liefern — Fallback wäre die CAN-Variante (bmscan.cpp) oder eine MAX485-Doppelbrücke.

**Geplant (Phase 2):**
- Lagesensor: Funktionstest am Fahrzeug (Kalibrierung in der Ebene, Keilwerte gegen reale Schräglage plausibilisieren)
- Renogy-Wechselrichter: Status-Eingänge (Power/Alarm) eingebaut — Pinbelegung/Spannungsteiler noch am realen Gerät zu verifizieren
- Remote-Telemetrie (SIM7080G oder Walter-Board, MQTT über TLS)
- Android-App (nativer BLE-Container: `index.html` in WebView, BLE-Bridge auf die bestehende `render(D)`-Pipeline)
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
| NimBLE-Arduino (gepinnt 1.4.x) | Apache-2.0 | github.com/h2zero/NimBLE-Arduino |
| FS / SD / LittleFS | LGPL-2.1 (Teil von arduino-esp32) | github.com/espressif/arduino-esp32 |

Es gelten jeweils die Originallizenzen der Bibliotheken; sie werden hier nur referenziert, nicht im Repo mitgeliefert oder verändert.
