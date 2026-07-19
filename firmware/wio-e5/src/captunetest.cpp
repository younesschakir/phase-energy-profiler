// ============================================================
//  Capture-tuning byte-count test  (env:lora_e5_captune)
//
//  Measures the JPEG byte count of a QQVGA 160x120 capture (Strategy C's
//  native size) under grayscale + higher-compression settings, to quantify
//  the "tune the capture instead of MCU-slicing" airtime option.
//
//  Cycles through configs, printing "CFG <label> len=<N>" each frame, and
//  dumps the JPEG hex (JPEG_START/END) so scratchpad/jpegdump.py can save +
//  size-check each variant. Scene-dependent absolute bytes, but the RELATIVE
//  reduction (color->BW, QS sweep) transfers to the real meter.
//
//    * BW grayscale  : OV2640 special effect (U=V=0x80 neutral chroma).
//    * Compression   : QS register 0x44 (DSP bank). Sweeps to find direction
//                      + a usable quality/size knee.
// ============================================================
#ifdef CAPTUNE
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

#define CAM_CS  PA0
ArduCAM myCAM(OV2640, CAM_CS);

struct Variant { const char* label; bool bw; int qs; };  // qs<0 = leave default
static Variant variants[] = {
  {"COLOR_default", false, -1},
  {"BW_default",    true,  -1},
  {"BW_QS04",       true,  0x04},
  {"BW_QS08",       true,  0x08},
  {"BW_QS0C",       true,  0x0c},
  {"BW_QS20",       true,  0x20},
  {"BW_QS40",       true,  0x40},
};
static const int NVAR = sizeof(variants) / sizeof(variants[0]);
static int vi = 0;

static void setBW(bool on) {
  // OV2640 special effect: BW = fixed neutral chroma; Normal = color.
  myCAM.wrSensorReg8_8(0xff, 0x00);
  myCAM.wrSensorReg8_8(0x7c, 0x00);
  myCAM.wrSensorReg8_8(0x7d, on ? 0x18 : 0x00);
  myCAM.wrSensorReg8_8(0x7c, 0x05);
  myCAM.wrSensorReg8_8(0x7d, 0x80);
  myCAM.wrSensorReg8_8(0x7d, 0x80);
}

static void setQS(int qs) {
  if (qs < 0) return;
  myCAM.wrSensorReg8_8(0xff, 0x00);
  myCAM.wrSensorReg8_8(0x44, (uint8_t)qs);   // JPEG quantization scale
}

static const char HEXC[] = "0123456789abcdef";

void setup() {
  Serial.setTx(PB_6); Serial.setRx(PB_7); Serial.begin(115200); delay(600);
  Wire.setSDA(PA_15); Wire.setSCL(PB_15); Wire.begin();
  SPI.setMOSI(PA_10); SPI.setMISO(PB_14); SPI.setSCLK(PB_13); SPI.begin();
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  pinMode(CAM_CS, OUTPUT); digitalWrite(CAM_CS, HIGH);

  myCAM.write_reg(0x07, 0x80); delay(100);
  myCAM.write_reg(0x07, 0x00); delay(100);
  myCAM.wrSensorReg8_8(0xff, 0x01);
  myCAM.set_format(JPEG);
  myCAM.InitCAM();
  myCAM.OV2640_set_JPEG_size(OV2640_160x120);   // Strategy C native QQVGA
  delay(1000);
  Serial.println("\n=== CAPTURE-TUNE byte-count test (QQVGA 160x120) ===");
}

void loop() {
  Variant v = variants[vi];
  setBW(v.bw);
  setQS(v.qs);
  delay(1200);                                  // let AEC + settings settle

  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();
  uint32_t t0 = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK))
    if (millis() - t0 > 3000) { Serial.println("CAP TIMEOUT"); delay(1000); goto next; }

  {
    uint32_t len = myCAM.read_fifo_length();
    Serial.print("CFG "); Serial.print(v.label);
    Serial.print(" len="); Serial.println(len);
    Serial.print("JPEG_START len="); Serial.println(len);
    myCAM.CS_LOW();
    myCAM.set_fifo_burst();
    char line[129]; int col = 0;
    for (uint32_t i = 0; i < len; i++) {
      uint8_t b = SPI.transfer(0x00);
      line[col++] = HEXC[b >> 4];
      line[col++] = HEXC[b & 0x0f];
      if (col >= 128) { line[col] = 0; Serial.print(line); col = 0; }
    }
    if (col) { line[col] = 0; Serial.print(line); }
    myCAM.CS_HIGH();
    Serial.println();
    Serial.println("JPEG_END");
  }
next:
  vi = (vi + 1) % NVAR;
  delay(2500);
}
#endif // CAPTUNE
