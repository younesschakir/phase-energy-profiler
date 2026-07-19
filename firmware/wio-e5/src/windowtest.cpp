// ============================================================
//  OV2640 hardware windowing test  (env:lora_e5_windowtest)
//
//  RESULT (2026-07-11): true hardware windowed cropping WORKS on the classic
//  ArduCAM Mini 2MP Plus / E5 -- but with a hard DSP limit.
//
//    * The JPEG DSP has a MINIMUM output of 160x120 (QQVGA). Any output
//      smaller than ~120 rows, or a strongly non-4:3 aspect, is REJECTED and
//      the sensor silently reverts to QVGA 320x240 (the InitCAM default).
//      This is why no OV2640 driver ships a sub-QQVGA JPEG size.
//    * WITHIN that limit, real optical cropping works: this config crops a
//      centered 200x150 (4:3) sub-region of the CIF 400x296 ISP frame and
//      zooms it to a genuine, well-exposed 160x120 (a true "zoom into the
//      meter" ROI -- verified as a distinct, more-detailed image, not a
//      fallback).
//    * => A native 160x40 digit-strip JPEG is NOT achievable on this sensor
//      path. For a tight strip: capture the 160x120 hw ROI and slice the
//      band in the MCU, or use modeled byte-scaling (paper's Strategy C).
//
//  How the boundary was found (all via jpegdump.py SOF check):
//    160x120 / 176x144 / 320x240 full-frame  -> OK (>=120 rows, ~4:3)
//    200x150 crop -> 160x120                  -> OK (windowing proven)
//    any 160x{40,80,96} or 80x60              -> 320x240 fallback (<120 rows)
//
//  Mechanism notes:
//    * Size changes only latch when applied inside the table's
//      0xe0=0x04 (RESET_DVP) .. 0xe0=0x00 (release) bracket. Standard
//      ArduCAM size tables and this CIF table both do that.
//    * CIF datapath: ISP 400x296, CTRLI=0x80 (no divider), 2.5x DCW zoom.
//      set_window regs are the esp32-camera formula (all dims /4).
//
//  Dumps the JPEG as hex over CP2102@115200 (JPEG_START/JPEG_END) for
//  reconstruction + SOF dimension check via scratchpad/jpegdump.py.
//  (cfg[]/ovr[]/applyOverrides + patchVertical are the SVGA-base iteration
//   harness kept for future probing; the live path uses cfgCIF via applyTable.)
// ============================================================
#ifdef WINDOW_TEST
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

#define CAM_CS  PA0
ArduCAM myCAM(OV2640, CAM_CS);

// ---- desired output height of the band (width fixed at 160) ----
static const int OUT_H = 40;
// vertical band position as fraction of the 600-row ISP frame free travel
// (0.0=top, 0.5=center, 1.0=bottom). Tune to the meter digit row.
static const float BAND_V = 0.5f;

// ArduCAM OV2640_160x120_JPEG (proven 160x120) copied to RAM so the vertical
// crop regs can be patched. {0xff,0xff} terminates. Bank markers {0xff,0x01}
// = SENSOR, {0xff,0x00} = DSP.
static uint8_t cfg[][2] = {
  {0xff,0x01},{0x12,0x40},{0x17,0x11},{0x18,0x43},{0x19,0x00},{0x1a,0x4b},
  {0x32,0x09},{0x4f,0xca},{0x50,0xa8},{0x5a,0x23},{0x6d,0x00},{0x39,0x12},
  {0x35,0xda},{0x22,0x1a},{0x37,0xc3},{0x23,0x00},{0x34,0xc0},{0x36,0x1a},
  {0x06,0x88},{0x07,0xc0},{0x0d,0x87},{0x0e,0x41},{0x4c,0x00},
  {0xff,0x00},{0xe0,0x04},{0xc0,0x64},{0xc1,0x4b},{0x86,0x35},{0x50,0x92},
  {0x51,0xc8},{0x52,0x96},{0x53,0x00},{0x54,0x00},{0x55,0x00},{0x57,0x00},
  {0x5a,0x28},{0x5b,0x1e},{0x5c,0x00},{0xe0,0x00},{0xff,0xff},
};

// esp32-camera CIF datapath as a complete table (ISP 400x296, CTRLI=0x80,
// pure 2.5x DCW). Crop a 400x100 (4:1) band of the CIF ISP, centered, and
// zoom to 160x40. Applied via applyTable() within one 0xe0=0x04..0x00
// bracket, then JPEG re-asserted.
//   win (/4 units): HSIZE=100 VSIZE=25 off_y=24 -> ZMOW=40 ZMOH=10
static uint8_t cfgCIF[][2] = {
  {0xff,0x01},              // SENSOR bank
  {0x12,0x20},              // COM7 = CIF
  {0x03,0x0a},              // COM1
  {0x32,0x89},              // REG32 = CIF
  {0x17,0x11},{0x18,0x43},{0x19,0x00},{0x1a,0x25},
  {0x4f,0xca},{0x50,0xa8},  // BD50 / BD60  (0x50 = sensor-bank BD60 here)
  {0x5a,0x23},{0x6d,0x00},{0x3d,0x38},{0x39,0x92},
  {0x35,0xda},{0x22,0x1a},{0x37,0xc3},{0x23,0x00},
  {0x34,0xc0},{0x06,0x88},{0x07,0xc0},{0x0d,0x87},{0x0e,0x41},{0x4c,0x00},
  {0xff,0x00},              // DSP bank
  {0xe0,0x04},              // RESET_DVP  (bracket open)
  {0xc0,0x32},{0xc1,0x25},  // HSIZE8 / VSIZE8  (CIF ISP 400x296)
  {0x8c,0x00},              // SIZEL
  {0x86,0x3d},              // CTRL2 = DCW_EN|0x1d
  {0x50,0x80},              // CTRLI = LP_DP (no divider)
  {0x51,0x32},{0x52,0x25},  // HSIZE=50(200) VSIZE=37(150) -- 4:3 sub-region crop
  {0x53,0x19},{0x54,0x12},  // XOFFL=25(100) YOFFL=18(72) center zoom into meter
  {0x55,0x10},{0x57,0x00},  // VHYX / TEST
  {0x5a,0x28},{0x5b,0x1e},{0x5c,0x00},   // ZMOW=40(160) ZMOH=30(120) -> real 160x120 hw ROI
  {0xe0,0x00},              // bracket close
  {0xff,0x01},{0x04,0x08},  // (JPEG mirror flag, per ArduCAM tables)
  {0xff,0xff},
};

// SENSOR-ARRAY windowing test (the "online post" method): crop the physical
// pixel readout via Bank-1 HSTART/HSTOP(0x17/0x18) + VSTART/VSTOP(0x19/0x1a)
// to a ~160x40 patch of the 1600x1200 array, then DSP passthrough ~1:1 (no
// down-scale, CTRLI=0). Question: does this bypass the DSP's 120-row floor?
// Based on esp32-camera to_uxga; only the start/stop regs overridden (HREF
// 0x32 / VREF 0x03 left at base to avoid the post's unreliable overflow math).
static uint8_t cfgSENSOR[][2] = {
  {0xff,0x01},              // SENSOR bank
  {0x12,0x00},              // COM7 = UXGA
  {0x03,0x0f},              // COM1 (VREF base)
  {0x32,0x36},              // REG32 (HREF base)
  {0x17,0x3e},{0x18,0x48},  // HSTART/HSTOP -> ~160px wide readout, centered
  {0x19,0x49},{0x1a,0x4e},  // VSTART/VSTOP -> ~40-line readout, centered
  {0x3d,0x34},{0x4f,0xbb},{0x50,0x9c},{0x5a,0x57},{0x6d,0x80},{0x39,0x82},
  {0x23,0x00},{0x07,0xc0},{0x4c,0x00},{0x35,0x88},{0x22,0x0a},{0x37,0x40},
  {0x34,0xa0},{0x06,0x02},{0x0d,0xb7},{0x0e,0x01},{0x42,0x83},
  {0xff,0x00},              // DSP bank
  {0xe0,0x04},              // RESET_DVP  (bracket open)
  {0xc0,0x14},{0xc1,0x05},  // HSIZE8/VSIZE8 = 160x40 (input to DSP)
  {0x8c,0x00},              // SIZEL
  {0x86,0x3d},              // CTRL2 = DCW_EN|0x1d
  {0x50,0x00},              // CTRLI = 0 (no scaling)
  {0x51,0x28},{0x52,0x0a},  // HSIZE/VSIZE = 160x40
  {0x53,0x00},{0x54,0x00},{0x55,0x00},{0x57,0x00},
  {0x5a,0x28},{0x5b,0x0a},{0x5c,0x00},   // ZMOW/ZMOH = 160x40 (1:1 passthrough)
  {0xe0,0x00},              // bracket close
  {0xff,0x01},{0x04,0x08},
  {0xff,0xff},
};

// DSP-bank register overrides applied in-place to the proven 160x120 table
// (so they land inside the 0xe0=0x04 .. 0xe0=0x00 latch bracket). Edit this
// list to iterate. {0,0} terminates.  Currently: SQUISH test -- change only
// ZMOH so the full 800x600 FOV scales to 160x40 (no crop, no divider change).
static uint8_t ovr[][2] = {
  // CONTROL TEST: reconstruct the known-good 176x144 table via override
  // (160x120 base + ZMOW/ZMOH of 176x144). MUST yield 176x144 if the
  // override harness is sound.
  {0x5a, 0x2c},   // ZMOW -> 176
  {0x5b, 0x24},   // ZMOH -> 144
  {0, 0},
};

static void applyOverrides() {
  for (size_t k = 0; ovr[k][0] != 0 || ovr[k][1] != 0; k++) {
    uint8_t reg = ovr[k][0], val = ovr[k][1];
    int bank = -1;
    for (size_t i = 0; cfg[i][0] != 0xff || cfg[i][1] != 0xff; i++) {
      if (cfg[i][0] == 0xff) { bank = cfg[i][1]; continue; }  // track bank
      if (bank == 0x00 && cfg[i][0] == reg) cfg[i][1] = val;  // DSP bank only
    }
    Serial.print("DIAG ovr 0x"); Serial.print(reg, HEX);
    Serial.print("=0x"); Serial.println(val, HEX);
  }
}

static void patchVertical(int outH, float bandV) {
  // ISP frame is 800x600. DSP input window (esp32-camera units: all /4).
  const int MAX_X = 800 / 4;                 // 200  (full width, unchanged)
  int inH   = 600 * outH / 120;              // 200 rows for outH=40 (band height in ISP px)
  int MAX_Y = inH / 4;                       // 50
  int OFF_Y = (int)((600 - inH) * bandV) / 4;// centered band, /4
  int W     = 160 / 4;                       // 40 (output width/4, unchanged)
  int H     = outH / 4;                      // 10 for 40
  const int OFF_X = 0;
  for (size_t i = 0; cfg[i][0] != 0xff || cfg[i][1] != 0xff; i++) {
    if (cfg[i][0] == 0xff) continue;         // bank marker (only patch DSP-bank regs below)
    switch (cfg[i][0]) {
      case 0x52: cfg[i][1] = MAX_Y & 0xff; break;                                   // VSIZE
      case 0x54: cfg[i][1] = OFF_Y & 0xff; break;                                   // YOFFL
      case 0x55: cfg[i][1] = ((MAX_Y>>1)&0x80)|((OFF_Y>>4)&0x70)|
                             ((MAX_X>>5)&0x08)|((OFF_X>>8)&0x07); break;            // VHYX
      case 0x5b: cfg[i][1] = H & 0xff; break;                                        // ZMOH
      case 0x5c: cfg[i][1] = ((H>>6)&0x04)|((W>>8)&0x03); break;                     // ZMHH
    }
  }
  Serial.print("DIAG band inH="); Serial.print(inH);
  Serial.print(" offY="); Serial.print(OFF_Y*4);
  Serial.print(" out=160x"); Serial.println(outH);
}

static void applyCfg() {
  for (size_t i = 0; cfg[i][0] != 0xff || cfg[i][1] != 0xff; i++)
    myCAM.wrSensorReg8_8(cfg[i][0], cfg[i][1]);
}

static void applyTable(uint8_t t[][2]) {
  for (size_t i = 0; t[i][0] != 0xff || t[i][1] != 0xff; i++)
    myCAM.wrSensorReg8_8(t[i][0], t[i][1]);
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
  myCAM.InitCAM();                       // full JPEG init (proven on E5)

  // --- true hardware ROI: 4:3 zoom crop (200x150 sub-region -> 160x120) ---
  // NB the classic-ArduCAM OV2640 JPEG DSP floor is 160x120 (QQVGA); smaller
  // or non-~4:3 outputs revert to QVGA 320x240. See file header.
  // Live path = the WORKING hardware ROI (4:3 zoom crop -> 160x120).
  // cfgSENSOR (the online post's sensor-array-window method) was tested and
  // does NOT bypass the 120-row output floor -> kept only as a negative ref.
  applyTable(cfgCIF);
  Serial.println("DIAG applied cfgCIF (4:3 zoom crop -> 160x120 hw ROI)");

  delay(2000);                           // let AEC/AGC settle for the new window
  myCAM.clear_fifo_flag();
  Serial.println("\n=== WINDOW TEST: 4:3 hw zoom-crop -> 160x120 ===");
}

void loop() {
  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();
  uint32_t t0 = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK))
    if (millis() - t0 > 3000) { Serial.println("CAP TIMEOUT"); delay(3000); return; }

  uint32_t len = myCAM.read_fifo_length();
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
  delay(6000);
}
#endif // WINDOW_TEST
