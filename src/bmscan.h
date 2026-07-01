// ============================================================
//  bmscan.h — Womo Energy Core v5.4
//  JK-BMS CAN-Bus-Empfänger (Alternative zu bms.cpp/RS485)
//
//  100 % aufruf- und antwortkompatibel zu bms.h:
//    - identische Struct BMSData (aus bms.h)
//    - identische globale Symbole g_bms / g_bmsMutex
//    - identische Funktionen bms_init() / bms_poll() / bms_to_json()
//    - identischer JSON-Output (gleiche Keys/Formate)
//
//  Quelle: JIKONG "BMS-CAN communication protocol" V1.02 (20190428)
//    CAN 2.0A, Standard-Frame (11-Bit), 250 kbit/s, Little-Endian.
//    BMS sendet zyklisch (Push) — KEIN Polling, KEINE Query.
//
//  STROMKONVENTION:
//    CAN-Doku:  + = ENTLADEN, − = LADEN
//    intern:    + = LADEN,    − = ENTLADEN  → wird invertiert,
//               damit logic.cpp identisch zur RS485-Variante arbeitet.
//
//  TEMPERATUR-MAPPING (CAN liefert Max/Min/Avg statt 3 Sensoren):
//    tempMOS     ← MaxCellTemp
//    tempSensor1 ← MinCellTemp
//    tempSensor2 ← AvrgCellTemp   (→ wird an MPPT gesendet)
//
//  NICHT im CAN-Protokoll vorhanden → fest 0/false:
//    nominalCapacityAh, remainingCapacityAh,
//    chargeMOSFETEnabled, dischargeMOSFETEnabled
//
//  AUSWAHL RS485 vs. CAN (Compile-Zeit, beide Dateien bleiben erhalten):
//    Standard-Build  → bms.cpp aktiv (dieser Body ist via
//                      #ifdef BMS_USE_CAN inaktiv, keine Symbol-Kollision).
//    CAN-Build        → in platformio.ini:
//                          build_flags     = -D BMS_USE_CAN
//                          build_src_filter = +<*> -<bms.cpp>
//                      (bms.cpp aus dem Build nehmen, sonst doppelte Symbole.)
// ============================================================
#pragma once
#include "bms.h"          // Struct BMSData + g_bms/g_bmsMutex + Funktions-Prototypen

// ── CAN-Pins (Default: MAX485-Pins wiederverwendet) ──────────
// Bei CAN-Auswahl steckt statt des MAX485-Moduls der CAN-Transceiver
// (z. B. SN65HVD230, 3,3 V) an denselben Pins. Override in config.h möglich.
#ifndef CAN_TX_PIN
#define CAN_TX_PIN              17      // = UART1 TX / MAX485 DI
#endif
#ifndef CAN_RX_PIN
#define CAN_RX_PIN              16      // = UART1 RX / MAX485 RO
#endif

// RX-Queue-Tiefe: BATT_ST kommt alle 20 ms (~100 Frames je 2 s Poll-Zyklus).
// poll() leert die Queue komplett und behält die jeweils neuesten Werte.
#ifndef CAN_RX_QUEUE_LEN
#define CAN_RX_QUEUE_LEN        64
#endif

// ── JK-CAN Frame-Funktionscodes (high byte der 11-Bit-ID) ────
// ID = (FUNC << 8) | SA,  SA = 0xF4 + Geräteadresse (0xF4..0xFF).
// Auf FUNC matchen ⇒ adressunabhängig.
#define JKCAN_FUNC_BATT_ST      0x02    // 0x02F4: Spannung, Strom, SoC, Entladezeit
#define JKCAN_FUNC_CELL_VOLT    0x04    // 0x04F4: Max/Min-Zellspannung (Struct hat kein Feld → ignoriert)
#define JKCAN_FUNC_CELL_TEMP    0x05    // 0x05F4: Max/Min/Avg-Temperatur
#define JKCAN_FUNC_ALM_INFO     0x07    // 0x07F4: Alarm-Flags (Struct hat kein Feld → ignoriert)
#define JKCAN_SA_HIGH_NIBBLE    0xF0    // SA-Maske: gültig wenn (sa & 0xF0) == 0xF0
