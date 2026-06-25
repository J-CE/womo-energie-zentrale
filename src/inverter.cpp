// ============================================================
//  inverter.cpp — Womo Energy Core v5.0
//  Wechselrichter RJ12-Sniffer — STUB (Phase 2)
//
//  Edecoa WR: RJ12, Protokoll unbekannt.
//  Alle Funktionen sind No-Ops bis zur Protokollanalyse.
//  UART_INV_RX/TX = -1, kein UART initialisiert.
// ============================================================
#include "inverter.h"
#include "config.h"

InverterData g_inverter = {};

void inverter_init() {
    memset(&g_inverter, 0, sizeof(g_inverter));
    strlcpy(g_inverter.rawProtocol, "?", sizeof(g_inverter.rawProtocol));
    Serial.println("[INV] Stub — RJ12 Protokoll noch nicht bekannt");
    Serial.println("[INV] Multimeter-Messung am Edecoa RJ12 ausstehend");
}

bool inverter_poll() { return false; }

String inverter_to_json() {
    return "{\"valid\":false,\"note\":\"Protokoll ausstehend\"}";
}