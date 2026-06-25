// ============================================================
//  I2C-Scanner + Pin-Diagnose — DS3231 Bus-Fehlersuche
//  Temporär statt main.cpp. SDA=GPIO1, SCL=GPIO2.
//
//  Teil 1: Ruhepegel-Test der Leitungen OHNE I2C.
//          Ein gesunder I2C-Bus mit Pullups liegt im Ruhezustand
//          auf HIGH. Liest ein Pin LOW → Leitung tot/kurzgeschlossen
//          oder keine Versorgung am Modul → keine Pullups aktiv.
//  Teil 2: Adress-Scan 0x01..0x7F.
// ============================================================
#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA 1
#define I2C_SCL 2

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n=== DS3231 Bus-Diagnose ===");

    // ---- Teil 1: Ruhepegel ohne I2C ----
    // Pins als reine Eingänge OHNE interne Pullups → wir sehen den
    // echten Pegel, den die externen Modul-Pullups erzeugen.
    pinMode(I2C_SDA, INPUT);
    pinMode(I2C_SCL, INPUT);
    delay(50);
    int sdaIdle = digitalRead(I2C_SDA);
    int sclIdle = digitalRead(I2C_SCL);
    Serial.printf("Ruhepegel  SDA(GPIO%d)=%s   SCL(GPIO%d)=%s\n",
                  I2C_SDA, sdaIdle ? "HIGH" : "LOW",
                  I2C_SCL, sclIdle ? "HIGH" : "LOW");
    if (!sdaIdle || !sclIdle) {
        Serial.println(">> WARNUNG: Leitung liegt LOW im Ruhezustand!");
        Serial.println(">> -> Modul ohne Versorgung (3,3V/GND?), keine Pullups,");
        Serial.println(">>    Leitung gegen GND kurzgeschlossen, oder Pin falsch.");
    } else {
        Serial.println(">> Ruhepegel OK (beide HIGH) — Pullups & Versorgung plausibel.");
    }

    // ---- Teil 2: I2C-Scan ----
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    delay(100);
    Serial.println("\nStarte Adress-Scan ...");
}

void loop() {
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  Gerät @ 0x%02X", addr);
            if (addr == 0x68) Serial.print("  <- DS3231 RTC!");
            if (addr == 0x57) Serial.print("  <- AT24C32 EEPROM (DS3231-Modul)");
            Serial.println();
            found++;
        }
    }
    if (found == 0)
        Serial.println("  KEIN Gerät gefunden.");
    else
        Serial.printf("  %d Gerät(e).\n", found);
    Serial.println("---");
    delay(3000);
}
