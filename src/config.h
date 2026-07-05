// ============================================================
//  config.h — Womo Energy Core v5.6.0
//  Zielplattform: ESP32-S3 DevKitC-1 N16R8
//
//  v5.6.0: BLE GATT-Server (NUS). Neuer Define-Block "Bluetooth
//          Low Energy" (Name, MTU, Queue); BLE_PASSKEY → secrets.h.
//  v5.5.3: NTP-Sync-Status im Dashboard (net.ntp im Live-JSON).
//  v5.5.2: mDNS (womo.local) + NTP-Zeitsync (STA). Neue Defines
//          MDNS_HOSTNAME und NTP_SERVER im Netzwerkdienst-Block.
//
//  GPIO-Reservierungen ESP32-S3 N16R8:
//  ├─ GPIO  0,3      Strapping                  → gesperrt
//  ├─ GPIO  4,5      WR-Status (Renogy NVS Power-/Alarm-LED)
//  ├─ GPIO 19,20     USB D-/D+                  → freigehalten
//  ├─ GPIO 26–37     Intern Flash/PSRAM          → gesperrt
//  ├─ GPIO 39–42     Default-JTAG (MTCK…MTMS)
//  │                 → nutzbar, da USB-JTAG aktiv
//  ├─ GPIO 45,46     Strapping                   → gesperrt
//  ├─ GPIO 48        On-board RGB-LED (WS2812)    → genutzt (Status)
//  └─ GPIO 1–2       I2C DS3231 RTC (reserviert, nicht bestückt)
// ============================================================

#pragma once

// ── Firmware-Version (v5.6.0) ────────────────────────────────
// Zentrale Quelle für Boot-Banner (main.cpp) und /api/ota.
// Bei jedem Release NUR hier ändern (+ Datei-Kopfzeilen).
#define FW_VERSION "5.6.0"

// ============================================================
//  BLOCK 1 — HARDWARE-KONSTANTEN
// ============================================================

// ── Zeitzone ─────────────────────────────────────────────────
// Intern/Storage IMMER UTC-Epoch (DS3231, Ringpuffer, CSV-ts-Spalte,
// SD-Dateinamen). Der lokale Offset gilt nur für Anzeige und die
// lokale CSV-Spalte und kommt aus newlib localtime_r — gesteuert über
// einen vollwertigen POSIX-TZ-String (Sommerzeit/DST automatisch).
// Zur Laufzeit über /api/tz änderbar, persistiert in NVS "clock"/"tz".
// Format: STDoffset[DST[,start[/time],end[/time]]]
//   Europe/Berlin : "CET-1CEST,M3.5.0,M10.5.0/3"
//   Europe/London : "GMT0BST,M3.5.0/1,M10.5.0"
//   Europe/Athens : "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define DEFAULT_TZ                  "CET-1CEST,M3.5.0,M10.5.0/3"   // Europe/Berlin

// ── Serielle Schnittstellen ──────────────────────────────────

#define SERIAL_BAUD                 115200

// JK-BMS direkt per TTL-UART am GPS-Port (UART1) — kein MAX485 mehr
// im Pfad seit der Umstellung vom nicht beschaffbaren RS485-Port
// (s. Anschluss_Anleitung.txt Modul 2). GPIO 15 (ehem. MAX485 DE/RE)
// ist frei und unbeschaltet.
#define UART_BMS_PORT               1
#define UART_BMS_RX                 16
#define UART_BMS_TX                 17
#define UART_BMS_BAUD               115200

// VE.Direct (Victron MPPT) — UART2, bidirektional ab v5.0
//   RX: Spannungsteiler 1kΩ+2kΩ (5V→3,3V)
//   TX: 3,3V direkt, kein Pegelwandler (MPPT-Eingang kompatibel)
//   Text-Protokoll (automatisch alle ~1s, RX)
//   HEX-Protokoll (Temperatur senden, TX)
#define UART_MPPT_PORT              2
#define UART_MPPT_RX                38
#define UART_MPPT_TX                42          // neu v5.0 (war -1)
#define UART_MPPT_BAUD              19200

// Wechselrichter Status (Renogy 12V/2000W mit NVS) — v5.3
// Kein Datenprotokoll vorhanden (anders als bei BMS/MPPT) — der
// RJ11/RJ12-Fernbedienungsport liefert nur zwei rohe LED-Spannungs-
// pegel (Power=grün, Alarm=rot) + einen Schalterkontakt für die
// Fernsteuerung (separat, s. GPIO_OPTO_WR_REMOTE unten).
// ACHTUNG: Pinbelegung am Fernbedienungsport variiert je Renogy-
// Modell/Charge (Forenberichte uneinheitlich) — vor Verkabelung mit
// Multimeter verifizieren (Pegel in beiden Zuständen messen, danach
// ggf. Spannungsteiler-Werte und WR_LED_ACTIVE anpassen).
// GPIO 4/5 gewählt: frei, kein Strapping/JTAG/SPI/UART-Konflikt.
#define GPIO_WR_LED_POWER           4     // Power-LED (grün), über Spannungsteiler
#define GPIO_WR_LED_ALARM           5     // Alarm-LED (rot),  über Spannungsteiler
#define WR_LED_ACTIVE               HIGH  // nach Messung ggf. auf LOW ändern

// ── SPI — SD-Karte ───────────────────────────────────────────
#define SPI_SD_CS                   11
#define SPI_SD_CLK                  12
#define SPI_SD_MOSI                 13
#define SPI_SD_MISO                 14

// ── I2C — DS3231 RTC (bestückt) + optionaler Lagesensor ──────
// EIN gemeinsamer Bus (SDA=GPIO1, SCL=GPIO2). Adressen kollisionsfrei:
//   DS3231 = 0x68 | MMA8452 = 0x1D (SA0=1) bzw. 0x1C (SA0=0).
// Alle Bus-Zugriffe laufen unter g_i2cMutex (siehe clock.h).
#define I2C_RTC_SDA                 1
#define I2C_RTC_SCL                 2

// ── Lagesensor (Wasserwaage, OPTIONAL) — MMA8452 am I2C-Bus ──
// 3-Achs-Accelerometer; misst im Stillstand die Schwerkraft → Neigung.
// Versorgung 3,3V direkt (max. 3,6V), KEIN Pegelwandler/GPIO nötig —
// nur SDA/SCL/3V3/GND. Modul fehlt → Feature meldet "nicht erkannt".
#define LEVEL_I2C_ADDR_PRIMARY      0x1D        // SA0=1 (Default-Breakout)
#define LEVEL_I2C_ADDR_ALT          0x1C        // SA0=0
#define LEVEL_POLL_INTERVAL_MS      250         // 4 Hz — flüssiges Leveling
#define LEVEL_TOLERANCE_DEG         1.2f        // |roll|,|pitch| darunter = eben

// ── Digitale Eingänge ────────────────────────────────────────
#define GPIO_LANDSTROM_SENSOR       18          // INPUT, Spannungsteiler 2k/1,5k von 5V, HIGH = Landstrom

// ── Digitale Ausgänge ────────────────────────────────────────
// v5.5-FIX: D+ läuft seit dem Hardware-Umbau über ein Joy-it COM-MOSFET
// (IRF9540N, P-Kanal High-Side, Active-HIGH) — die in der Anschluss_
// Anleitung dokumentierte Umstellung RELAY_D_PLUS_ACTIVE LOW→HIGH war
// hier nie nachgezogen worden. Folge: EIN/AUS invertiert UND D+ ab
// Boot aktiv (io_init() setzte HIGH als "sicher" nach alter Logik).
#define GPIO_RELAY_D_PLUS           21          // Active-HIGH (Joy-it COM-MOSFET)
#define RELAY_D_PLUS_ACTIVE         HIGH

#define GPIO_MOSFET_GEL             39          // Active-HIGH (JTAG-MTCK, USB-JTAG aktiv)
#define MOSFET_GEL_ACTIVE           HIGH

#define GPIO_OPTO_WR_REMOTE         40          // Active-HIGH (JTAG-MTDO)
                                                  // Renogy NVS: Brücke Schalterkontakt am Fernbedienungsport
#define OPTO_WR_ACTIVE              HIGH

#define GPIO_STATUS_LED             41          // Active-HIGH (JTAG-MTDI)

// ── On-board RGB-LED (WS2812 auf GPIO 48) ────────────────────
// Angesteuert über die Core-eigene neopixelWrite()-Funktion
// (Arduino-ESP32 2.x, RMT-basiert) — KEINE externe Library nötig.
// Nur aus dem LED-Render-Task ansprechen (Single-Thread, RMT-sicher).
// RGB_LED_BRIGHTNESS dimmt global (Nachtsicht im Womo); Basisfarben 0–255.
#define GPIO_RGB_LED                48
#define RGB_LED_BRIGHTNESS              40          // 0–255 globale Helligkeit
#define RGB_SOC_GAUGE_CRIT_PCT      10          // < diesem SoC → SoC-Slot rot (kritisch)
#define RGB_SOC_GAUGE_LOW_PCT       20          // < diesem SoC → SoC-Slot orange
#define RGB_SOC_GAUGE_MID_PCT       40          // < diesem SoC → SoC-Slot gelb, sonst grün

// ── WLAN ─────────────────────────────────────────────────────
#define WIFI_AP_SSID                "GODMODULE"
#define WIFI_AP_PASSWORD            "BM-hy678"
#define WIFI_AP_CHANNEL             6
#define WIFI_AP_MAX_CLIENTS         3

// Heim-WLAN (STA): WoMo bucht sich vor der Haustür ins Heimnetz ein.
// v5.5.1: bis zu 3 Netze (NVS "wifi", Schlüssel ssid1/pass1..ssid3/pass3),
// Konfiguration zur Laufzeit per Dashboard. Bei mehreren konfigurierten
// Netzen wählt ein asynchroner Scan das stärkste bekannte Netz; bei
// genau einem Netz Direktverbindung wie bisher. Alle Slots leer =
// reiner AP-Betrieb. Details: http_server.cpp (wifi_apply_sta/wifi_tick).
// APSTA teilt EIN Radio: bei STA-Verbindung wandert der AP zwingend
// auf den Router-Kanal (WIFI_AP_CHANNEL gilt nur ohne STA-Verbindung).

// ── Webserver ────────────────────────────────────────────────
#define WEBSERVER_PORT              80          // WS auf /ws, gleichem Port

// ── Netzwerkdienste (v5.5.2) ─────────────────────────────────
// mDNS: Erreichbarkeit unter "<host>.local" ohne IP — greift sowohl im
// AP-Modus (192.168.4.1) als auch nach STA-Verbindung (Heimnetz-IP).
// Bei STA-Verbindung wird der Dienst neu angekündigt (http_server.cpp).
#define MDNS_HOSTNAME               "womo"      // → http://womo.local
// NTP: Zeitsync NUR über STA (Internet). Der globale Pool routet zum
// nächstgelegenen Server — bewusst kein Länder-Pool, damit der Sync auch
// im Ausland greift. Gestellt wird über clock_set_epoch() (UTC), also
// derselbe Pfad wie der Browser-Sync; im AP-Only-Betrieb bleibt der
// Browser die Zeitquelle. Kein configTime() → POSIX-TZ bleibt unberührt.
#define NTP_SERVER                  "pool.ntp.org"

// ── Bluetooth Low Energy (v5.6.0) ────────────────────────────
// GATT-Server mit Nordic UART Service (NUS) — newline-delimited
// JSON, Live-Push alle 2s + Kommandos (s. ble.h). Passkey-Pairing
// (BLE_PASSKEY in secrets.h). Schalter: NVS "ble"/"en", Toggle im
// Dashboard (System-Tab) → deferred Reboot. NimBLE + WiFi koexistent
// (Kostenpunkt ≈ 70–90 KB Heap bei aktivem Stack).
#define BLE_DEVICE_NAME             "WomoEnergy" // Advertising-Name
#define DEFAULT_BLE_ENABLED         1            // NVS-Default: an
#define BLE_PREF_MTU                517          // max. MTU-Angebot (Client handelt)
#define BLE_RX_LINE_MAX             256          // max. Kommandozeile (Byte)
#define BLE_RX_QUEUE_LEN            4            // RX-Queue-Tiefe (Zeilen)

// ── RAM-Ringpuffer (PSRAM) ───────────────────────────────────
#define LOG_BUFFER_SIZE             86400       // 48h @ 2s = 86400 Einträge

// ── SD-Karte ─────────────────────────────────────────────────
#define LOG_FILE_PREFIX             "/"         // /log_NNNNN.csv (NNNNN = Tage UTC)

// ── Watchdog ─────────────────────────────────────────────────
#define WDT_TIMEOUT_MS              4000

// ── BMS Protokoll (JK 4E-57-Frame) ──────────────────────────
#define BMS_POLL_INTERVAL_MS        2000
#define BMS_FRAME_MAX_LEN           320
#define BMS_FRAME_HEADER_1          0x4E
#define BMS_FRAME_HEADER_2          0x57

// Hardware: JK_B2A8S20P, HW V11.XW, FW V11.287H, Protokoll V5.11.0
//           4S LFP 280Ah, 8S-BMS (Zellen 05-08 leer/"--")
// Stromkodierung 0x84 — intern: + = LADEN
//   FW V11.287H verwendet int16 Zweierkomplement (am Gerät verifiziert)
//   Abweichung von Protokoll-Doku (dort: 10000-Offset oder Bit15-Flag)
//   BMS_CURRENT_MODE 0 = int16 Zweierkomplement ← korrekt für dieses Gerät
//   BMS_CURRENT_MODE 1 = Bit15-Flag (für andere FW-Varianten)
//   BMS_CURRENT_MODE 2 = 10000-Offset (Protokoll-Standard C0=0x00)
#define BMS_CURRENT_MODE            0
#define BMS_CURRENT_SCALE           0.01f       // A/Bit (Doku: 0.01A/Bit)

// Staleness: BMS-Daten veraltet wenn > BMS_STALE_TIMEOUT_MS kein Frame
#define BMS_STALE_TIMEOUT_MS        60000       // 60s (L-SW03/04/05)

// ── MPPT VE.Direct ───────────────────────────────────────────
#define MPPT_FRAME_TIMEOUT_MS       3000        // Text-Protokoll Timeout

// VE.Direct HEX — Remote-Temperatursensing (v5.0)
// Register 0x2003: Battery temperature sense (Remote control registers)
//   Typ:  sn16 (vorzeichenbehaftet — negative Temperaturen möglich!)
//   Skala: 0.01  →  Wert = tempC × 100 als int16_t
//   Einheit: Grad Celsius  (NICHT Kelvin!)
//   Timeout: MPPT kehrt nach 60s ohne Update zur internen Messung zurück
//   Reservierter N/A-Wert: 0x7FFF (senden wenn BMS-Daten veraltet)
//   Neueste Firmware bestätigt — Register 0x2003 voll unterstützt
// Sendeintervall: 10s (weit unter 60s-Timeout)
// Frame: ':' + '8' (SET, single nibble!) + reg_lo + reg_hi
//        + flags(00) + val_lo + val_hi + chk + '\n'
// Checksum: (0x55 - (0x08 + alle Datenbytes)) & 0xFF
#define MPPT_HEX_TEMP_REGISTER      0x2003      // Battery temperature sense
#define MPPT_HEX_TEMP_NA            0x7FFF      // N/A-Wert wenn keine gültigen Daten
#define MPPT_HEX_SEND_INTERVAL_MS   10000       // 10s << 60s Timeout


// ============================================================
//  BLOCK 2 — DEFAULT-PARAMETER (NVS-überschreibbar)
// ============================================================

// ── D+-Relais Kühlschrank ─────────────────────────────────────
// v5.5 (Kriterien-Redesign): kein PV-basiertes AUS mehr, kein
// socHigh, MPPT-Ausfall ist KEINE AUS-Bedingung (nur die EIN-Seite
// braucht gültige MPPT-Daten). Float-Modus des MPPT zählt als
// "genug PV" — im Float ist der Akku voll, Überschuss vorhanden.
// EIN:      SoC >= ON  UND  (PV(MA) >= pvDPlusMinW  ODER  CS=Float)
//           UND BMS gültig UND MPPT gültig UND kein Landstrom
// AUS hart: Landstrom | BMS ungültig/veraltet | SoC < OFF
#define DEFAULT_SOC_D_PLUS_ON           95      // % EIN-Schwelle
#define DEFAULT_SOC_D_PLUS_OFF          80      // % harte AUS-Schwelle
#define DEFAULT_PV_D_PLUS_MIN_W        100      // W Mindest-PV (ODER Float)

// ── Gel-Lader Starterbatterie ────────────────────────────────
// v5.5: identische Struktur wie D+ (inkl. Landstrom-AUS, neu) —
// eigener socGelOff (bisher war socGelOn zugleich harte AUS-Schwelle).
// EIN:      SoC >= socGelOn  UND  (PV(MA) >= pvGelMinW  ODER  CS=Float)
//           UND BMS gültig UND MPPT gültig UND kein Landstrom
// AUS hart: Landstrom | BMS ungültig/veraltet | SoC < socGelOff
#define DEFAULT_SOC_GEL_ON              95      // % EIN-Schwelle
#define DEFAULT_SOC_GEL_OFF             80      // % harte AUS-Schwelle (neu v5.5)
#define DEFAULT_PV_GEL_MIN_W            20      // W Mindest-PV (v5.5: 60→20W)

// ── Wechselrichter Remote (v5.5: KEIN Auto-EIN mehr) ─────────
// Einschalten ausschließlich manuell (Dashboard, ohne Deadman-Timer).
// Die Automatik darf den WR nur noch ABschalten — auch während
// Manuell-EIN (Sicherheitsnetz, da der Timer entfällt):
// AUS hart: BMS ungültig/veraltet
// AUS weich:SoC < socWROff
// (v5.3: Landstrom-Abhängigkeit entfernt — Renogy-NVS übernimmt die
// AC-seitige Umschaltung Landstrom/Inverter selbst.)
#define DEFAULT_SOC_WR_OFF              80      // %

// ── Flatter-Schutz ───────────────────────────────────────────
#define DEFAULT_RELAY_DEBOUNCE_CYCLES   10      // Zyklen × 2s = 20s Stabilität

// ── Manueller Aktor-Override (Webinterface, v5.4/v5.5) ───────
// v5.5: Deadman-Timeout gilt NUR noch für Manuell-EIN von D+ und
// Gel-Lader. Manuell-AUS ist dauerhaft (NVS-persistent, überlebt
// Reboot, kein Rückfall auf Auto). WR-Manuell-EIN läuft ohne Timer
// (Sicherheitsnetz sind die Auto-AUS-Bedingungen, s. oben).
#define DEFAULT_MANUAL_TIMEOUT_MIN      30      // min Deadman (nur EIN, nur D+/Gel)

// ── PV-Glättung ──────────────────────────────────────────────
#define LOGIC_PPV_MA_WINDOW             15      // Samples × 2s = 30s MA-Fenster

// ── MPPT Recovery-Debounce ───────────────────────────────────
#define LOGIC_MPPT_RECOVERY_MIN          5      // valide Frames nach Timeout (≈ 10s)

// ── Logging ──────────────────────────────────────────────────
#define DEFAULT_LOG_INTERVAL_MS     900000      // 15 Minuten SD-Batch

// ── Lagesensor — Default-Fahrzeuggeometrie (NVS-überschreibbar) ──
// Spurweite = seitlicher Radabstand, Radstand = Abstand Vorder-/Hinterachse.
// Im Dashboard (Tab "Lage") einstellbar; nur für die Keilhöhen-Berechnung.
#define DEFAULT_TRACK_MM            1800        // mm Spurweite
#define DEFAULT_WHEELBASE_MM        3500        // mm Radstand