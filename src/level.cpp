// ============================================================
//  level.cpp — Womo Energy Core (optionales Modul)
//  Elektronische Wasserwaage / Lagesensor (MMA8452Q)
// ============================================================
#include "level.h"
#include "config.h"
#include "clock.h"          // g_i2cMutex (gemeinsamer I2C-Bus)
#include <Wire.h>
#include <Preferences.h>
#include <math.h>

// ── MMA8452Q Register ─────────────────────────────────────────
#define MMA_REG_OUT_X_MSB  0x01
#define MMA_REG_WHO_AM_I   0x0D
#define MMA_REG_XYZ_CFG    0x0E
#define MMA_REG_CTRL1      0x2A
#define MMA_REG_CTRL2      0x2B
#define MMA_WHO_AM_I_VAL   0x2A     // Kennung MMA8452Q

#define NVS_NS  "level"
#define DEG     (57.29577951f)      // rad → Grad
#define RAD     (0.017453293f)      // Grad → rad

static Preferences  lprefs;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static LevelState   s_state;          // geteilter Cache (per s_mux geschützt)
static uint8_t      s_addr = 0;       // erkannte I2C-Adresse (0 = keiner)

// ── Konfiguration (RAM-Spiegel des NVS) ───────────────────────
static uint16_t c_track, c_wbase, c_rot;
static bool     c_invR, c_invP, c_enabled;
static float    c_zRoll, c_zPitch;

// ── Glättung (EMA über die g-Werte gegen Vibration/Erschütterung) ──
static float f_ax = 0, f_ay = 0, f_az = 1.0f;
static bool  f_init = false;

// ── MMA8452-Bustreiber — Aufrufer MUSS g_i2cMutex halten! ─────
static bool mma_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool mma_read(uint8_t reg, uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)s_addr, (int)n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}

static bool mma_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(MMA_REG_WHO_AM_I);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)addr, 1) != 1) return false;
    return Wire.read() == MMA_WHO_AM_I_VAL;
}

static bool mma_setup() {
    if (!mma_write(MMA_REG_CTRL1, 0x00)) return false;  // STANDBY
    if (!mma_write(MMA_REG_XYZ_CFG, 0x00)) return false; // ±2g, kein HPF (max. Auflösung)
    if (!mma_write(MMA_REG_CTRL2, 0x02)) return false;  // MODS=10: High-Resolution-Oversampling
    if (!mma_write(MMA_REG_CTRL1, 0x21)) return false;  // DR=50Hz + ACTIVE (12-bit)
    return true;
}

// 12-bit, links-justiert (MSB:LSB), ±2g → 1024 LSB/g
static bool mma_read_accel(float& ax, float& ay, float& az) {
    uint8_t b[6];
    if (!mma_read(MMA_REG_OUT_X_MSB, b, 6)) return false;
    int16_t rx = (int16_t)((b[0] << 8) | b[1]) >> 4;
    int16_t ry = (int16_t)((b[2] << 8) | b[3]) >> 4;
    int16_t rz = (int16_t)((b[4] << 8) | b[5]) >> 4;
    ax = rx / 1024.0f;
    ay = ry / 1024.0f;
    az = rz / 1024.0f;
    return true;
}

// ── NVS laden ─────────────────────────────────────────────────
static void cfg_load() {
    c_track   = lprefs.getUShort("track", DEFAULT_TRACK_MM);
    c_wbase   = lprefs.getUShort("wbase", DEFAULT_WHEELBASE_MM);
    c_rot     = lprefs.getUShort("rot",   0);
    c_invR    = lprefs.getBool  ("invR",  false);
    c_invP    = lprefs.getBool  ("invP",  false);
    c_enabled = lprefs.getBool  ("en",    true);
    c_zRoll   = lprefs.getFloat ("zRoll", 0.0f);
    c_zPitch  = lprefs.getFloat ("zPitch", 0.0f);
}

// Sensor erkennen + konfigurieren (hält selbst den Bus-Lock)
static bool detect_and_setup() {
    uint8_t found = 0;
    if (g_i2cMutex && xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if      (mma_probe(LEVEL_I2C_ADDR_PRIMARY)) found = LEVEL_I2C_ADDR_PRIMARY;
        else if (mma_probe(LEVEL_I2C_ADDR_ALT))     found = LEVEL_I2C_ADDR_ALT;
        s_addr = found;
        if (found && !mma_setup()) { s_addr = 0; found = 0; }
        xSemaphoreGive(g_i2cMutex);
    }
    return found != 0;
}

void level_init() {
    lprefs.begin(NVS_NS, false);
    cfg_load();

    memset(&s_state, 0, sizeof(s_state));
    s_state.az      = 1.0f;
    s_state.enabled = c_enabled;
    s_state.refWheel = 0;

    bool ok = detect_and_setup();
    s_state.present = ok;

    if (ok)
        Serial.printf("[LEVEL] MMA8452 @ 0x%02X erkannt (Spur %u / Radstand %u mm, Rot %u)\n",
                      s_addr, c_track, c_wbase, c_rot);
    else
        Serial.println("[LEVEL] Kein Lagesensor am I2C-Bus gefunden (Modul optional)");
}

// ── Neigung + Keilgeometrie berechnen und im Cache ablegen ────
static void recompute_and_store(float ax, float ay, float az) {
    // EMA-Glättung
    if (!f_init) { f_ax = ax; f_ay = ay; f_az = az; f_init = true; }
    else {
        const float a = 0.30f;
        f_ax += a * (ax - f_ax);
        f_ay += a * (ay - f_ay);
        f_az += a * (az - f_az);
    }

    // Roh-Neigung aus der Schwerkraftverteilung
    float rollRaw  = atan2f(f_ay, f_az) * DEG;
    float pitchRaw = atan2f(-f_ax, sqrtf(f_ay * f_ay + f_az * f_az)) * DEG;

    // Einbaudrehung um die Hochachse (Sensor ↔ Fahrzeuglängsachse)
    float r, p;
    switch (c_rot) {
        case 90:  r =  pitchRaw; p = -rollRaw;  break;
        case 180: r = -rollRaw;  p = -pitchRaw; break;
        case 270: r = -pitchRaw; p =  rollRaw;  break;
        default:  r =  rollRaw;  p =  pitchRaw; break;
    }
    if (c_invR) r = -r;
    if (c_invP) p = -p;

    // Null-Offset (Kalibrierung in der Ebene)
    float roll  = r - c_zRoll;
    float pitch = p - c_zPitch;

    // Keilhöhen: z_i = x_i·tan(roll) + y_i·tan(pitch) [mm]
    //   roll>0  → rechte Seite (+x) höher   pitch>0 → Front (+y) höher
    //   Räder:  VL(-x,+y) VR(+x,+y) HL(-x,-y) HR(+x,-y)
    float tr = tanf(roll * RAD);
    float tp = tanf(pitch * RAD);
    float hx = c_track * 0.5f;
    float hy = c_wbase * 0.5f;
    float z[4];
    z[0] = -hx * tr + hy * tp;   // VL
    z[1] =  hx * tr + hy * tp;   // VR
    z[2] = -hx * tr - hy * tp;   // HL
    z[3] =  hx * tr - hy * tp;   // HR

    uint8_t ref = 0;
    float   zmax = z[0];
    for (uint8_t i = 1; i < 4; i++) if (z[i] > zmax) { zmax = z[i]; ref = i; }

    float w[4];
    for (uint8_t i = 0; i < 4; i++) { w[i] = zmax - z[i]; if (w[i] < 0) w[i] = 0; }

    bool ok = fabsf(roll) < LEVEL_TOLERANCE_DEG && fabsf(pitch) < LEVEL_TOLERANCE_DEG;

    taskENTER_CRITICAL(&s_mux);
    s_state.ax = f_ax; s_state.ay = f_ay; s_state.az = f_az;
    s_state.rawRoll = r; s_state.rawPitch = p;
    s_state.roll = roll; s_state.pitch = pitch;
    for (uint8_t i = 0; i < 4; i++) s_state.wedge[i] = w[i];
    s_state.refWheel = ref;
    s_state.levelOk  = ok;
    s_state.valid    = true;
    s_state.lastMs   = millis();
    taskEXIT_CRITICAL(&s_mux);
}

void level_task(void*) {
    uint32_t   lastDetect = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        bool en = c_enabled;
        if (en && s_addr) {
            float ax, ay, az;
            bool got = false;
            if (g_i2cMutex && xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
                got = mma_read_accel(ax, ay, az);
                xSemaphoreGive(g_i2cMutex);
            }
            if (got) recompute_and_store(ax, ay, az);
            else {
                taskENTER_CRITICAL(&s_mux); s_state.valid = false; taskEXIT_CRITICAL(&s_mux);
            }
        } else if (en && !s_addr) {
            // Bring-up/Hotplug: alle 3s erneut nach dem Sensor suchen
            if ((uint32_t)(millis() - lastDetect) > 3000) {
                lastDetect = millis();
                bool ok = detect_and_setup();
                taskENTER_CRITICAL(&s_mux); s_state.present = ok; taskEXIT_CRITICAL(&s_mux);
            }
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LEVEL_POLL_INTERVAL_MS));
    }
}

// ── Cache-Schnappschuss (thread-sicher) ───────────────────────
void level_get(LevelState& out) {
    taskENTER_CRITICAL(&s_mux);
    out = s_state;
    taskEXIT_CRITICAL(&s_mux);
}

// ── JSON ──────────────────────────────────────────────────────
String level_to_json() {
    LevelState s;
    level_get(s);
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"present\":%s,\"valid\":%s,\"enabled\":%s,\"level\":%s,"
        "\"roll\":%.2f,\"pitch\":%.2f,\"ref\":%u,"
        "\"wedge\":[%.0f,%.0f,%.0f,%.0f],"
        "\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f}",
        s.present ? "true" : "false",
        s.valid   ? "true" : "false",
        s.enabled ? "true" : "false",
        s.levelOk ? "true" : "false",
        s.roll, s.pitch, s.refWheel,
        s.wedge[0], s.wedge[1], s.wedge[2], s.wedge[3],
        s.ax, s.ay, s.az);
    return String(buf);
}

String level_cfg_to_json() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"track\":%u,\"wheelbase\":%u,\"rot\":%u,"
        "\"invRoll\":%s,\"invPitch\":%s,\"enabled\":%s,"
        "\"zeroRoll\":%.2f,\"zeroPitch\":%.2f}",
        c_track, c_wbase, c_rot,
        c_invR ? "true" : "false",
        c_invP ? "true" : "false",
        c_enabled ? "true" : "false",
        c_zRoll, c_zPitch);
    return String(buf);
}

// ── Setter ────────────────────────────────────────────────────
bool level_set_track(uint16_t mm) {
    if (mm < 800 || mm > 3000) return false;
    c_track = mm; lprefs.putUShort("track", mm); return true;
}
bool level_set_wheelbase(uint16_t mm) {
    if (mm < 1500 || mm > 8000) return false;
    c_wbase = mm; lprefs.putUShort("wbase", mm); return true;
}
bool level_set_rot(uint16_t deg) {
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) return false;
    c_rot = deg; lprefs.putUShort("rot", deg); return true;
}
bool level_set_invert(bool invRoll, bool invPitch) {
    c_invR = invRoll; c_invP = invPitch;
    lprefs.putBool("invR", invRoll); lprefs.putBool("invP", invPitch);
    return true;
}
bool level_set_enabled(bool en) {
    c_enabled = en; lprefs.putBool("en", en);
    taskENTER_CRITICAL(&s_mux); s_state.enabled = en; taskEXIT_CRITICAL(&s_mux);
    return true;
}

// ── Kalibrierung ──────────────────────────────────────────────
bool level_calibrate(bool reset) {
    if (reset) {
        c_zRoll = 0.0f; c_zPitch = 0.0f;
        lprefs.putFloat("zRoll", 0.0f); lprefs.putFloat("zPitch", 0.0f);
        return true;
    }
    LevelState s;
    level_get(s);
    if (!s.present || !s.valid) return false;   // ohne gültige Messung nicht kalibrierbar
    c_zRoll = s.rawRoll; c_zPitch = s.rawPitch;
    lprefs.putFloat("zRoll", c_zRoll); lprefs.putFloat("zPitch", c_zPitch);
    return true;
}
