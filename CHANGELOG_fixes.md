# Umgesetzte Fixes — Womo Energy Core v5.4 (Review-Runde)

Geänderte Dateien: `main.cpp`, `watchdog.cpp`, `bms.cpp`, `http_server.cpp`,
`logic.cpp`, `logger.cpp`, `clock.cpp`. Nur chirurgische Diffs gegen den
aktuellen Projektstand. **K-5 und B-8 wurden bewusst NICHT angefasst** (siehe unten).

## Block 1 — Crash-/Bootloop-Prävention
- **K-1 (USB-CDC-Stall)**
  - `main.cpp`: `Serial.setTxTimeoutMs(0)` direkt nach `Serial.begin` → USB-CDC
    verwirft TX statt zu blockieren, wenn kein Host liest.
  - `bms.cpp::parse_frame`: die `Serial.printf`-Ausgabe bei korrupter ID wird
    nicht mehr **unter** `g_bmsMutex` gemacht — `bad_id` wird gemerkt und erst
    NACH `xSemaphoreGive` geloggt. (Der gefährlichste Pfad: Serial-Block bei
    gehaltenem BMS-Mutex → SW-WDT-Reboot.)
- **K-2 (WebSocket UAF)**
  - `http_server.cpp::webserver_broadcast`: kein `ws.textAll()` mehr aus dem
    ws_task. Stattdessen Iteration über `ws.getClients()` mit
    `status()==WS_CONNECTED && canSend()` und `c->text(...)` je Client
    (Backpressure inklusive). Race-Fenster deutlich verkleinert.

## Block 2 — Sicherheitslogik
- **B-11 (verzögertes Hart-AUS)**
  - `logic.cpp`: für D+, Gel und WR je eine Express-Route. Bei hartem Grund
    (`!bms_ok` / `!mppt_ok` / `landstrom`) wird der Aktor **sofort** geschaltet,
    der Debounce für diesen Zyklus übersprungen **und `db_*` = 0 gesetzt** (sonst
    könnte ein stehen gebliebener Zähler im Folgezyklus zurückschalten). Der
    Manual-Override bleibt Vorrang (Express liegt im Auto-Zweig).
    WR-Hart = nur `!bms_ok` (gemäß Header-Semantik).

## Block 3 — Stabilität / Speicher / Datenintegrität
- **K-4 (Stack-Overflow-Schutz)**
  - `logger.cpp`: die 1920-B-Arrays `LogEntry local[60]` sind jetzt Off-Stack —
    **zwei getrennte** statische Puffer `s_flushBuf` (logger_task) und
    `s_emergBuf` (wdt_task), da beide Pfade gleichzeitig laufen können.
  - `main.cpp`: logger-Stack 4096 → 6144. `watchdog.cpp`: wdt_task 4096 → 6144.
- **K-3 / B-14 (SD-Schonung + kürzere Mutex-Haltezeit)**
  - `logger.cpp::write_batch_to_sd`: öffnet die Tagesdatei **einmal pro Batch**
    statt 60× open/close pro Zeile. `write_row_to_sd` entfernt. Verkürzt die
    `g_sdMutex`-Haltezeit drastisch → parallele Web-SD-Zugriffe treiben den
    Flush nicht mehr in den Timeout (kein Batch-Verlust mehr beim Log-Ansehen).
- **B-15 (SD-Laufzeit-Recovery)**
  - `logger.cpp`: `sd_try_remount()` + Logik in `logger_flush_sd`: bei fehlender
    SD alle 60 s Remount versuchen; bei Schreibfehler `s_sd_ok=false` setzen.

## Block 4 — Verschleiß / Performance
- **B-18 (NVS-Flash-Verschleiß)**
  - `clock.cpp::clock_set_epoch`: Hysterese. DS3231 wird nur bei > 5 s Abweichung
    gestellt; NVS nur bei > 10 min Drift seit letzter Persistenz (analog
    `clock_persist`). Beendet die NVS-Writes bei jedem 5-min-Dashboard-Sync.
- **H-4 (String-Churn)**
  - `http_server.cpp::build_live_json`: `j.reserve(1200)` gegen Reallokations-
    Fragmentierung beim 2-s-Broadcast.

---

## Bewusst ZURÜCKGESTELLT
- **K-5 (leerer POST → keine Antwort):** auf deinen Wunsch zurückgestellt.
  Zusätzlicher Grund: erst gegen die verbaute ESPAsyncWebServer-Version
  verifizieren (echter Leer-POST), und der Fix muss über `contentLength()==0`
  laufen — **nicht** über `_tempObject` (das ist im Erfolgsfall bereits `nullptr`
  → würde gültige POSTs mit 400 beantworten).
- **B-8 (`find_data_end`):** bewusst NICHT geändert. Der Parser ist am Gerät
  verifiziert; eine Umstellung braucht zuerst einen `BMS_DEBUG_RAW=1`-Hexdump
  zur Bestätigung des Trailer-Layouts. Blind ändern = Risiko, einen
  funktionierenden Parser zu brechen.

## Rest-/Verifikationspunkte (nicht in dieser Runde)
- H-3-Architektur: NVS-Writes der `params`-Setter noch im AsyncTCP-Kontext
  (Hysterese deckt den Zeit-Wear ab; das Deferred-Apply der übrigen Setter ist
  offen).
- K-2-Restrisiko: vollständig sicher wäre der Broadcast nur im AsyncTCP-Kontext.
  Falls der Fork `canSend()` nicht kennt: durch `!c->queueIsFull()` ersetzen.
- `beginResponseStream`-Semantik für große `/api/buffer`-Antworten prüfen.
- `uxTaskGetStackHighWaterMark()` nach den Stack-Änderungen für alle Tasks messen.
