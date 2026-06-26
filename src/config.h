// ============================================================
//  config.h — Womo Energy Core v5.1
//  Zielplattform: ESP32-S3 DevKitC-1 N16R8
//
//  GPIO-Reservierungen ESP32-S3 N16R8:
//  ├─ GPIO  0,3      Strapping                  → gesperrt
//  ├─ GPIO 19,20     USB D-/D+                  → freigehalten
//  ├─ GPIO 26–37     Intern Flash/PSRAM          → gesperrt
//  ├─ GPIO 39–42     Default-JTAG (MTCK…MTMS)
//  │                 → nutzbar, da USB-JTAG aktiv
//  ├─ GPIO 45,46     Strapping                   → gesperrt
//  ├─ GPIO 48        On-board RGB-LED (WS2812)    → genutzt (Status)
//  └─ GPIO 1–2       I2C DS3231 RTC (reserviert, nicht bestückt)
// ============================================================

#pragma once

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

// JK-BMS über MAX485 (UART1)
#define UART_BMS_PORT               1
#define UART_BMS_RX                 16
#define UART_BMS_TX                 17
#define UART_BMS_BAUD               115200
#define GPIO_RS485_DE_RE            15

// VE.Direct (Victron MPPT) — UART2, bidirektional ab v5.0
//   RX: Spannungsteiler 1kΩ+2kΩ (5V→3,3V)
//   TX: 3,3V direkt, kein Pegelwandler (MPPT-Eingang kompatibel)
//   Text-Protokoll (automatisch alle ~1s, RX)
//   HEX-Protokoll (Temperatur senden, TX)
#define UART_MPPT_PORT              2
#define UART_MPPT_RX                38
#define UART_MPPT_TX                42          // neu v5.0 (war -1)
#define UART_MPPT_BAUD              19200

// Wechselrichter RJ12 Sniffer (Stub)
#define UART_INV_PORT               0
#define UART_INV_RX                 -1
#define UART_INV_TX                 -1
#define UART_INV_BAUD               9600

// ── SPI — SD-Karte ───────────────────────────────────────────
#define SPI_SD_CS                   11
#define SPI_SD_CLK                  12
#define SPI_SD_MOSI                 13
#define SPI_SD_MISO                 14

// ── I2C — DS3231 RTC (reserviert, noch nicht bestückt) ───────
#define I2C_RTC_SDA                 1
#define I2C_RTC_SCL                 2

// ── Digitale Eingänge ────────────────────────────────────────
#define GPIO_LANDSTROM_SENSOR       18          // INPUT, Spannungsteiler 2k/1,5k von 5V, HIGH = Landstrom

// ── Digitale Ausgänge ────────────────────────────────────────
#define GPIO_RELAY_D_PLUS           21          // Active-LOW
#define RELAY_D_PLUS_ACTIVE         LOW

#define GPIO_MOSFET_GEL             39          // Active-HIGH (JTAG-MTCK, USB-JTAG aktiv)
#define MOSFET_GEL_ACTIVE           HIGH

#define GPIO_OPTO_WR_REMOTE         40          // Active-HIGH (JTAG-MTDO)
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
// Konfiguration zur Laufzeit per Dashboard → NVS ("wifi"-Namespace).
// Diese Defaults greifen nur beim allerersten Boot / leerem NVS.
// Leere SSID = reiner AP-Betrieb, STA wird nicht gestartet.
// APSTA teilt EIN Radio: bei STA-Verbindung wandert der AP zwingend
// auf den Router-Kanal (WIFI_AP_CHANNEL gilt nur ohne STA-Verbindung).
#define DEFAULT_WIFI_STA_SSID       ""
#define DEFAULT_WIFI_STA_PASSWORD   ""

// ── Webserver ────────────────────────────────────────────────
#define WEBSERVER_PORT              80          // WS auf /ws, gleichem Port

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
// EIN:      SoC >= ON  UND  PV(MA) >= pvThresholdOn  UND  kein Landstrom
// AUS hart: SoC < OFF
// AUS weich:(PV(MA) < pvThresholdOff  UND  SoC < socDPlusHigh)
//           → bei SoC >= socDPlusHigh bleibt Kühlschrank trotz PV-Einbruch EIN
#define DEFAULT_SOC_D_PLUS_ON           95      // % EIN-Schwelle
#define DEFAULT_SOC_D_PLUS_OFF          80      // % harte AUS-Schwelle
#define DEFAULT_SOC_D_PLUS_HIGH         90      // % weiche PV-AUS nur unterhalb
#define DEFAULT_PV_THRESHOLD_ON        150      // W EIN-Schwelle
#define DEFAULT_PV_THRESHOLD_OFF       100      // W AUS-Schwelle (nur wenn SoC < HIGH)

// ── Gel-Lader Starterbatterie ────────────────────────────────
// EIN:      SoC >= socGelOn  UND  PV(MA) >= pvGelMinW
// AUS hart: SoC < socGelOn  (kein separater socGelOff — Schwelle = EIN-Schwelle)
// AUS weich:(PV(MA) < pvGelMinW  UND  SoC < socGelHigh)
//           → bei SoC >= socGelHigh läuft Gel-Lader auch ohne PV weiter
#define DEFAULT_SOC_GEL_ON              82      // % EIN-Schwelle + harte AUS-Schwelle
#define DEFAULT_SOC_GEL_HIGH            90      // % weiche PV-AUS nur unterhalb
#define DEFAULT_PV_GEL_MIN_W            60      // W Mindest-PV (v5.0: 30→60W)

// ── Wechselrichter Remote (SoC-basiert, kein PV-Check) ───────
// EIN:      SoC >= socWROn  UND  kein Landstrom  UND  BMS gültig
// AUS hart: Landstrom  ODER  BMS ungültig/veraltet
// AUS weich:SoC < socWROff
#define DEFAULT_SOC_WR_ON               95      // %
#define DEFAULT_SOC_WR_OFF              80      // %

// ── Flatter-Schutz ───────────────────────────────────────────
#define DEFAULT_RELAY_DEBOUNCE_CYCLES   10      // Zyklen × 2s = 20s Stabilität

// ── PV-Glättung ──────────────────────────────────────────────
#define LOGIC_PPV_MA_WINDOW             15      // Samples × 2s = 30s MA-Fenster

// ── MPPT Recovery-Debounce ───────────────────────────────────
#define LOGIC_MPPT_RECOVERY_MIN          5      // valide Frames nach Timeout (≈ 10s)

// ── D+-Mindestlaufzeit (Kompressor-Schutz) ───────────────────
#define LOGIC_DPLUS_MIN_ON_MS       300000      // 300s = 5min

// ── Logging ──────────────────────────────────────────────────
#define DEFAULT_LOG_INTERVAL_MS     900000      // 15 Minuten SD-Batch