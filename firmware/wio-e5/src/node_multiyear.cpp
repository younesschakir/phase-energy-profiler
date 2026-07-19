/**
 * ============================================================
 *  MULTI-YEAR NODE — Wio-E5 (STM32WLE5) + ArduCAM Mini 2MP Plus
 *  The deployable architecture: every proven lever combined.
 *
 *    * Camera POWER-GATE (FQP30N06L low-side on camera GND):
 *        gate ON -> camera re-init -> capture -> pins safe -> gate OFF
 *        kills the ~137 mA un-gateable OV2640 sleep floor.
 *    * Optimized capture (from stratC_opt): hardware 160x120 zoom-crop
 *        ROI + grayscale + JPEG compression (QS) + TX-to-EOI padding strip.
 *    * STOP2 + WARM radio sleep (proven in sleeptest5, ~60 uA board-level):
 *        radio.sleep(true) retains SX1262 config across STOP2;
 *        post-wake radio.standby() recovers it (re-begin fallback).
 *
 *  Cycle (markers logger-compatible, same P1-P6 protocol as main.cpp):
 *    P1 wake + radio recover      (standby / re-begin)
 *    P2 gate ON + camera re-init + capture + EOI read + gate OFF
 *       (P2 deliberately contains the camera bring-up: that IS the real
 *        per-reading camera cost of a gated node)
 *    P3 preprocess (reference load)
 *    P4 LoRa TX (packets sized to the EOI-stripped true byte count)
 *    P5 RX window
 *    P6 sleep entry -> radio.sleep(true) -> STOP2 deepSleep(READ_INTERVAL)
 *
 *  MOSFET wiring (FQP30N06L, low-side):
 *    Camera GND wire -> Drain;  Source -> real GND;
 *    Gate -> CAM_GATE_PIN (PB9) with 10 kOhm Gate->GND pulldown
 *    (pulldown keeps the camera OFF during boot/reset/STOP2).
 *    Camera VCC -> 3V3 as before.
 *
 *  Build envs:
 *    lora_e5_node        markers -> PA9 (measurement, INA rig)
 *    lora_e5_node_debug  text -> PB6/PB7 CP2102 COM9 @9600
 *    lora_e5_gatetest    MOSFET wiring check: toggles gate every 4 s,
 *                        no camera init — watch current step ~57 mA.
 *
 *  Author : Youness Chakir — Chouaib Doukkali University
 * ============================================================
 */
#ifdef NODE_MULTIYEAR
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>
#include <SubGhz.h>
#include <ArduCAM.h>
#include "memorysaver.h"

// ============================================================
//  PINS  (STM32duino rule: SPI/Wire setters take PinName PA_10;
//  pinMode/digitalWrite/ArduCAM CS take the no-underscore macro)
// ============================================================
#define CAM_CS        PA0        // ArduChip chip-select
#define CAM_GATE_PIN  PB9        // MOSFET gate (free since CS moved to PA0)
#define SPI_MOSI      PA_10
#define SPI_MISO      PB_14
#define SPI_SCK       PB_13
#define I2C_SDA       PA_15
#define I2C_SCL       PB_15
// no-underscore twins for pinMode/digitalWrite when parking the bus
#define P_MOSI  PA10
#define P_MISO  PB14
#define P_SCK   PB13
#define P_SDA   PA15
#define P_SCL   PB15

// ============================================================
//  RADIO (EU868 SF7, HP PA 15 dBm) + TIMING
// ============================================================
#define LORA_FREQ       868.1
#define LORA_BW         125.0
#define LORA_SF         7
#define LORA_CR         5
#define LORA_TX_POWER   15
#define LORA_PREAMBLE   8
#define LORA_MAX_BYTES  222

#ifdef DEBUG_SERIAL
  #define READ_INTERVAL_SEC  15      // watchable cycles while debugging
#else
  #define READ_INTERVAL_SEC  30      // bench/rig value; deployment: 3600 (hourly)
#endif
#define CAM_POWERUP_MS   120         // camera rail settle after gate ON
#define AEC_SETTLE_MS   1000         // auto-exposure settle before capture

#ifndef JPEG_QS
#define JPEG_QS 0x20                 // compression knob; tune on the real meter
#endif

// ============================================================
//  MARKERS (identical protocol to main.cpp -> capture_run.py works)
// ============================================================
#define MARKER_BAUD     9600
#define MARK_CYCLE_END  0xA0
#define MARK_PHASE(n)   (0xA0 | (n))
static inline void sendMarker(uint8_t b) { Serial.write(b); Serial.flush(); delay(2); }
static inline void beginPhase(int n)     { sendMarker(MARK_PHASE(n)); }

#ifdef DEBUG_SERIAL
  #define DBG(x)    Serial.println(x)
  #define DBGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)    do{}while(0)
  #define DBGF(...) do{}while(0)
#endif

// ============================================================
//  RADIO
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
STM32RTC& rtc = STM32RTC::getInstance();
ArduCAM myCAM(OV2640, CAM_CS);

static int radioStart() {
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    return radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                       RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                       LORA_TX_POWER, LORA_PREAMBLE);
}

// ============================================================
//  OPTIMIZED CAPTURE CONFIG (proven in windowtest/stratC_opt)
//  Hardware 160x120 zoom-crop: centered 200x150 (4:3) sub-region of the
//  CIF 400x296 ISP zoomed to 160x120. Shift 0x53/0x54 to aim at the meter.
// ============================================================
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

// ============================================================
//  CAMERA POWER GATE
// ============================================================
static void gateOn() {
    digitalWrite(CAM_GATE_PIN, HIGH);         // N-FET conducts -> camera GND connected
    delay(CAM_POWERUP_MS);
}

// Float every camera-facing pin to HIGH-IMPEDANCE before cutting the gate.
// The camera VCC stays wired (low-side gate cuts only GND), so if these pins
// were driven LOW the camera would phantom-power itself THROUGH them into the
// E5 ground (~30 mA measured). Hi-Z removes that return path -> the isolated
// camera VCC can't sink current.  (INPUT_ANALOG = no Schmitt/pull = lowest.)
static void gateOff() {
    SPI.end();
    Wire.end();
    const int pins[] = {P_MOSI, P_MISO, P_SCK, P_SDA, P_SCL, CAM_CS};
    for (unsigned i = 0; i < sizeof(pins)/sizeof(pins[0]); i++)
        pinMode(pins[i], INPUT_ANALOG);       // Hi-Z, not driven LOW
    digitalWrite(CAM_GATE_PIN, LOW);          // camera GND cut
}

// Full camera bring-up after gate ON (the camera lost ALL state).
static bool cameraUp() {
    Wire.setSDA(I2C_SDA);  Wire.setSCL(I2C_SCL);  Wire.begin();
    SPI.setMOSI(SPI_MOSI); SPI.setMISO(SPI_MISO); SPI.setSCLK(SPI_SCK);
    SPI.begin();
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    pinMode(CAM_CS, OUTPUT); digitalWrite(CAM_CS, HIGH);

    myCAM.write_reg(0x07, 0x80); delay(100);  // CPLD reset
    myCAM.write_reg(0x07, 0x00); delay(100);
    myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
    if (myCAM.read_reg(ARDUCHIP_TEST1) != 0x55) return false;

    myCAM.wrSensorReg8_8(0xff, 0x01);
    myCAM.set_format(JPEG);
    myCAM.InitCAM();
#ifdef FULLFRAME
    // Strategy B (gated): full-frame QVGA 320x240 color JPEG, no ROI/gray/QS.
    myCAM.OV2640_set_JPEG_size(OV2640_320x240);
#else
    // optimized capture: hw ROI + grayscale + compression
    for (size_t i = 0; cfgCIF[i][0] != 0xff || cfgCIF[i][1] != 0xff; i++)
        myCAM.wrSensorReg8_8(cfgCIF[i][0], cfgCIF[i][1]);
    myCAM.wrSensorReg8_8(0xff, 0x00);         // grayscale (BW effect)
    myCAM.wrSensorReg8_8(0x7c, 0x00); myCAM.wrSensorReg8_8(0x7d, 0x18);
    myCAM.wrSensorReg8_8(0x7c, 0x05); myCAM.wrSensorReg8_8(0x7d, 0x80);
    myCAM.wrSensorReg8_8(0x7d, 0x80);
    myCAM.wrSensorReg8_8(0xff, 0x00);         // compression
    myCAM.wrSensorReg8_8(0x44, JPEG_QS);
#endif
    delay(AEC_SETTLE_MS);
    myCAM.clear_fifo_flag();
    return true;
}

// ============================================================
//  P2: gated capture -> EOI-stripped true byte count
// ============================================================
static uint32_t phaseCapture() {
    beginPhase(2);
    gateOn();
    if (!cameraUp()) { DBG("  camera FAIL"); gateOff(); return 1; }

    myCAM.flush_fifo();
    myCAM.clear_fifo_flag();
    myCAM.start_capture();
    uint32_t t0 = millis();
    while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK))
        if (millis() - t0 > 3000) { DBG("  capture TIMEOUT"); break; }

    uint32_t fullLen = myCAM.read_fifo_length();
    // read out, stop at JPEG EOI (FIFO length is 512-byte-page padded)
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

    gateOff();                                 // camera dark until next reading
    DBGF("  P2: fullLen=%lu trueLen=%lu\n",
         (unsigned long)fullLen, (unsigned long)trueLen);
    return trueLen < 1 ? 1 : trueLen;
}

// ============================================================
//  P3-P5 (same semantics as main.cpp)
// ============================================================
static void phasePreprocess(uint32_t n) {
    beginPhase(3);
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < n / 4; i++) acc += i;
    (void)acc;
}

static int phaseLoraTx(uint8_t cycle, uint32_t imgBytes) {
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
        delay(10);
    }
    DBGF("  P4: %lu pkts, state=%d\n", (unsigned long)numPackets, lastState);
    return lastState;
}

static void phaseRxWindow() {
    beginPhase(5);
    uint8_t rxBuf[32];
    (void)radio.receive(rxBuf, sizeof(rxBuf));   // ERR_RX_TIMEOUT expected
}

// ============================================================
//  P6: warm radio sleep + STOP2  (proven sequence, ~60 uA board-level)
// ============================================================
static void phaseSleep() {
    beginPhase(6);
    radio.clearDio1Action();           // detach level-IRQ (prevents wake storm)
    SubGhz.clearPendingInterrupt();
    radio.sleep(true);                 // WARM: SX1262 config retained
    sendMarker(MARK_CYCLE_END);
    delay(5);
    Serial.flush();
    LowPower.deepSleep((uint32_t)READ_INTERVAL_SEC * 1000);
    // Re-init the marker UART: STOP2 powers down USART1, so without this the
    // post-wake phase markers never reach the logger (phase column stuck at 0).
#ifdef DEBUG_SERIAL
    Serial.setTx(PB_6);
#else
    Serial.setTx(PA_9);
#endif
    Serial.begin(MARKER_BAUD);
    // wake: recover radio from warm sleep
    if (radio.standby() != RADIOLIB_ERR_NONE) radioStart();
    SubGhz.clearPendingInterrupt();
}

// ============================================================
//  GATE_TEST: MOSFET wiring check only — no camera init, no radio.
//  Watch the supply current: gate ON adds the camera (~57 mA uninit);
//  gate OFF must remove it completely.
// ============================================================
#ifdef GATE_TEST
void setup() {
#ifdef DEBUG_SERIAL
    Serial.setTx(PB_6); Serial.setRx(PB_7);
#else
    Serial.setTx(PA_9); Serial.setRx(PB_7);
#endif
    Serial.begin(MARKER_BAUD); delay(300);
    pinMode(CAM_GATE_PIN, OUTPUT);
    digitalWrite(CAM_GATE_PIN, LOW);
    DBG("\n=== GATE TEST: 4s OFF / 4s ON ===");
}
void loop() {
    DBG("gate OFF"); digitalWrite(CAM_GATE_PIN, LOW);  delay(4000);
    DBG("gate ON");  digitalWrite(CAM_GATE_PIN, HIGH); delay(4000);
}
#else

// ============================================================
//  NODE MAIN
// ============================================================
static uint8_t cycleNum = 0;

void setup() {
#ifdef DEBUG_SERIAL
    Serial.setTx(PB_6); Serial.setRx(PB_7);    // CP2102 COM9
#else
    Serial.setTx(PA_9); Serial.setRx(PB_7);    // markers -> logger D5
#endif
    Serial.begin(MARKER_BAUD);
    delay(300);
    DBG("\n=== MULTI-YEAR NODE (gate + stratC_opt + STOP2) ===");

    pinMode(CAM_GATE_PIN, OUTPUT);
    digitalWrite(CAM_GATE_PIN, LOW);           // camera OFF until needed

    // Kill the always-on red LED (3V3 -> R13(1k) -> D6 -> physical PB5): drive
    // PB5 HIGH so there's no voltage across the LED (~1.5 mA saved). Use HAL
    // directly on physical PB5 -- the Arduino `PB5` macro maps to the wrong pin
    // in this variant (same trap as PA_0 vs PA0).
    __HAL_RCC_GPIOB_CLK_ENABLE();
    {
        GPIO_InitTypeDef g = {0};
        g.Pin   = GPIO_PIN_5;
        g.Mode  = GPIO_MODE_OUTPUT_PP;
        g.Pull  = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOB, &g);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);   // PB5 = 3V3 -> LED off
    }
    // Camera-facing pins to Hi-Z at boot (camera GND is gated off; Hi-Z avoids
    // both the phantom-sink AND the back-feed — see gateOff()).
    {
        const int pins[] = {P_MOSI, P_MISO, P_SCK, P_SDA, P_SCL, CAM_CS};
        for (unsigned i = 0; i < sizeof(pins)/sizeof(pins[0]); i++)
            pinMode(pins[i], INPUT_ANALOG);
    }

    // Radio FIRST: HSE32 is sourced from the radio's TCXO (VDD_TCXO), so it
    // MUST be up before any deepSleep or the cold-boot STOP2 wake hangs
    // waiting for HSE. (ST-Link resets hid this; battery cold-boots expose it.)
    // Proven cold-boot-robust in sleeptest9.
    int st = radioStart();
    DBGF("radio.begin = %d\n", st);

    // begin(true) forces resetBackupDomain() -> clears any corrupt RTC/backup
    // clock selection so STOP2 wake timing is reliable on a cold boot.
    rtc.setClockSource(STM32RTC::LSI_CLOCK);   // Wio-E5 has no LSE crystal
    rtc.begin(true);
    LowPower.begin();
}

void loop() {
    cycleNum++;
    DBGF("--- cycle %u ---\n", cycleNum);

    // P1: wake marker. Radio was already recovered at the end of phaseSleep
    // (standby/ re-begin right after deepSleep), so nothing to do here.
    beginPhase(1);

    uint32_t imgBytes = phaseCapture();        // P2 (gated)
    phasePreprocess(imgBytes);                 // P3
    phaseLoraTx(cycleNum, imgBytes);           // P4
    phaseRxWindow();                           // P5
    phaseSleep();                              // P6 + STOP2
}
#endif // GATE_TEST
#endif // NODE_MULTIYEAR
