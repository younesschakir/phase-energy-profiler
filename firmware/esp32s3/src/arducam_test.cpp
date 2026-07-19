// ============================================================
//  ArduCAM Mini 2MP Plus (B0067/B006701) bring-up test
//  Platform: XIAO ESP32-S3  (native-good SPI host)
//  Built ONLY by env:arducam_test (-DARDUCAM_TEST, main.cpp excluded).
//
//  Purpose: run Arducam's OFFICIAL library init (incl. the CPLD reset
//  the minimal E5 probe skipped) to decide module-good vs module-faulty,
//  and capture the exact working init to port back to the Wio-E5.
//
//  Wiring  Mini  ->  XIAO ESP32-S3 pin (GPIO)
//    CS   -> D3  (GPIO4)
//    SCK  -> D8  (GPIO7)
//    MISO -> D9  (GPIO8)
//    MOSI -> D10 (GPIO9)
//    SDA  -> D4  (GPIO5)
//    SCL  -> D5  (GPIO6)
//    VCC  -> 3V3     GND -> GND
// ============================================================
#ifdef ARDUCAM_TEST
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

#define CAM_CS    4    // D3
#define PIN_SCK   7    // D8
#define PIN_MISO  8    // D9
#define PIN_MOSI  9    // D10
#define PIN_SDA   5    // D4
#define PIN_SCL   6    // D5

ArduCAM myCAM(OV2640, CAM_CS);

void runTest();

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n[boot] serial up"); Serial.flush();
    Wire.begin(PIN_SDA, PIN_SCL);
    Serial.println("[boot] wire ok"); Serial.flush();
    pinMode(CAM_CS, OUTPUT);
    digitalWrite(CAM_CS, HIGH);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, CAM_CS);
    Serial.println("[boot] spi ok"); Serial.flush();
}

// Repeats forever so the result can be caught whenever the USB-CDC
// port is opened (a reset re-enumerates and drops boot-time prints).
void loop() {
    Serial.println("[loop] tick"); Serial.flush();
    runTest();
    delay(3000);
}

void runTest() {
    Serial.println("\n=== ArduCAM Mini 2MP Plus test (XIAO ESP32-S3) ===");

    // --- CPLD reset (the step the minimal E5 probe skipped) ---
    myCAM.write_reg(0x07, 0x80);
    delay(100);
    myCAM.write_reg(0x07, 0x00);
    delay(100);

    // --- 1. SPI bus test on ArduChip test register ---
    bool spi_ok = false;
    for (int i = 0; i < 5; i++) {
        myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
        uint8_t t = myCAM.read_reg(ARDUCHIP_TEST1);
        Serial.print("  SPI test reg0x00 = 0x"); Serial.println(t, HEX);
        if (t == 0x55) { spi_ok = true; break; }
        delay(200);
    }
    if (!spi_ok) {
        Serial.println(">> SPI interface ERROR - ArduChip not responding");
        Serial.println("=== stop ===");
        return;
    }
    Serial.println(">> SPI interface OK");

    // --- 2. OV2640 identity over the ArduChip's SCCB bridge ---
    uint8_t vid = 0, pid = 0;
    myCAM.wrSensorReg8_8(0xff, 0x01);
    myCAM.rdSensorReg8_8(OV2640_CHIPID_HIGH, &vid);
    myCAM.rdSensorReg8_8(OV2640_CHIPID_LOW,  &pid);
    Serial.print("  OV2640 VID=0x"); Serial.print(vid, HEX);
    Serial.print(" PID=0x"); Serial.println(pid, HEX);
    if (vid == 0x26 && (pid == 0x41 || pid == 0x42))
        Serial.println(">> OV2640 detected");
    else
        Serial.println(">> OV2640 NOT detected (via ArduChip)");

    // --- 3. Full capture: prove end-to-end frame grab ---
    myCAM.set_format(JPEG);
    myCAM.InitCAM();
    myCAM.OV2640_set_JPEG_size(OV2640_320x240);
    delay(1000);
    myCAM.clear_fifo_flag();
    myCAM.start_capture();
    Serial.println("  capturing 320x240 JPEG...");
    uint32_t t0 = millis();
    while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
        if (millis() - t0 > 3000) {
            Serial.println(">> capture TIMEOUT (CAP_DONE never set)");
            Serial.println("=== stop ===");
            return;
        }
    }
    uint32_t len = myCAM.read_fifo_length();
    Serial.print(">> capture DONE, JPEG length = ");
    Serial.print(len); Serial.println(" bytes");
    Serial.println((len > 0 && len < 0x7fffff)
                   ? ">> MODULE FULLY WORKING"
                   : ">> capture length invalid");
    Serial.println("=== test complete ===");
}
#endif // ARDUCAM_TEST
