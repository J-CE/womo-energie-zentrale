// ============================================================
//  bms.cpp — Womo Energy Core v5.6.5
//  JK-BMS UART-Parser (4E-57-Frame, direkt per TTL am GPS-Port)
//
//  v5.6.5: Frame-Ende & -Länge deterministisch statt geraten
//  (Grundlage: offizielles JK/JiKong-Protokoll-PDF, "Monitoring
//  Platform ↔ BMS"). Bisher wurde die Framelänge über drei
//  Kandidaten (LEN+2/LEN+4/rx_len) erraten — bei zufällig gültiger
//  Checksumme auf einer FALSCHEN Kandidatenlänge (Reste eines
//  Vorgänger-Frames im Puffer) wurden Byte-Fragmente aus Alt- und
//  Neu-Frame als ein Frame geparst → reproduzierbare Fantasiewerte
//  (z.B. Strom/Leistung) OHNE Fehlerzählung, da die Prüfsumme über
//  die falsche Länge zufällig aufging.
//  Fix: LEN-Feld ist lt. Doku exakt "Gesamtlänge − 2" → nur EIN
//  gültiger Kandidat, kein Raten mehr. find_data_end()-Bytewert-
//  Suche (0x68) entfällt ersatzlos — 0x68 kann als Nutzdaten-Byte
//  auftreten und die Suche fehlleiten; der Trailer-Offset wird
//  stattdessen aus der (bereits checksummen-validierten) Frame-
//  länge fest berechnet: Record-Nr(4) + End-ID(1) + Checksum(4)
//  = 9 Byte nach dem Datenfeld. End-ID-Byte wird zusätzlich an der
//  erwarteten Position verifiziert (Strukturprüfung, kein Bytewert-
//  Scan mehr).
//  Zusätzlich: Werte werden in ein lokales Snapshot geparst und
//  nur bei vollständig synchronem UND plausiblem Frame atomar nach
//  g_bms übernommen (bisher konnten bei einem Abbruch mitten im
//  Frame bereits geschriebene Einzelfelder als „letzter Stand"
//  stehenbleiben). Plausibilitätsgrenzen s. config.h.
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
#include <math.h>

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

// Prüft, ob ein Wertesatz physikalisch plausibel ist, BEVOR er nach
// g_bms übernommen wird. Fängt Frames ab, die trotz gültiger
// Prüfsumme (falsche Kandidatenlänge, Bitfehler o.ä.) Unsinn enthalten.
// Grenzen bewusst großzügig (Hardware-Rahmen, nicht Betriebsfenster) —
// s. config.h BMS_PLAUSIBLE_*.
static bool values_plausible(float v, float i, uint8_t soc,
                              float tMos, float tS1, float tS2) {
    if (v    < BMS_PLAUSIBLE_V_MIN    || v    > BMS_PLAUSIBLE_V_MAX)    return false;
    if (fabsf(i) > BMS_PLAUSIBLE_I_MAX_A)                               return false;
    if (soc  > 100)                                                     return false;
    if (tMos < BMS_PLAUSIBLE_T_MIN || tMos > BMS_PLAUSIBLE_T_MAX)       return false;
    if (tS1  < BMS_PLAUSIBLE_T_MIN || tS1  > BMS_PLAUSIBLE_T_MAX)       return false;
    if (tS2  < BMS_PLAUSIBLE_T_MIN || tS2  > BMS_PLAUSIBLE_T_MAX)       return false;
    return true;
}

static bool parse_frame(const uint8_t* buf, uint16_t len) {
    // Trailer lt. Protokoll-PDF (Kap. 4.2, Tab. 1): nach dem Datenfeld
    // folgen deterministisch Record-Nr(4) + End-ID(1)=0x68 + Checksum(4)
    // = 9 Byte. endIdx = Ende des Datenfelds (exklusiv), Datenfeld
    // beginnt bei Offset 11 (STX2+LEN2+TerminalID4+CMD1+SRC1+TYPE1).
    // Verifiziert am Query-Frame (BMS_QUERY, 21 B, LEN=0x0013):
    // endIdx=21-9=12, End-ID an Index 12+4=16 → rx_buf[16]==0x68. ✓
    if (len < 20) return false;
    int endIdx = (int)len - 9;
    if (endIdx < 11) { Serial.println("[BMS] Frame zu kurz (Trailer)"); return false; }
    if (buf[endIdx + 4] != 0x68) {
        // Strukturprüfung statt Bytewert-Suche: End-ID nicht an der
        // erwarteten, aus der validierten Framelänge berechneten
        // Position → Frame verwerfen statt raten.
        Serial.println("[BMS] End-ID (0x68) an falscher Position — Frame verworfen");
        note_error();
        return false;
    }

    const uint8_t* data = &buf[11];
    int dlen = endIdx - 11;
    int pos  = 0;
    bool desync = false;
    int  bad_id = -1;   // K-1: Serial-Ausgabe NICHT unter g_bmsMutex — hier nur merken

    // Lokales Snapshot: erst bei vollständig synchronem & plausiblem
    // Frame atomar nach g_bms übernehmen. Nicht in diesem Frame
    // enthaltene Felder bleiben (wie bisher) auf dem letzten Stand —
    // dafür mit den aktuellen g_bms-Werten vorbelegt.
    if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    float   nV = g_bms.totalVoltage, nI = g_bms.current;
    float   nTMos = g_bms.tempMOS, nTS1 = g_bms.tempSensor1, nTS2 = g_bms.tempSensor2;
    float   nNom = g_bms.nominalCapacityAh;
    uint8_t nSoc = g_bms.soc;
    bool    nChg = g_bms.chargeMOSFETEnabled, nDis = g_bms.dischargeMOSFETEnabled;
    xSemaphoreGive(g_bmsMutex);

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
        case 0x80: nTMos = jk_temp(read_u16(&data[pos])); break;
        case 0x81: nTS1  = jk_temp(read_u16(&data[pos])); break;
        case 0x82: nTS2  = jk_temp(read_u16(&data[pos])); break;
        case 0x83: nV    = read_u16(&data[pos]) * 0.01f; break;
        case 0x84: nI    = jk_current(read_u16(&data[pos])); break;
        case 0x85: nSoc  = data[pos]; break;
        case 0x8C: {  // MOSFET-Status (Bits 0+1)
            uint16_t st = read_u16(&data[pos]);
            nChg = (st & 0x01) != 0;
            nDis = (st & 0x02) != 0;
            break;
        }
        case 0xAA: nNom = (float)read_u32(&data[pos]); break; // Einheit: Ah direkt (Doku: "4 HEX AH")
        default: break; // bekannte Länge, hier nicht ausgewertet
        }
        pos += plen;
    }

    bool plausible = !desync && values_plausible(nV, nI, nSoc, nTMos, nTS1, nTS2);

    if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    g_bms.frameCount++;
    if (!plausible) {
        // M-6 (erweitert v5.6.5): Desynchronisierter ODER unplausibler
        // Frame → NICHT übernehmen. g_bms bleibt auf dem letzten guten
        // Stand; valid/lastUpdateMs bleiben unverändert → die Staleness-
        // Prüfung in der Logik greift, statt Fantasiewerte als „gültig"
        // zu behandeln. Ein einzelner Ausreißer zwischen guten Frames
        // kostet keine Frische (gute Frames setzen lastUpdateMs);
        // dauerhafte Fehler führen sauber in den Stale-Zustand
        // (bms_ok=false → Fail-Safe).
        g_bms.errorCount++;
    } else {
        g_bms.tempMOS = nTMos; g_bms.tempSensor1 = nTS1; g_bms.tempSensor2 = nTS2;
        g_bms.totalVoltage = nV; g_bms.current = nI;
        g_bms.soc = nSoc; g_bms.nominalCapacityAh = nNom;
        g_bms.chargeMOSFETEnabled = nChg; g_bms.dischargeMOSFETEnabled = nDis;
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
    else if (desync)
        Serial.println("[BMS] Frame-Desync (Längenüberlauf) — Rest verworfen");
    else if (!plausible)
        Serial.printf("[BMS] Unplausibler Frame verworfen (V=%.2f I=%.2f SoC=%u "
                      "tMOS=%.1f tS1=%.1f tS2=%.1f)\n", nV, nI, nSoc, nTMos, nTS1, nTS2);
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

    // v5.6.5: LEN-Feld ist lt. Protokoll-PDF exakt "Gesamtlänge − 2"
    // (Kap. 4.2.2) → GENAU EIN gültiger Kandidat, kein Raten mehr.
    // Bytes über flen hinaus (Rest/nächstes Frame im Puffer) werden
    // nicht mit einbezogen — verhindert das Vermischen von Alt- und
    // Neu-Frame-Fragmenten bei zufällig passender Fallback-Checksumme.
    uint16_t lf   = ((uint16_t)rx_buf[2]<<8)|rx_buf[3];
    uint16_t flen = (uint16_t)(lf + 2);
    if (flen < 20 || flen > rx_len || !verify_checksum(rx_buf, flen)) {
        Serial.println("[BMS] Checksum/Länge FEHLER");
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
