/*
 * =============================================================
 * INA219 Current Logger -- ESP8266 D1 Mini  (7-Phase AMR)
 * =============================================================
 * Migrated from INA226 to INA219 (12-bit, ~1 kHz, Adafruit lib).
 * INA226 hardware was unresponsive; INA219 is the replacement.
 * Phase boundaries detected via SoftwareSerial UART bytes from DUT.
 * No fixed delay -- samples at INA219 maximum rate (~1 kHz).
 *
 * Wiring:
 *   INA219 SDA  -> D2 (GPIO4)
 *   INA219 SCL  -> D1 (GPIO5)
 *   INA219 VCC  -> 3.3V
 *   INA219 GND  -> GND
 *   INA219 VIN+ -> USB VBUS (after D+/D- cut on DUT cable)
 *   INA219 VIN- -> DUT 5V header pin
 *
 *   DUT GPIO43 (TX1) -> D5 (GPIO14)   <- phase marker UART RX
 *   Common GND between D1 Mini and DUT
 *
 *   D1 Mini USB: intact -- powers logger + serial output to PC.
 *   DUT USB: D+/D- cut, VBUS through INA219, no serial to PC.
 *
 * Build flags (platformio.ini):
 *   -DSHUNT_CONFIG=0   Run A: active phases, 0.1 ohm shunt, 32V/2A range
 *   -DSHUNT_CONFIG=1   Run B: sleep only,    10  ohm shunt, 16V/400mA range
 *
 * INA219 key properties:
 *   - I2C address 0x40 (A0=A1=GND)
 *   - 12-bit ADC, +/-0.5% accuracy
 *   - Conversion time: ~532 us per sample (continuous mode)
 *   - Theoretical max: ~1887 Hz; practical on ESP8266: ~800-1200 Hz
 *   - No hardware averaging register (unlike INA226) -- Python post-processes
 *
 * Author     : Youness Chakir -- Chouaib Doukkali University
 * Supervisor : Prof. Abdessadek Aaroud
 * =============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <Adafruit_INA219.h>

// =============================================================
// HARDWARE CONFIG
// =============================================================
#define I2C_SDA_PIN     4       // D2
#define I2C_SCL_PIN     5       // D1
#define INA219_ADDR     0x40    // A0=A1=GND (default)
#define PHASE_RX_PIN    14      // D5 -- SoftwareSerial RX from DUT TX
#define SERIAL_BAUD     500000  // USB output baud rate
#define MARKER_BAUD     9600    // must match DUT UART baud

// =============================================================
// SHUNT CONFIG -- set via -DSHUNT_CONFIG=0 or =1 in platformio.ini
// Run A (active phases): 0.1 ohm, 32V/2A calibration range
// Run B (sleep only):    10  ohm, 16V/400mA calibration (higher sensitivity)
// When swapping shunt resistors: change SHUNT_CONFIG and reflash.
// =============================================================
#ifndef SHUNT_CONFIG
  #define SHUNT_CONFIG  0
#endif

#if SHUNT_CONFIG == 0
  #define SHUNT_LABEL   "Run A -- 0.1 ohm (active phases, 32V/2A range)"
#else
  #define SHUNT_LABEL   "Run B -- 10 ohm  (sleep only, 16V/400mA range)"
#endif

// =============================================================
// MEASUREMENT PROTOCOL BYTES  (must match DUT firmware)
// =============================================================
#define MARK_SYNC       0xFF
#define MARK_CYCLE_END  0xA0
#define MARK_PHASE(n)   (0xA0 | (n))  // 0xA1..0xA7 (Platform A=7 phases, C=6 phases)
#define NUM_PHASES      7              // max phases across all platforms

// =============================================================
// OBJECTS
// =============================================================
Adafruit_INA219 ina219(INA219_ADDR);
SoftwareSerial  phaseSerial(PHASE_RX_PIN, -1, false);  // RX only

// =============================================================
// PER-PHASE STATISTICS
// =============================================================
struct PhaseStats {
    double        sumI_mA;   // sum of current samples (mA)
    double        sumE_mJ;   // accumulated energy (mJ)
    float         maxI_mA;   // peak current (mA)
    unsigned long nSamples;  // INA219 reads in this phase
    unsigned long tStart_ms; // millis() at phase start marker
    unsigned long tEnd_ms;   // millis() at phase end (= next marker)
};

PhaseStats    stats[NUM_PHASES + 1];  // index 0 = pre-cycle / idle
unsigned long cycleCount  = 0;
unsigned long sampleCount = 0;
unsigned long loggerStart = 0;

int  currentPhase = 0;     // 0 = idle, 1-7 = active phases
bool inCycle      = false;
bool syncReceived = false;

// Timing for energy integration (dt between consecutive reads)
unsigned long lastSampleTime_us = 0;

// =============================================================
// STATS MANAGEMENT
// =============================================================
void resetStats() {
    for (int i = 0; i <= NUM_PHASES; i++) {
        stats[i] = {0.0, 0.0, 0.0f, 0, 0, 0};
    }
}

// =============================================================
// CYCLE SUMMARY PRINT
// =============================================================
void printCycleSummary() {
    Serial.println();
    Serial.print("# ===== CYCLE ");
    Serial.print(cycleCount);
    Serial.print(" | CONFIG ");
    Serial.println(SHUNT_LABEL);
    Serial.println("# Phase           | Avg_mA  | Peak_mA | Dur_ms    | Energy_mJ");
    Serial.println("# ----------------|---------|---------|-----------|----------");

    double totalEnergy_mJ = 0.0;
    for (int i = 1; i <= NUM_PHASES; i++) {
        if (stats[i].nSamples == 0) {
            Serial.print("# P");
            Serial.print(i);
            Serial.println(" -- no samples (marker not received)");
            continue;
        }
        float avgI   = (float)(stats[i].sumI_mA  / stats[i].nSamples);
        float peakI  = stats[i].maxI_mA;
        float dur_ms = (float)(stats[i].tEnd_ms - stats[i].tStart_ms);
        float eng_mJ = (float)stats[i].sumE_mJ;
        totalEnergy_mJ += eng_mJ;

        // Machine-readable CSV line -- parsed by analysis/parse_logs.py
        // Format: PHASE_CSV,cycle,phase,avg_mA,peak_mA,dur_ms,energy_mJ
        Serial.print("PHASE_CSV,");
        Serial.print(cycleCount);  Serial.print(",");
        Serial.print(i);           Serial.print(",");
        Serial.print(avgI, 3);     Serial.print(",");
        Serial.print(peakI, 3);    Serial.print(",");
        Serial.print(dur_ms, 1);   Serial.print(",");
        Serial.println(eng_mJ, 4);

        // Human-readable comment
        Serial.print("# P");
        Serial.print(i);
        Serial.print("               | ");
        Serial.print(avgI, 2);   Serial.print("    | ");
        Serial.print(peakI, 2);  Serial.print("    | ");
        Serial.print(dur_ms, 0); Serial.print("        | ");
        Serial.println(eng_mJ, 4);
    }

    Serial.print("# TOTAL ACTIVE    |         |         |           | ");
    Serial.println((float)totalEnergy_mJ, 4);

    if (stats[0].nSamples > 0) {
        float avgSleep = (float)(stats[0].sumI_mA / stats[0].nSamples);
        Serial.print("# Idle/Sleep avg  | ");
        Serial.print(avgSleep, 4);
        Serial.print(" mA = ");
        Serial.print(avgSleep * 1000.0f, 2);
        Serial.println(" uA");
    }
    Serial.println("# =============================================");
    Serial.println();
}

// =============================================================
// PHASE TRANSITION HANDLER
// Called whenever a valid marker byte is received on SoftwareSerial.
// =============================================================
void handleMarker(uint8_t byte) {
    unsigned long now_ms = millis();

    if (byte == MARK_SYNC) {
        if (!syncReceived) {
            syncReceived = true;
            Serial.println("# SYNC received -- DUT booted");
        }
        return;
    }

    if (byte == MARK_CYCLE_END) {
        // Close the last active phase
        if (inCycle && currentPhase >= 1 && currentPhase <= NUM_PHASES) {
            stats[currentPhase].tEnd_ms = now_ms;
        }
        cycleCount++;
        printCycleSummary();
        resetStats();
        currentPhase = 0;
        inCycle      = false;
        return;
    }

    // Phase start markers: 0xA1..0xA7
    if (byte >= MARK_PHASE(1) && byte <= MARK_PHASE(NUM_PHASES)) {
        int newPhase = byte & 0x0F;

        // Close previous phase
        if (inCycle && currentPhase >= 1 && currentPhase < newPhase) {
            stats[currentPhase].tEnd_ms = now_ms;
        }

        // Open new phase
        currentPhase = newPhase;
        stats[currentPhase].tStart_ms = now_ms;

        if (newPhase == 1) {
            inCycle = true;
            Serial.print("# --- Cycle ");
            Serial.print(cycleCount + 1);
            Serial.println(" starting ---");
        }
    }
}

// =============================================================
// INA219 SETUP
// =============================================================
void setupINA219() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);  // 400 kHz fast mode

    if (!ina219.begin()) {
        Serial.println("ERROR: INA219 not found! Check SDA->D2, SCL->D1, VCC->3V3");
        while (true) delay(1000);
    }

    // Select calibration range based on shunt config
#if SHUNT_CONFIG == 0
    // Run A: active phases -- expect up to ~500 mA peak (CNN, WiFi, LoRa TX)
    // 32V/2A range: current LSB = 0.1 mA, adequate for all active phases
    ina219.setCalibration_32V_2A();
#else
    // Run B: sleep current only -- expect <5 mA (deep sleep 2-15 uA)
    // 16V/400mA range: current LSB = 0.04878 mA -- higher sensitivity
    ina219.setCalibration_16V_400mA();
#endif

    Serial.println("# INA219 OK");
    Serial.print("# Shunt config: "); Serial.println(SHUNT_LABEL);
    Serial.println("# Conv time  : ~532 us per sample (continuous mode)");
    Serial.println("# Theor. max : ~1887 Hz | Practical on ESP8266: ~800-1200 Hz");
}

// =============================================================
// SETUP
// =============================================================
void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial) delay(10);

    Serial.println("# ============================================");
    Serial.println("# INA219 Logger -- ESP8266 D1 Mini");
    Serial.println("# 7-Phase AMR Energy Profiling (Paper 3)");
    Serial.println("# Phase detection: SoftwareSerial UART bytes");
    Serial.println("# ============================================");
    Serial.print("# Shunt config: ");
    Serial.println(SHUNT_LABEL);
    Serial.println();

    setupINA219();

    // SoftwareSerial for phase markers from DUT
    phaseSerial.begin(MARKER_BAUD);
    Serial.println("# SoftwareSerial RX ready on GPIO14 (D5) at 9600 baud");

    // CSV header for raw sample stream (parsed by analysis/parse_logs.py)
    Serial.println("timestamp_ms,current_mA,voltage_V,power_mW,phase");

    loggerStart       = millis();
    lastSampleTime_us = micros();
    resetStats();

    Serial.println("# Waiting for DUT sync byte (0xFF)...");
}

// =============================================================
// MAIN LOOP -- runs as fast as possible
// No fixed delay: rate limited by INA219 conversion time (~532 us)
// and ESP8266 I2C transaction overhead at 400 kHz.
// =============================================================
void loop() {
    // ----------------------------------------------------------
    // 1. Check for phase marker bytes (non-blocking)
    //    SoftwareSerial is interrupt-driven -- byte ready when available
    // ----------------------------------------------------------
    if (phaseSerial.available()) {
        uint8_t b = (uint8_t)phaseSerial.read();
        handleMarker(b);
    }

    // ----------------------------------------------------------
    // 2. Read INA219 (returns last conversion result immediately)
    //    In continuous mode the chip always has fresh data every ~532 us.
    //    We read as fast as the I2C bus allows; no blocking wait needed.
    // ----------------------------------------------------------
    float current_mA = ina219.getCurrent_mA();
    float busVoltage  = ina219.getBusVoltage_V();
    float power_mW    = ina219.getPower_mW();

    // Clamp noise floor to zero (INA219 can read slightly negative at idle)
    if (current_mA < 0.0f) current_mA = 0.0f;

    // ----------------------------------------------------------
    // 3. Energy integration using measured dt and measured voltage
    //    E (mJ) = I (mA) x V (V) x dt (ms) / 1000
    //    Using measured voltage is more accurate than assuming 5.0V.
    // ----------------------------------------------------------
    unsigned long now_us = micros();
    float dt_ms = (float)(now_us - lastSampleTime_us) / 1000.0f;
    lastSampleTime_us = now_us;

    float dE_mJ = current_mA * busVoltage * (dt_ms / 1000.0f);  // mW x s = mJ

    // ----------------------------------------------------------
    // 4. Accumulate into current phase stats
    // ----------------------------------------------------------
    int si = inCycle ? currentPhase : 0;
    if (si >= 0 && si <= NUM_PHASES) {
        stats[si].sumI_mA += current_mA;
        stats[si].sumE_mJ += dE_mJ;
        stats[si].nSamples++;
        if (current_mA > stats[si].maxI_mA)
            stats[si].maxI_mA = current_mA;
    }

    // ----------------------------------------------------------
    // 5. Output raw CSV line (every sample -- full waveform data)
    // ----------------------------------------------------------
    unsigned long ts = millis() - loggerStart;
    Serial.print(ts);             Serial.print(',');
    Serial.print(current_mA, 3);  Serial.print(',');
    Serial.print(busVoltage, 4);  Serial.print(',');
    Serial.print(power_mW, 3);    Serial.print(',');
    Serial.println(si);

    sampleCount++;
    // No delay -- next iteration reads INA219 again immediately.
    // ESP8266 I2C overhead naturally paces us to ~800-1200 Hz.
}
