// ============================================================
//  mppt.cpp — Womo Energy Core v5.4
//  Victron VE.Direct Text Protocol Parser + HEX-TX Temperatur
//
//  Text-Protokoll (VE.Direct Protocol v3.34, April 2025):
//   • Format: \r\n<Label>\t<Value>  pro Feld
//   • Einheiten: V=mV, I=mA, VPV=mV, PPV=W, H20=0.01kWh, H21=W
//   • Block endet mit "Checksum\t[raw_byte]"
//   • Checksumme: Modulo-256-Summe ALLER Bytes im Block = 0
//   • Max. 22 Felder pro Block (Doku v3.28) → MAX_FIELDS=25
//   • Max. Feldwert-Länge: 32 Zeichen (Doku) → values[33]
//
//  HEX-Protokoll (SET, v5.0):
//   • Register 0x2003 Battery temperature sense (Remote control)
//   • sn16, 0.01°C, Timeout 60s
//   • Command als single nibble '8' im ASCII-Frame
//   • Checksum: (0x55 - (0x08 + alle Datenbytes)) & 0xFF
// ============================================================
#include "mppt.h"
#include "config.h"
#include <HardwareSerial.h>

MPPTData          g_mppt      = {};
SemaphoreHandle_t g_mpptMutex = nullptr;

static HardwareSerial MPPT_Serial(UART_MPPT_PORT);

// ── Text-Protokoll Puffer ─────────────────────────────────────
// MAX_FIELDS >= 22 (Doku-Anforderung seit v3.28)
// values[33]: Doku empfiehlt 33 Bytes für Feldwerte
#define LINE_BUF    64
#define MAX_FIELDS  25
static char    line_buf[LINE_BUF];
static uint8_t line_pos    = 0;
static char    keys  [MAX_FIELDS][12];   // Doku: max 9 Bytes → 12 mit Reserve
static char    values[MAX_FIELDS][33];   // Doku: max 33 Bytes
static uint8_t field_count = 0;

// Checksummen-Akkumulator: Modulo-256 über ALLE Bytes im TEXT-Block
// Doku: "The modulo 256 sum of all bytes in a block will equal 0"
// Wird in mppt_poll() pro Byte accumul., in commit_block() validiert.
static uint8_t s_block_sum = 0;

// M-1: VE.Direct verschachtelt asynchrone Text-Frames mit HEX-Antwort-Frames
// (':' … '\n'), die der MPPT auf unsere HEX-SET-Kommandos (Temp-TX, alle 10s)
// schickt. Diese HEX-Bytes gehören NICHT zur Modulo-256-Text-Checksumme und
// sind keine Text-Zeilen. Ohne Ausblendung verfälscht jede HEX-Antwort die
// Summe → der umschließende Text-Block scheitert an der Prüfsumme und wird
// verworfen. s_hex überspringt den kompletten HEX-Frame (state-persistent über
// mppt_poll-Aufrufe hinweg, da ein Frame über mehrere Polls gesplittet ankommen
// kann). s_hexLen kappt einen nie terminierten HEX-Frame (verlorenes '\n' bei
// RX-Overflow) → kein dauerhaftes Verschlucken des Text-Protokolls.
static bool    s_hex    = false;
static uint8_t s_hexLen = 0;

static void commit_block() {
    if (field_count == 0) return;

    // Checksummen-Validierung (inkl. Checksum-Byte selbst)
    if (s_block_sum != 0) {
        Serial.printf("[MPPT] Checksumme FEHLER (sum=0x%02X) — Block verworfen\n",
                      s_block_sum);
        field_count  = 0;
        s_block_sum  = 0;
        return;
    }
    s_block_sum = 0;   // Reset für nächsten Block

    if (xSemaphoreTake(g_mpptMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        field_count = 0;
        return;
    }
    for (uint8_t i = 0; i < field_count; i++) {
        const char* k = keys[i];
        const char* v = values[i];
        if      (strcmp(k,"V")   ==0) g_mppt.batteryVoltage = atol(v)/1000.0f;
        else if (strcmp(k,"I")   ==0) g_mppt.chargeCurrent  = atol(v)/1000.0f;
        else if (strcmp(k,"VPV") ==0) g_mppt.panelVoltage   = atol(v)/1000.0f;
        else if (strcmp(k,"PPV") ==0) g_mppt.panelPower     = (uint16_t)atoi(v);
        else if (strcmp(k,"CS")  ==0) g_mppt.chargeState    = (uint8_t)atoi(v);
        else if (strcmp(k,"ERR") ==0) g_mppt.errorCode      = (uint8_t)atoi(v);
        else if (strcmp(k,"H20") ==0) g_mppt.yieldToday     = atol(v)*0.01f;
        else if (strcmp(k,"H21") ==0) g_mppt.maxPowerToday  = (uint16_t)atoi(v);
    }
    g_mppt.valid        = true;
    g_mppt.lastUpdateMs = millis();
    g_mppt.frameCount++;
    g_mppt.timeout      = false;
    xSemaphoreGive(g_mpptMutex);
    field_count = 0;
}

static void process_line(const char* line) {
    // "Checksum" immer zuerst prüfen (kein Feld-Eintrag, nur commit)
    if (strncmp(line, "Checksum", 8) == 0) { commit_block(); return; }
    const char* tab = strchr(line, '\t');
    if (!tab || tab == line) return;
    if (field_count >= MAX_FIELDS) return;      // Schutz (sollte nie passieren)
    size_t klen = (size_t)(tab - line);
    if (klen >= sizeof(keys[0])) klen = sizeof(keys[0]) - 1;
    strncpy(keys[field_count], line, klen);
    keys[field_count][klen] = '\0';
    strlcpy(values[field_count], tab + 1, sizeof(values[0]));
    field_count++;
}

// ── VE.Direct HEX TX ─────────────────────────────────────────
// Protokoll lt. Victron BlueSolar MPPT Protocol Rev 18:
//   Frame: ':' [cmd_nibble=8] [reg_lo reg_hi flags val_lo val_hi] [chk] \n
//   Command ist 1 ASCII-Nibble, nicht 2 Hex-Chars!
//   Checksum: (0x55 - (0x08 + alle Datenbytes)) & 0xFF
//   Verifiziert gegen Doku-Beispiel: :8F0ED0064000C → chk=0x0C ✓

void mppt_send_temperature(float tempC) {
    // Register 0x2003: sn16, Skala 0.01, Einheit °C
    int16_t val;
    if (tempC < -327.0f || tempC > 327.0f) {
        val = (int16_t)MPPT_HEX_TEMP_NA;
    } else {
        val = (int16_t)(tempC * 100.0f + (tempC >= 0.0f ? 0.5f : -0.5f));
    }
    const uint8_t reg_lo = (uint8_t)(MPPT_HEX_TEMP_REGISTER & 0xFF);  // 0x03
    const uint8_t reg_hi = (uint8_t)(MPPT_HEX_TEMP_REGISTER >> 8);    // 0x20
    const uint8_t flags  = 0x00;
    const uint8_t val_lo = (uint8_t)((uint16_t)val & 0xFF);
    const uint8_t val_hi = (uint8_t)((uint16_t)val >> 8);
    const uint8_t chk    = (uint8_t)(0x55u - 0x08u
                           - reg_lo - reg_hi - flags - val_lo - val_hi);
    char frame[20];
    snprintf(frame, sizeof(frame), ":8%02X%02X%02X%02X%02X%02X\n",
             reg_lo, reg_hi, flags, val_lo, val_hi, chk);
    MPPT_Serial.print(frame);
    Serial.printf("[MPPT] HEX-TX 0x2003 = %.1f°C (val=%d) → %s", tempC, val, frame);
}

void mppt_send_temp_na() {
    // N/A-Wert 0x7FFF lt. Victron-Doku
    const uint8_t reg_lo = (uint8_t)(MPPT_HEX_TEMP_REGISTER & 0xFF);
    const uint8_t reg_hi = (uint8_t)(MPPT_HEX_TEMP_REGISTER >> 8);
    const uint8_t flags  = 0x00;
    const uint8_t val_lo = 0xFF;   // 0x7FFF LE
    const uint8_t val_hi = 0x7F;
    const uint8_t chk    = (uint8_t)(0x55u - 0x08u
                           - reg_lo - reg_hi - flags - val_lo - val_hi);
    char frame[20];
    snprintf(frame, sizeof(frame), ":8%02X%02X%02X%02X%02X%02X\n",
             reg_lo, reg_hi, flags, val_lo, val_hi, chk);
    MPPT_Serial.print(frame);
    Serial.printf("[MPPT] HEX-TX 0x2003 = N/A (BMS veraltet) → %s", frame);
}

// ── Init & Poll ───────────────────────────────────────────────
void mppt_init() {
    g_mpptMutex = xSemaphoreCreateMutex();
    memset(&g_mppt, 0, sizeof(g_mppt));
    // UART2 bidirektional: RX=38 Text-Protokoll, TX=42 HEX-Protokoll
    // F-09: RX-Puffer von 256 (Default) auf 512 B — bei 19200 Baud und ~100 ms
    // Poll-Takt fallen ~192 B/Zyklus an; unter Preemption (logic_task) kann das
    // Intervall wachsen → 256 B reichten knapp nicht. Muss VOR begin() gesetzt sein.
    MPPT_Serial.setRxBufferSize(512);
    MPPT_Serial.begin(UART_MPPT_BAUD, SERIAL_8N1, UART_MPPT_RX, UART_MPPT_TX);
    Serial.printf("[MPPT] UART2 RX=%d TX=%d Baud=%d\n",
                  UART_MPPT_RX, UART_MPPT_TX, UART_MPPT_BAUD);
}

bool mppt_poll() {
    bool got = false;
    while (MPPT_Serial.available()) {
        uint8_t b = MPPT_Serial.read();

        // M-1: HEX-Antwort-Frame (':' am Zeilenanfang … '\n') komplett
        // überspringen — weder in die Text-Checksumme noch in line_buf.
        if (!s_hex && line_pos == 0 && b == ':') { s_hex = true; s_hexLen = 0; }
        if (s_hex) {
            if (b == '\n' || ++s_hexLen > 64) s_hex = false;  // Frame-Ende / Notbremse
            continue;                                          // NICHT summieren/parsen
        }

        // Checksummen-Akkumulation: ALLE (Text-)Bytes (inkl. \r, \n, \t)
        s_block_sum = (uint8_t)(s_block_sum + b);

        if (b == '\r') continue;    // \r ignorieren (aber in Checksumme enthalten)
        if (b == '\n') {
            line_buf[line_pos] = '\0';
            if (line_pos > 0) { process_line(line_buf); got = true; }
            line_pos = 0;
        } else {
            if (line_pos < LINE_BUF - 1) line_buf[line_pos++] = (char)b;
        }
    }

    // Timeout-Check unter Mutex (v4.3+)
    if (xSemaphoreTake(g_mpptMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_mppt.valid &&
            (uint32_t)(millis() - g_mppt.lastUpdateMs) > MPPT_FRAME_TIMEOUT_MS) {
            if (!g_mppt.timeout) {
                g_mppt.timeout = true;
                Serial.println("[MPPT] TIMEOUT");
            }
        }
        xSemaphoreGive(g_mpptMutex);
    }
    return got;
}

String mppt_cs_text(uint8_t cs) {
    switch (cs) {
        case 0:   return "Aus";
        case 2:   return "Fehler";
        case 3:   return "Bulk";
        case 4:   return "Absorption";
        case 5:   return "Float";
        case 6:   return "Speicher";
        case 7:   return "Ausgleich";
        case 247: return "Auto-Ausgleich";
        case 252: return "Ext.Steuerung";
        default:  return "CS" + String(cs);
    }
}

String mppt_error_text(uint8_t err) {
    switch (err) {
        case 0:   return "OK";
        case 2:   return "Batterie-Überspannung";
        case 17:  return "Charger Temp hoch";
        case 18:  return "Charger Überstrom";
        case 20:  return "Bulk-Zeitlimit";
        case 33:  return "PV-Spannung zu hoch";
        case 65:  return "Kommunikationsverlust";
        case 67:  return "BMS Verbindung verloren";
        case 116: return "Kalibrierdaten verloren";
        case 117: return "Firmware inkompatibel";
        default:  return "ERR" + String(err);
    }
}

String mppt_to_json() {
    if (xSemaphoreTake(g_mpptMutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return "{\"valid\":false}";
    char buf[300];
    snprintf(buf, sizeof(buf),
        "{\"valid\":%s,\"timeout\":%s,"
        "\"v\":%.3f,\"i\":%.3f,\"vpv\":%.3f,\"ppv\":%d,"
        "\"cs\":%d,\"cs_s\":\"%s\","
        "\"err\":%d,\"err_s\":\"%s\","
        "\"h20\":%.2f,\"h21\":%d,\"fc\":%lu}",
        g_mppt.valid    ? "true" : "false",
        g_mppt.timeout  ? "true" : "false",
        g_mppt.batteryVoltage, g_mppt.chargeCurrent,
        g_mppt.panelVoltage,   g_mppt.panelPower,
        g_mppt.chargeState, mppt_cs_text(g_mppt.chargeState).c_str(),
        g_mppt.errorCode,   mppt_error_text(g_mppt.errorCode).c_str(),
        g_mppt.yieldToday, g_mppt.maxPowerToday,
        (unsigned long)g_mppt.frameCount);
    xSemaphoreGive(g_mpptMutex);
    return String(buf);
}
