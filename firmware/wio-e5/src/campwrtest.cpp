// ============================================================
//  Camera software power-down test. Built only by env:lora_e5_campwrtest.
//  Inits the OV2640 (runs ~137mA), then toggles the ArduChip's sensor-LDO
//  enable (GPIO_PWREN) OFF/ON every 6s. Watch the current on a multimeter
//  (in-line with the Mini VCC, or total): if the OFF windows collapse, the
//  ArduChip can power-gate the camera in software -> no MOSFET needed.
//  Debug out: CP2102 (PB6/PB7) @ 9600 announces each state.
// ============================================================
#ifdef CAM_PWR_TEST
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

#define CAM_CS  PA0

ArduCAM myCAM(OV2640, CAM_CS);

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(500);
    Wire.setSDA(PA_15); Wire.setSCL(PB_15); Wire.begin();
    SPI.setMOSI(PA_10); SPI.setMISO(PB_14); SPI.setSCLK(PB_13); SPI.begin();
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    pinMode(CAM_CS, OUTPUT); digitalWrite(CAM_CS, HIGH);

    myCAM.write_reg(0x07, 0x80); delay(100);
    myCAM.write_reg(0x07, 0x00); delay(100);
    myCAM.wrSensorReg8_8(0xff, 0x01);
    myCAM.set_format(JPEG);
    myCAM.InitCAM();
    myCAM.OV2640_set_JPEG_size(OV2640_320x240);
    delay(500);
    Serial.println("\ncamera initialized (running ~137mA)");
}

void loop() {
    // LDO OFF (attempt software power-gate of the OV2640)
    myCAM.clear_bit(ARDUCHIP_GPIO, GPIO_PWREN_MASK);
    myCAM.set_bit(ARDUCHIP_GPIO, GPIO_PWDN_MASK);   // also assert standby
    Serial.println("CAM OFF (PWREN=0, PWDN=1) -- measure current now");
    Serial.flush();
    delay(6000);

    // LDO ON again
    myCAM.clear_bit(ARDUCHIP_GPIO, GPIO_PWDN_MASK);
    myCAM.set_bit(ARDUCHIP_GPIO, GPIO_PWREN_MASK);
    Serial.println("CAM ON  (PWREN=1, PWDN=0) -- measure current now");
    Serial.flush();
    delay(6000);
}
#endif // CAM_PWR_TEST
