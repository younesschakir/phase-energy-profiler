/**
 * ============================================================
 *  Platform C — Seeed Wio-E5 Mini (STM32WLE5) + ArduCAM Mini 2MP Plus
 *  6-Phase energy-measurement cycle for Paper 3.
 *
 *  Phases (each begins with a UART marker to the D1 Mini logger):
 *    P1: System init (wake + radio re-init)
 *    P2: Camera capture (JPEG via SPI ArduChip; I2C OV2640)
 *    P3: Preprocessing (mock reference; real deploy = server-side)
 *    P4: LoRa TX (multi-packet, sized to the real JPEG byte count)
 *    P5: RX window (Class A ACK window)
 *    P6: Sleep entry (radio sleep + MCU STOP2 ~2 uA)
 *
 *  Strategy select (build flag):
 *    -DSTRATEGY_B : full-frame QVGA 320x240 + full JPEG payload (Paper 2 B)
 *    (default)    : QQVGA 160x120 + modeled 160x40 ROI byte count (Strategy C1)
 *
 *  Serial routing:
 *    (default / measurement) markers -> USART1 TX = PA9 -> logger D5
 *    -DDEBUG_SERIAL          text+markers -> PB6/PB7 -> onboard CP2102 (COM9)
 *
 *  Camera pin rule (STM32duino): SPI/Wire setters take PinName (PA_10 ...);
 *    ArduCAM CS + pinMode/digitalWrite take the no-underscore macro PA0 (=22).
 *
 *  Author : Youness Chakir — Chouaib Doukkali University
 * ============================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>
#include <ArduCAM.h>
#include "memorysaver.h"

// ============================================================
//  CAMERA PINS
// ============================================================
#define CAM_CS      PA0        // physical PA0 (no-underscore Arduino macro)
#define SPI_MOSI    PA_10
#define SPI_MISO    PB_14
#define SPI_SCK     PB_13
#define I2C_SDA     PA_15
#define I2C_SCL     PB_15

// ============================================================
//  RADIO CONFIG (EU868, SF7, HP PA 15 dBm)
// ============================================================
#define LORA_FREQ       868.1
#define LORA_BW         125.0
#define LORA_SF         7
#define LORA_CR         5
#define LORA_TX_POWER   15
#define LORA_PREAMBLE   8
#define LORA_MAX_BYTES  222     // max SF7/125kHz payload (EU868)

// ============================================================
//  MEASUREMENT CONFIG
// ============================================================
#define CYCLE_COUNT     30
#ifdef DEBUG_SERIAL
  #define SLEEP_SEC     5       // short sleep so several cycles are watchable
#else
  #define SLEEP_SEC     8       // inter-cycle gap (plain delay): long enough for a
                                // clean segment; keeps the 30-cycle run practical
#endif

// Strategy C1 modeled ROI (160x40 centre crop of 160x120)
#define ROI_FULL_H      120
#define ROI_CROP_H       40

#ifdef STRATEGY_B
  #define JPEG_SIZE     OV2640_320x240
#else
  #define JPEG_SIZE     OV2640_160x120
#endif

// ============================================================
//  PHASE MARKER PROTOCOL (identical to the ESP32-S3 DUT)
// ============================================================
#define MARKER_BAUD     9600
#define MARK_CYCLE_END  0xA0
#define MARK_PHASE(n)   (0xA0 | (n))

static inline void sendMarker(uint8_t b) {
    Serial.write(b);
    Serial.flush();
    delay(2);   // logger SoftwareSerial settling gap
}
static inline void beginPhase(int n) { sendMarker(MARK_PHASE(n)); }

#ifdef DEBUG_SERIAL
  #define DBG(x)    Serial.println(x)
  #define DBGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)    do{}while(0)
  #define DBGF(...) do{}while(0)
#endif

// ============================================================
//  RADIO — STM32WLx built-in SX1262 (RF switch PA4/PA5)
// ============================================================
STM32WLx radio = new STM32WLx_Module();
static const uint32_t rfswitch_pins[] = {
    PA_4, PA_5, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
};
static const Module::RfSwitchMode_t rfswitch_table[] = {
    {STM32WLx::MODE_IDLE,  {LOW,  LOW }},
    {STM32WLx::MODE_RX,    {HIGH, LOW }},
    {STM32WLx::MODE_TX_HP, {LOW,  HIGH}},
    END_OF_MODE_TABLE,
};

// ============================================================
//  CAMERA + RTC
// ============================================================
ArduCAM  myCAM(OV2640, CAM_CS);
STM32RTC& rtc = STM32RTC::getInstance();

// ------------------------------------------------------------
//  Radio (re)initialisation — called each cycle's P1
// ------------------------------------------------------------
static int radioInit() {
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    return radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                       RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                       LORA_TX_POWER, LORA_PREAMBLE);
}

// ------------------------------------------------------------
//  Strategy C-optimized capture path (build flag -DSTRATEGY_C_OPT)
//  Real hardware-achievable ROI capture, vs the modeled 160x40 of the
//  default Strategy C. Applies: (1) hardware 160x120 zoom-crop ROI, (2)
//  grayscale, (3) JPEG compression, and (4) TX only to the JPEG EOI
//  (see phaseCapture). Gives the HONEST bytes the deployed node sends.
// ------------------------------------------------------------
#ifdef STRATEGY_C_OPT
#ifndef JPEG_QS
#define JPEG_QS 0x20    // OV2640 quantization scale (0x44): higher = more
                        // compression. Tune to the readability knee on a
                        // real meter (raise until OCR accuracy just drops).
#endif
// Hardware 160x120 zoom-crop ROI: crops a centered 200x150 (4:3) sub-region
// of the CIF 400x296 ISP and zooms to 160x120 (proven in windowtest.cpp).
// The crop centre (regs 0x53/0x54) can be shifted to aim at the digit row.
static const uint8_t cfgCIF[][2] = {
  {0xff,0x01},{0x12,0x20},{0x03,0x0a},{0x32,0x89},
  {0x17,0x11},{0x18,0x43},{0x19,0x00},{0x1a,0x25},
  {0x4f,0xca},{0x50,0xa8},{0x5a,0x23},{0x6d,0x00},{0x3d,0x38},{0x39,0x92},
  {0x35,0xda},{0x22,0x1a},{0x37,0xc3},{0x23,0x00},
  {0x34,0xc0},{0x06,0x88},{0x07,0xc0},{0x0d,0x87},{0x0e,0x41},{0x4c,0x00},
  {0xff,0x00},{0xe0,0x04},{0xc0,0x32},{0xc1,0x25},{0x8c,0x00},{0x86,0x3d},
  {0x50,0x80},{0x51,0x32},{0x52,0x25},{0x53,0x19},{0x54,0x12},{0x55,0x10},
  {0x57,0x00},{0x5a,0x28},{0x5b,0x1e},{0x5c,0x00},{0xe0,0x00},
  {0xff,0x01},{0x04,0x08},{0xff,0xff},
};
static void applyTable(const uint8_t t[][2]) {
    for (size_t i = 0; t[i][0] != 0xff || t[i][1] != 0xff; i++)
        myCAM.wrSensorReg8_8(t[i][0], t[i][1]);
}
static void setGrayscale() {   // BW special effect = neutral chroma
    myCAM.wrSensorReg8_8(0xff,0x00); myCAM.wrSensorReg8_8(0x7c,0x00);
    myCAM.wrSensorReg8_8(0x7d,0x18); myCAM.wrSensorReg8_8(0x7c,0x05);
    myCAM.wrSensorReg8_8(0x7d,0x80); myCAM.wrSensorReg8_8(0x7d,0x80);
}
static void setQS(uint8_t qs) {
    myCAM.wrSensorReg8_8(0xff,0x00); myCAM.wrSensorReg8_8(0x44, qs);
}
#endif // STRATEGY_C_OPT

// ------------------------------------------------------------
//  Camera init (once, in setup)
// ------------------------------------------------------------
static bool cameraInit() {
    Wire.setSDA(I2C_SDA);
    Wire.setSCL(I2C_SCL);
    Wire.begin();
    SPI.setMOSI(SPI_MOSI);
    SPI.setMISO(SPI_MISO);
    SPI.setSCLK(SPI_SCK);
    SPI.begin();
    pinMode(CAM_CS, OUTPUT);
    digitalWrite(CAM_CS, HIGH);

    // CPLD reset then SPI bus check
    myCAM.write_reg(0x07, 0x80); delay(100);
    myCAM.write_reg(0x07, 0x00); delay(100);
    myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
    if (myCAM.read_reg(ARDUCHIP_TEST1) != 0x55) return false;

    // OV2640 identity
    uint8_t vid = 0, pid = 0;
    myCAM.wrSensorReg8_8(0xff, 0x01);
    myCAM.rdSensorReg8_8(OV2640_CHIPID_HIGH, &vid);
    myCAM.rdSensorReg8_8(OV2640_CHIPID_LOW,  &pid);
    if (!(vid == 0x26 && (pid == 0x41 || pid == 0x42))) return false;

    myCAM.set_format(JPEG);
    myCAM.InitCAM();
#ifdef STRATEGY_C_OPT
    applyTable(cfgCIF);                 // (1) hardware 160x120 zoom-crop ROI
    setGrayscale();                    // (2) grayscale
    setQS(JPEG_QS);                    // (3) JPEG compression
#else
    myCAM.OV2640_set_JPEG_size(JPEG_SIZE);
#endif
    delay(1000);
    myCAM.clear_fifo_flag();
    return true;
}

// ============================================================
//  P2: CAMERA CAPTURE -> returns effective payload byte count
// ============================================================
uint32_t phaseCapture() {
    beginPhase(2);
    myCAM.flush_fifo();
    myCAM.clear_fifo_flag();
    myCAM.start_capture();
    uint32_t t0 = millis();
    while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
        if (millis() - t0 > 3000) { DBG("  capture TIMEOUT"); break; }
    }
    uint32_t fullLen = myCAM.read_fifo_length();
    uint32_t txBytes;

#if defined(STRATEGY_C_OPT)
    // Read the image out and stop at the JPEG EOI (FF D9): read_fifo_length is
    // padded to a 512-byte page, so the padding bytes past EOI are junk. This
    // yields the REAL byte count the deployed node transmits (EOI-strip) and
    // realistically includes the FIFO readout in P2.
    myCAM.CS_LOW();
    myCAM.set_fifo_burst();
    uint32_t trueLen = fullLen;
    uint8_t prev = 0;
    for (uint32_t i = 0; i < fullLen; i++) {
        uint8_t b = SPI.transfer(0x00);
        if (prev == 0xFF && b == 0xD9) { trueLen = i + 1; break; }
        prev = b;
    }
    myCAM.CS_HIGH();
    txBytes = trueLen;                                            // EOI-stripped real bytes
#elif defined(STRATEGY_B)
    txBytes = fullLen;                                            // full JPEG
#else
    txBytes = (uint32_t)(((uint64_t)fullLen * ROI_CROP_H) / ROI_FULL_H);  // modeled 160x40
#endif
    if (txBytes < 1) txBytes = 1;
    DBGF("  P2 capture: fullLen=%lu txBytes=%lu\n",
         (unsigned long)fullLen, (unsigned long)txBytes);
    return txBytes;
}

// ============================================================
//  P3: PREPROCESSING (mock reference load)
// ============================================================
void phasePreprocess(uint32_t imgBytes) {
    beginPhase(3);
    uint32_t n = imgBytes / 4;
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) acc += i;
    (void)acc;
}

// ============================================================
//  P4: LORA TX — multi-packet, sized to the JPEG byte count.
//  Content is synthetic fill (energy depends on byte/packet count,
//  not payload contents — matches Paper 2's size-driven model).
// ============================================================
int phaseLoraTx(uint8_t cycle, uint32_t imgBytes) {
    beginPhase(4);
    uint32_t numPackets = (imgBytes + LORA_MAX_BYTES - 1) / LORA_MAX_BYTES;
    if (numPackets < 1) numPackets = 1;

    int lastState = RADIOLIB_ERR_NONE;
    for (uint32_t pkt = 0; pkt < numPackets; pkt++) {
        uint32_t chunk = imgBytes - pkt * LORA_MAX_BYTES;
        if (chunk > LORA_MAX_BYTES) chunk = LORA_MAX_BYTES;

        uint8_t payload[LORA_MAX_BYTES];
        memset(payload, 0xAA, (size_t)chunk);
        payload[0] = cycle;
        payload[1] = (uint8_t)pkt;
        payload[2] = (uint8_t)numPackets;
        payload[3] = (uint8_t)((imgBytes >> 8) & 0xFF);
        payload[4] = (uint8_t)( imgBytes       & 0xFF);

        lastState = radio.transmit(payload, (size_t)chunk);
        delay(10);   // inter-packet gap
    }
    DBGF("  P4 TX: %lu packets, last state=%d\n",
         (unsigned long)numPackets, lastState);
    return lastState;
}

// ============================================================
//  P5: RX WINDOW (Class A ACK wait)
// ============================================================
void phaseRxWindow() {
    beginPhase(5);
    uint8_t rxBuf[32];
    int st = radio.receive(rxBuf, sizeof(rxBuf));   // ERR_RX_TIMEOUT expected
    (void)st;
}

// ============================================================
//  P6: SLEEP ENTRY + STOP2 DEEP SLEEP
//  NOTE: the classic ArduCAM Mini has no MCU power-gate API; the
//  OV2640/ArduChip idle current is therefore part of the measured
//  sleep floor (an honest as-built limitation, like Platform A).
// ============================================================
void phaseSleep() {
    beginPhase(6);
    radio.sleep(false);              // warm sleep (keep config)
    sendMarker(MARK_CYCLE_END);
    delay(5);
    Serial.flush();
    // Inter-cycle gap: plain delay (NOT STOP2 — see setup() note). The MCU
    // stays awake (~camera-floored current); true sleep is characterised
    // separately. Exact timing, radio survives.
    delay(SLEEP_SEC * 1000);
}

// ============================================================
//  SETUP = one full measurement run (CYCLE_COUNT cycles)
// ============================================================
void setup() {
#ifdef DEBUG_SERIAL
    Serial.setTx(PB_6);            // CP2102 (COM9) for text verification
    Serial.setRx(PB_7);
#else
    Serial.setTx(PA_9);            // markers -> logger D5
    Serial.setRx(PB_7);
#endif
    Serial.begin(MARKER_BAUD);
    delay(300);
    DBG("\n=== E5 STRATEGY MEASUREMENT (debug) ===");

    // NOTE: STOP2 deep-sleep is NOT used in the loop. On the STM32WL, entering
    // STOP2 wipes the SX1262 radio (post-wake SPI = RADIOLIB_ERR_SPI_CMD_FAILED),
    // and forcing RTC->LSI (needed for correct STOP2 timing) also breaks the
    // radio. Since the sleep floor is camera-limited (~137mA) anyway and the true
    // MCU STOP2 current (uA) is characterised separately, the inter-cycle gap uses
    // a plain delay() (radio stays alive, exact timing). See phaseSleep().
    rtc.begin();
    LowPower.begin();

    if (!cameraInit()) {
        DBG("CAMERA INIT FAILED");
#ifdef DEBUG_SERIAL
        while (true) { Serial.println("cam fail"); delay(1000); }
#endif
    } else {
        DBG("camera OK");
    }

    int st = radioInit();
    DBGF("radio.begin state=%d\n", st);
    if (st != RADIOLIB_ERR_NONE) {
        DBG("RADIO INIT FAILED");
#ifdef DEBUG_SERIAL
        while (true) { Serial.println("radio fail"); delay(1000); }
#endif
    }

    for (uint8_t i = 1; i <= CYCLE_COUNT; i++) {
        DBGF("--- cycle %u ---\n", i);
        beginPhase(1);                          // P1 (radio re-init after wake)
        if (i > 1) radioInit();

        uint32_t imgSize = phaseCapture();      // P2
        phasePreprocess(imgSize);               // P3
        phaseLoraTx(i, imgSize);                // P4
        phaseRxWindow();                        // P5
        phaseSleep();                           // P6 (+ STOP2 sleep)
    }

    sendMarker(MARK_CYCLE_END);
    radio.sleep(true);
    while (true) delay(1000);   // run complete (STOP2 unused — see setup note)
}

void loop() {}
