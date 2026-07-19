// ============================================================
//  Wio-E5 full-frame capture test (ArduCAM Mini 2MP Plus).
//  Built only by env:lora_e5_capture (-DE5_CAPTURE).
//  Proves the whole camera path on the E5: SPI ArduChip + I2C OV2640
//  + InitCAM + JPEG capture -> FIFO length.
//
//  Wiring (E5 -> camera):  SPI CS=PA0, SCK=PB13, MISO=PB14, MOSI=PA10
//                          I2C SDA=PA15, SCL=PB15,  VCC=3V3, GND=GND
//  Debug: CP2102 (USART1 PB6/PB7) @ 9600  (COM9)
//
//  Pin-reference rule (STM32duino):
//   - SPI/Wire setters take PinName (with underscore): PA_10, PB_14 ...
//   - pinMode/digitalWrite/ArduCAM CS take the Arduino-pin macro
//     (no underscore): PA0 (=22).  PA_0 would wrongly mean D0 = PB7.
// ============================================================
#ifdef E5_CAPTURE
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

#define CAM_CS   PA0        // physical PA0 (Arduino pin macro, NOT PA_0)

ArduCAM myCAM(OV2640, CAM_CS);

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(500);

    // Bus pins (PinName overloads -> correct physical pins)
    Wire.setSDA(PA_15);
    Wire.setSCL(PB_15);
    Wire.begin();
    SPI.setMOSI(PA_10);
    SPI.setMISO(PB_14);
    SPI.setSCLK(PB_13);
    SPI.begin();
    // The ArduCAM lib's STM32 path does bare SPI.transfer() with no
    // beginTransaction, so pin the peripheral to MODE0 @2MHz here and leave
    // it open (>=500kHz needed; the default mode/speed gives 0xFF).
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));

    // CS as a real output on physical PA0 (constructor ran pre-init; redo here)
    pinMode(CAM_CS, OUTPUT);
    digitalWrite(CAM_CS, HIGH);
}

void loop() {
    uint8_t vid = 0, pid = 0, temp;
    Serial.println("\n=== E5 CAPTURE TEST ===");

    // --- MANUAL diagnostic (bypass library) : proven-good probe style ---
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CAM_CS, LOW); SPI.transfer(0x07|0x80); SPI.transfer(0x80); digitalWrite(CAM_CS, HIGH);
    SPI.endTransaction(); delay(100);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CAM_CS, LOW); SPI.transfer(0x07|0x80); SPI.transfer(0x00); digitalWrite(CAM_CS, HIGH);
    SPI.endTransaction(); delay(100);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CAM_CS, LOW); SPI.transfer(0x00|0x80); SPI.transfer(0x55); digitalWrite(CAM_CS, HIGH);
    SPI.endTransaction();
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CAM_CS, LOW); SPI.transfer(0x00); uint8_t mv = SPI.transfer(0x00); digitalWrite(CAM_CS, HIGH);
    SPI.endTransaction();
    Serial.print("MANUAL reg0x00 = 0x"); Serial.println(mv, HEX);

    // CPLD reset
    myCAM.write_reg(0x07, 0x80); delay(100);
    myCAM.write_reg(0x07, 0x00); delay(100);

    // SPI bus check
    myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
    temp = myCAM.read_reg(ARDUCHIP_TEST1);
    if (temp != 0x55) {
        Serial.print("SPI FAIL (reg0x00=0x"); Serial.print(temp, HEX);
        Serial.println(") - retrying"); delay(1000); return;
    }
    Serial.println("SPI OK");

    // OV2640 identity
    myCAM.wrSensorReg8_8(0xff, 0x01);
    myCAM.rdSensorReg8_8(OV2640_CHIPID_HIGH, &vid);
    myCAM.rdSensorReg8_8(OV2640_CHIPID_LOW,  &pid);
    Serial.print("OV2640 VID=0x"); Serial.print(vid, HEX);
    Serial.print(" PID=0x"); Serial.println(pid, HEX);

    // Init + JPEG QVGA
    myCAM.set_format(JPEG);
    myCAM.InitCAM();
    myCAM.OV2640_set_JPEG_size(OV2640_320x240);
    delay(1000);
    myCAM.clear_fifo_flag();

    // Capture
    myCAM.start_capture();
    Serial.println("capturing 320x240 JPEG...");
    uint32_t t0 = millis();
    while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
        if (millis() - t0 > 3000) {
            Serial.println(">> capture TIMEOUT"); delay(1000); return;
        }
    }
    uint32_t len = myCAM.read_fifo_length();
    Serial.print(">> capture DONE, JPEG length = ");
    Serial.print(len); Serial.println(" bytes");
    Serial.println((len > 100 && len < 0x7fffff) ? ">> E5 CAMERA FULLY WORKING"
                                                 : ">> length invalid");
    delay(3000);
}
#endif // E5_CAPTURE
