// ============================================================
//  inverter.cpp — Womo Energy Core v5.4
//  Wechselrichter-Status — Renogy 12V/2000W mit NVS (2 LED-Eingänge)
//
//  Ersetzt den v5.0–v5.2-Stub (Edecoa-RJ12-Protokoll-Sniffer, nie
//  implementiert, da Edecoa gegen Renogy mit eingebauter NVS
//  getauscht wurde). Der Renogy-Fernbedienungsport hat KEIN
//  Datenprotokoll — die zwei LED-Pegel werden direkt als digitale
//  Eingänge gelesen, kein UART/RS485 nötig.
//
//  Debounce: WR_DEBOUNCE_SAMPLES aufeinanderfolgende übereinstimmende
//  Polls nötig, bevor sich der gemeldete Zustand ändert — filtert
//  kurze Störimpulse auf der Leitung (lange Strecke zum Heck-Einbauort
//  des Wechselrichters), ohne echte Zustandswechsel sichtbar zu
//  verzögern. inverter_poll() wird aus mppt_task() ca. alle 100ms
//  aufgerufen (s. main.cpp) → 3 Samples ≈ 300ms Verzögerung.
//
//  Cross-Core-Schutz: g_invMutex (wie g_bmsMutex/g_mpptMutex). Die
//  Hardware-Reads + Debounce laufen mutexfrei (nur Core 1 berührt die
//  s_*StableCount-Zähler); ausschließlich der Snapshot-Write nach
//  g_inverter und der JSON-Read sind durch den Mutex serialisiert.
// ============================================================
#include "inverter.h"
#include "config.h"

InverterData      g_inverter = {};
SemaphoreHandle_t g_invMutex = nullptr;

// Debounce-Zustand + Zähler: nur in inverter_poll() (Core 1) berührt →
// kein Mutex nötig. Bewusst von g_inverter getrennt, damit g_inverter
// AUSSCHLIESSLICH innerhalb des Mutex-Blocks geschrieben wird.
static bool    s_powerState       = false;
static bool    s_alarmState       = false;
static uint8_t s_powerStableCount = 0;
static uint8_t s_alarmStableCount = 0;
static const uint8_t WR_DEBOUNCE_SAMPLES = 3;

// Übernimmt 'raw' in 'state' erst nach WR_DEBOUNCE_SAMPLES gleichen
// Polls in Folge. Rückgabewert = aktueller (debounced) Zustand.
static bool debounce_level(bool raw, bool& state, uint8_t& counter) {
    if (raw == state) {
        counter = 0;
        return state;
    }
    counter++;
    if (counter >= WR_DEBOUNCE_SAMPLES) {
        state = raw;
        counter = 0;
    }
    return state;
}

void inverter_init() {
    g_invMutex = xSemaphoreCreateMutex();
    memset(&g_inverter, 0, sizeof(g_inverter));
    // Eingänge: externer Spannungsteiler liefert den Pegel UND zieht
    // den Pin im LED-AUS-Zustand auf GND (kein interner Pull, der den
    // Teiler verfälschen würde — gleiches Prinzip wie Landstrom-Sensor).
    pinMode(GPIO_WR_LED_POWER, INPUT);
    pinMode(GPIO_WR_LED_ALARM, INPUT);
    s_powerState = false; s_alarmState = false;
    s_powerStableCount = 0;
    s_alarmStableCount = 0;
    Serial.println("[INV] Renogy NVS Status-Eingaenge initialisiert (Power/Alarm)");
    Serial.printf ("[INV] GPIO Power=%d Alarm=%d — Pinbelegung/Spannungsteiler noch zu verifizieren!\n",
                   GPIO_WR_LED_POWER, GPIO_WR_LED_ALARM);
}

void inverter_poll() {
    bool rawPower = (digitalRead(GPIO_WR_LED_POWER) == WR_LED_ACTIVE);
    bool rawAlarm = (digitalRead(GPIO_WR_LED_ALARM) == WR_LED_ACTIVE);

    // Debounce mutexfrei auf lokalem State (nur dieser Task berührt ihn)
    bool power = debounce_level(rawPower, s_powerState, s_powerStableCount);
    bool alarm = debounce_level(rawAlarm, s_alarmState, s_alarmStableCount);

    // Snapshot-Write unter Mutex serialisieren (Leser: ws_task, Core 0),
    // damit der JSON-Leser keinen halb aktualisierten Satz
    // (power/alarm/valid/ts) erwischt. g_inverter wird AUSSCHLIESSLICH
    // hier geschrieben.
    if (g_invMutex && xSemaphoreTake(g_invMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_inverter.powerOn      = power;
        g_inverter.alarm        = alarm;
        g_inverter.valid        = true;
        g_inverter.lastUpdateMs = millis();
        xSemaphoreGive(g_invMutex);
    }
}

String inverter_to_json() {
    if (!g_invMutex || xSemaphoreTake(g_invMutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return "{\"valid\":false}";
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"valid\":%s,\"power_on\":%s,\"alarm\":%s,\"age_ms\":%lu}",
        g_inverter.valid   ? "true" : "false",
        g_inverter.powerOn ? "true" : "false",
        g_inverter.alarm   ? "true" : "false",
        (unsigned long)(millis() - g_inverter.lastUpdateMs));
    xSemaphoreGive(g_invMutex);
    return String(buf);
}
