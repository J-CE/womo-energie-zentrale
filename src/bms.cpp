// ============================================================
//  bms.cpp — Womo Energy Core v5.4
//  JK-BMS UART-Parser (4E-57-Frame, direkt per TTL am GPS-Port)
//
//  v5.4: GPIO_RS485_DE_RE (MAX485-Richtungssteuerung) entfernt —
//  seit der Umstellung auf direkte TTL-Verdrahtung am GPS-Port
//  (kein MAX485 im Pfad, siehe Anschluss_Anleitung.txt Modul 2)
//  war das Togglen von GPIO 15 nur noch totes Legacy-Verhalten
//  auf einem freien Pin.
//
//  v5.0: Reduzierter Datensatz — Zellspannungen (0x79) werden
//  korrekt geparst (Parser-Sync!), aber nicht gespeichert.
//  Schutzparameter, Zyklen, Balancing, Alarmflags entfallen.
// ============================================================
#include "bms.h"
#include "config.h"
#include <HardwareSerial.h>

#ifndef BMS_DEBUG_RAW
#define BMS_DEBUG_RAW 0
#endif
static void dump_frame(const uint8_t* b, uint16_t n) {
    Serial.printf("[BMS] RAW (%u B): ", n);
    for (uint16_t i = 0; i < n; i++) Serial.printf("%02X ", b[i]);
    Serial.println();
}

BMSData           g_bms      = {};
SemaphoreHandle_t g_bmsMutex = nullptr;

static HardwareSerial BMS_Serial(UART_BMS_PORT);

static const uint8_t BMS_QUERY[] = {
    0x4E,0x57,0x00,0x13,0x00,0x00,0x00,0x00,
    0x06,0x03,0x00,0x00,0x00,0x00,0x00,0x00,
    0x68,0x00,0x00,0x01,0x29
};

static uint8_t  rx_buf[BMS_FRAME_MAX_LEN];
static uint16_t rx_len = 0;

static inline uint16_t read_u16(const uint8_t* p) { return ((uint16_t)p[0]<<8)|p[1]; }
static inline uint32_t read_u32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

// JK-Temp: 1°C Schritte, >100 = negativ (raw-100, negiert)
static inline float jk_temp(uint16_t raw) {
    return (raw > 100) ? -((float)(raw-100)) : (float)raw;
}

// Strom 0x84: + = LADEN
static inline float jk_current(uint16_t raw) {
#if   BMS_CURRENT_MODE == 1
    float mag = (float)(raw & 0x7FFF) * BMS_CURRENT_SCALE;
    return (raw & 0x8000) ? mag : -mag;
#elif BMS_CURRENT_MODE == 2
    return (float)((int32_t)10000-(int32_t)raw) * BMS_CURRENT_SCALE;
#else
    return (float)(int16_t)raw * BMS_CURRENT_SCALE;
#endif
}

// Payload-Länge je Identifier (-2 = variabel/0x79, -1 = unbekannt)
static int id_payload_len(uint8_t id) {
    switch (id) {
        case 0x79:                                          return -2;
        case 0x80: case 0x81: case 0x82:
        case 0x83: case 0x84:                              return 2;
        case 0x85: case 0x86:                              return 1;
        case 0x87:                                         return 2;
        case 0x89:                                         return 4;
        case 0x8A: case 0x8B: case 0x8C:                  return 2;
        case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C:                                         return 2;
        case 0x9D:                                         return 1;
        case 0x9E: case 0x9F: case 0xA0: case 0xA1:
        case 0xA2: case 0xA3: case 0xA4: case 0xA5:
        case 0xA6: case 0xA7: case 0xA8:                  return 2;
        case 0xA9:                                         return 1;
        case 0xAA:                                         return 4;
        case 0xAB: case 0xAC:                              return 1;
        case 0xAD:                                         return 2;
        case 0xAE: case 0xAF:                              return 1;
        case 0xB0:                                         return 2;
        case 0xB1:                                         return 1;
        case 0xB2:                                         return 10;
        case 0xB3:                                         return 1;
        case 0xB4:                                         return 8;
        case 0xB5: case 0xB6:                              return 4;
        case 0xB7:                                         return 15;
        case 0xB8:                                         return 1;
        case 0xB9:                                         return 4;
        case 0xBA:                                         return 24;
        case 0xC0:                                         return 1;
        default:                                           return -1;
    }
}

static bool verify_checksum(const uint8_t* buf, uint16_t len) {
    if (len < 6) return false;
    uint32_t sum = 0;
    for (uint16_t i = 0; i < (uint16_t)(len-4); i++) sum += buf[i];
    return (uint16_t)(sum & 0xFFFF) == read_u16(&buf[len-2]);
}

static void note_error() {
    if (g_bmsMutex && xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_bms.errorCount++;
        xSemaphoreGive(g_bmsMutex);
    }
}

static int find_data_end(const uint8_t* buf, uint16_t len) {
    if (len >= 20 && buf[len-5] == 0x68) return len-5-4;
    if (len >= 20 && buf[len-7] == 0x68) return len-7;
    for (int i = (int)len-5; i >= 11; i--)
        if (buf[i] == 0x68) return i;
    return -1;
}

static bool parse_frame(const uint8_t* buf, uint16_t len) {
    if (len < 20) return false;
    int endIdx = find_data_end(buf, len);
    if (endIdx < 11) { Serial.println("[BMS] Frame-Ende nicht gefunden"); return false; }
    if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    const uint8_t* data = &buf[11];
    int dlen = endIdx - 11;
    int pos  = 0;
    bool desync = false;
    int  bad_id = -1;   // K-1: Serial-Ausgabe NICHT unter g_bmsMutex — hier nur merken

    while (pos < dlen) {
        uint8_t id = data[pos++];

        // 0x79 Zellspannungen: korrekt überspringen (Sync!), nicht speichern
        if (id == 0x79) {
            if (pos >= dlen) break;
            uint8_t nbytes = data[pos++];
            pos += nbytes;
            if (pos > dlen) { desync = true; break; }
            continue;
        }

        int plen = id_payload_len(id);
        if (plen < 0 || pos + plen > dlen) {
            bad_id = id;
            desync = true;
            break;
        }

        switch (id) {
        case 0x80: g_bms.tempMOS     = jk_temp(read_u16(&data[pos])); break;
        case 0x81: g_bms.tempSensor1 = jk_temp(read_u16(&data[pos])); break;
        case 0x82: g_bms.tempSensor2 = jk_temp(read_u16(&data[pos])); break;
        case 0x83: g_bms.totalVoltage = read_u16(&data[pos]) * 0.01f; break;
        case 0x84: g_bms.current      = jk_current(read_u16(&data[pos])); break;
        case 0x85: g_bms.soc          = data[pos]; break;
        case 0x8C: {  // MOSFET-Status (Bits 0+1)
            uint16_t st = read_u16(&data[pos]);
            g_bms.chargeMOSFETEnabled    = (st & 0x01) != 0;
            g_bms.dischargeMOSFETEnabled = (st & 0x02) != 0;
            break;
        }
        case 0xAA: g_bms.nominalCapacityAh = (float)read_u32(&data[pos]); break; // Einheit: Ah direkt (Doku: "4 HEX AH")
        default: break; // bekannte Länge, hier nicht ausgewertet
        }
        pos += plen;
    }

    g_bms.frameCount++;
    if (desync) {
        // M-6: Desynchronisierter Frame → NICHT als frisch & gültig führen.
        // Die Prüfsumme war ok (Bytes intakt), aber die TLV-Kette brach ab
        // (unbekannte ID / Längenüberlauf) → Felder VOR dem Abbruch könnten
        // bei fehlerhafter Längentabelle fehlinterpretiert worden sein.
        // valid/lastUpdateMs bleiben unverändert → die Staleness-Prüfung in der
        // Logik greift, statt potenziell falsche Werte als „gültig" zu behandeln.
        // Ein einzelner Desync zwischen guten Frames kostet keine Frische
        // (gute Frames setzen lastUpdateMs); dauerhafte Desyncs führen sauber
        // in den Stale-Zustand (bms_ok=false → Fail-Safe).
        g_bms.errorCount++;
    } else {
        g_bms.power = g_bms.totalVoltage * g_bms.current;
        g_bms.remainingCapacityAh = g_bms.nominalCapacityAh * (float)g_bms.soc / 100.0f;
        g_bms.valid        = true;
        g_bms.lastUpdateMs = millis();
    }

    xSemaphoreGive(g_bmsMutex);
    // K-1: erst NACH Freigabe loggen — Serial (USB-CDC) könnte blockieren.
    if (bad_id >= 0)
        Serial.printf("[BMS] Unbekannte/korrupte ID 0x%02X — Rest verworfen\n",
                      (unsigned)bad_id);
    return true;
}

static void align_to_header() {
    while (rx_len >= 1 && rx_buf[0] != BMS_FRAME_HEADER_1) {
        uint16_t k = 1;
        while (k < rx_len && rx_buf[k] != BMS_FRAME_HEADER_1) k++;
        memmove(rx_buf, rx_buf+k, rx_len-k);
        rx_len -= k;
    }
    if (rx_len >= 2 && rx_buf[1] != BMS_FRAME_HEADER_2) {
        memmove(rx_buf, rx_buf+1, rx_len-1);
        rx_len--;
        align_to_header();
    }
}

void bms_init() {
    g_bmsMutex = xSemaphoreCreateMutex();
    memset(&g_bms, 0, sizeof(g_bms));
    BMS_Serial.begin(UART_BMS_BAUD, SERIAL_8N1, UART_BMS_RX, UART_BMS_TX);
    Serial.println("[BMS] RX=" + String(UART_BMS_RX) + " TX=" + String(UART_BMS_TX));
}

bool bms_poll() {
    while (BMS_Serial.available()) BMS_Serial.read();
    rx_len = 0;
    BMS_Serial.write(BMS_QUERY, sizeof(BMS_QUERY));
    BMS_Serial.flush();

    const uint32_t start    = millis();
    uint32_t       lastByte = start;
    while ((uint32_t)(millis()-start) < 300) {
        bool got = false;
        while (BMS_Serial.available() && rx_len < BMS_FRAME_MAX_LEN) {
            rx_buf[rx_len++] = (uint8_t)BMS_Serial.read();
            got = true;
        }
        if (got) { lastByte = millis(); align_to_header(); }
        if (rx_len >= 4 && rx_buf[0]==BMS_FRAME_HEADER_1 && rx_buf[1]==BMS_FRAME_HEADER_2) {
            uint16_t lf = ((uint16_t)rx_buf[2]<<8)|rx_buf[3];
            if (lf >= 18 && rx_len >= (uint16_t)(lf+2)) break;   // Doku: frame=LENGTH+2
        }
        if (rx_len >= 20 && (uint32_t)(millis()-lastByte) > 40) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (rx_len < 20) { note_error(); return false; }
    if (rx_buf[0]!=BMS_FRAME_HEADER_1 || rx_buf[1]!=BMS_FRAME_HEADER_2) {
        note_error(); return false;
    }

#if BMS_DEBUG_RAW
    dump_frame(rx_buf, rx_len);
#endif

    uint16_t lf = ((uint16_t)rx_buf[2]<<8)|rx_buf[3];
    uint16_t candidates[3] = { (uint16_t)(lf+2), (uint16_t)(lf+4), rx_len };
    uint16_t flen = 0;
    for (uint8_t i = 0; i < 3; i++) {
        uint16_t c = candidates[i];
        if (c >= 20 && c <= rx_len && verify_checksum(rx_buf, c)) { flen = c; break; }
    }
    if (flen == 0) {
        Serial.println("[BMS] Checksum FEHLER");
        dump_frame(rx_buf, rx_len);
        note_error(); return false;
    }
    return parse_frame(rx_buf, flen);
}

String bms_to_json() {
    if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return "{\"valid\":false}";
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"valid\":%s,"
        "\"soc\":%d,"
        "\"v\":%.2f,\"i\":%.2f,\"pw\":%.1f,"
        "\"t_mos\":%.1f,\"t_s1\":%.1f,\"t_s2\":%.1f,"
        "\"rem\":%.1f,\"nom\":%.1f,"
        "\"ch_en\":%s,\"dis_en\":%s,"
        "\"fc\":%lu,\"ec\":%lu}",
        g_bms.valid?"true":"false",
        g_bms.soc,
        g_bms.totalVoltage, g_bms.current, g_bms.power,
        g_bms.tempMOS, g_bms.tempSensor1, g_bms.tempSensor2,
        g_bms.remainingCapacityAh, g_bms.nominalCapacityAh,
        g_bms.chargeMOSFETEnabled?"true":"false",
        g_bms.dischargeMOSFETEnabled?"true":"false",
        (unsigned long)g_bms.frameCount, (unsigned long)g_bms.errorCount);
    xSemaphoreGive(g_bmsMutex);
    return String(buf);
}
