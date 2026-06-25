// ============================================================
//  bmscan.cpp — Womo Energy Core v5.0
//  JK-BMS CAN-Bus-Empfänger (TWAI), Alternative zu bms.cpp
//
//  Aktiv NUR wenn BMS_USE_CAN definiert ist (sonst leer übersetzt →
//  keine Symbol-Kollision mit bms.cpp im Standard-Build).
//  Siehe bmscan.h für Aktivierung via platformio.ini.
//
//  Protokoll: JIKONG BMS-CAN V1.02 — CAN 2.0A, 250 kbit/s, Little-Endian,
//             Push (zyklisch). Empfang im LISTEN_ONLY-Modus (kein ACK,
//             keine Bus-Beeinflussung, kein zweiter Knoten nötig).
// ============================================================
#ifdef BMS_USE_CAN

#include "bmscan.h"
#include "config.h"
#include "driver/twai.h"

// ── Globale Symbole (identisch zu bms.cpp — nur eine .cpp im Build!) ──
BMSData           g_bms      = {};
SemaphoreHandle_t g_bmsMutex = nullptr;

// ── Little-Endian-Leser (CAN ist LE — Gegenteil zum RS485-4E57-Frame!) ──
static inline uint16_t rd_u16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void note_error() {
    if (g_bmsMutex && xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_bms.errorCount++;
        xSemaphoreGive(g_bmsMutex);
    }
}

// ── Init: TWAI-Treiber installieren und starten ──────────────
void bms_init() {
    g_bmsMutex = xSemaphoreCreateMutex();
    memset(&g_bms, 0, sizeof(g_bms));

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = CAN_RX_QUEUE_LEN;
    g.tx_queue_len = 0;                          // wir senden nie

    twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        Serial.println("[BMSCAN] TWAI driver install FEHLER");
        return;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("[BMSCAN] TWAI start FEHLER");
        return;
    }
    Serial.println("[BMSCAN] TWAI TX=" + String(CAN_TX_PIN) +
                   " RX=" + String(CAN_RX_PIN) + " @250k LISTEN_ONLY");
}

// ── Poll: gesamte RX-Queue leeren, neueste Werte je Frame-Typ behalten ──
//    Kein Senden, kein Warten — rein nicht-blockierend (Push-Protokoll).
bool bms_poll() {
    bool gotBatt = false, gotTemp = false;

    // Lokale Akkumulatoren (erst nach dem Drainen unter Mutex committen)
    float   volt = 0.0f, curr = 0.0f;
    uint8_t soc  = 0;
    float   tMax = 0.0f, tMin = 0.0f, tAvg = 0.0f;

    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {        // 0 = nicht blockieren
        if (msg.rtr || msg.extd) continue;           // nur Standard-Datenframes
        const uint8_t sa   = (uint8_t)(msg.identifier & 0xFF);
        const uint8_t func = (uint8_t)((msg.identifier >> 8) & 0xFF);
        if ((sa & JKCAN_SA_HIGH_NIBBLE) != JKCAN_SA_HIGH_NIBBLE) continue;  // SA 0xF4..0xFF

        const uint8_t* d = msg.data;

        switch (func) {
        case JKCAN_FUNC_BATT_ST:                     // 0x02F4 — Spannung/Strom/SoC
            if (msg.data_length_code < 5) break;
            volt = rd_u16_le(&d[0]) * 0.1f;          // 0.1 V/Bit
            // Doku: BattCurr = raw*0.1 − 400 (A), + = ENTLADEN
            // intern invertieren → + = LADEN
            curr = -(rd_u16_le(&d[2]) * 0.1f - 400.0f);
            soc  = d[4];                             // %
            // d[6..7] = Entladezeit (h) — kein Struct-Feld, ignoriert
            gotBatt = true;
            break;

        case JKCAN_FUNC_CELL_TEMP:                   // 0x05F4 — Max/Min/Avg-Temp
            if (msg.data_length_code < 5) break;
            tMax = (float)d[0] - 50.0f;              // Offset −50 °C
            tMin = (float)d[2] - 50.0f;
            tAvg = (float)d[4] - 50.0f;
            gotTemp = true;
            break;

        // 0x04F4 (CELL_VOLT) und 0x07F4 (ALM_INFO): Struct hat keine Felder
        // dafür → bewusst geparst-verworfen (kein Speichern).
        default:
            break;
        }
    }

    if (!gotBatt && !gotTemp) {                      // Bus still / kein Frame
        note_error();
        return false;
    }

    if (xSemaphoreTake(g_bmsMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    if (gotBatt) {
        g_bms.totalVoltage = volt;
        g_bms.current      = curr;
        g_bms.soc          = soc;
    }
    if (gotTemp) {
        g_bms.tempMOS     = tMax;                    // Max
        g_bms.tempSensor1 = tMin;                    // Min
        g_bms.tempSensor2 = tAvg;                    // Avg → MPPT
    }

    g_bms.power = g_bms.totalVoltage * g_bms.current;

    // Im CAN-Protokoll nicht vorhanden → fest 0/false
    g_bms.nominalCapacityAh    = 0.0f;
    g_bms.remainingCapacityAh  = 0.0f;
    g_bms.chargeMOSFETEnabled    = false;
    g_bms.dischargeMOSFETEnabled = false;

    // valid/Staleness nur bei frischem BATT_ST (mirror RS485: vollständiger Datensatz)
    if (gotBatt) {
        g_bms.valid        = true;
        g_bms.lastUpdateMs = millis();
        g_bms.frameCount++;
    }

    xSemaphoreGive(g_bmsMutex);
    return true;
}

// ── JSON-Ausgabe: byte-identisch zu bms.cpp (gleiche Keys/Formate) ──
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

#endif // BMS_USE_CAN
