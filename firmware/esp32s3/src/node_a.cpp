/**
 * ============================================================
 *  DEPLOYABLE PLATFORM-A NODE — XIAO ESP32-S3
 *  Gated ArduCAM Mini (SPI) + on-device jomjol CNN + WiFi (send reading)
 *  + deep sleep. Mirrors the E5 node's power management on the SAME camera,
 *  so A (WiFi + CNN + reading) vs the E5 node (LoRa + ROI image) is an
 *  apples-to-apples cross-platform comparison at equal gating.
 *
 *  Replaces the integrated-Sense camera (esp_camera, un-gateable) with the
 *  same ArduCAM Mini used on the E5, low-side power-gated via a MOSFET, so
 *  Platform A can finally reach a gated sleep floor.
 *
 *  Wiring (ArduCAM Mini -> XIAO ESP32-S3, from arducam_test.cpp):
 *    CS  -> D3(GPIO4)  SCK -> D8(GPIO7)  MISO -> D9(GPIO8)
 *    MOSI-> D10(GPIO9) SDA -> D4(GPIO5)  SCL  -> D5(GPIO6)
 *    VCC -> 3V3        GND -> MOSFET Drain; Source -> GND
 *    MOSFET Gate -> D1(GPIO2) + 10k pulldown to GND
 *    Marker: GPIO43(D6) -> logger D5
 *
 *  Phases: P1 init | P2 gated ArduCAM capture | P3 preproc |
 *          P4 jomjol CNN | P5 WiFi TX (reading) | P6 deep-sleep entry
 *
 *  Envs: node_a (markers->logger), node_a_debug (USB serial).
 *  Author: Youness Chakir — Chouaib Doukkali University
 * ============================================================
 */
#ifdef NODE_A
#include <Arduino.h>
#include <cmath>
// TFLite/STL headers FIRST, in a clean context: ArduCAM.h's short register
// macros otherwise clobber the STL templates these pull in (stl_tree error).
#include "dig_class100_model.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"
#include "esp_sleep.h"
#include "esp_timer.h"

// ---- config ----
#define CPU_MHZ            240
#define WIFI_SSID          "YOUR_WIFI_SSID"
#define WIFI_PASSWORD      "YOUR_WIFI_PASSWORD"
#define WIFI_MAX_ATTEMPTS  40
#define CYCLE_COUNT        30
#define CYCLE_INTERVAL_SEC 30
#define AEC_SETTLE_MS      1000

// ArduCAM Mini pins on XIAO ESP32-S3 (proven in arducam_test.cpp)
#define CAM_CS    4
#define PIN_SCK   7
#define PIN_MISO  8
#define PIN_MOSI  9
#define PIN_SDA   5
#define PIN_SCL   6
#define CAM_GATE  2      // MOSFET gate (low-side on camera GND) + 10k pulldown

// markers
#define MARKER_BAUD    9600
#define MARKER_TX_PIN  43
#define MARKER_RX_PIN  44
#define MARK_SYNC      0xFF
#define MARK_CYCLE_END 0xA0
#define MARK_PHASE(n)  (0xA0 | (n))

#ifdef DEBUG_OUTPUT
  #define DLOG(...) Serial.printf(__VA_ARGS__)
  #define DBEGIN()  do { Serial.begin(115200); delay(100); } while(0)
#else
  #define DLOG(...)
  #define DBEGIN()
#endif

RTC_DATA_ATTR int           bootCount   = 0;
RTC_DATA_ATTR unsigned long totalCycles = 0;

ArduCAM myCAM(OV2640, CAM_CS);
static float g_input[784];        // 28x28 preprocessed buffer for the CNN

static inline void sendMarker(uint8_t b) { Serial1.write(b); Serial1.flush(); }
static inline void beginPhase(int n)     { sendMarker(MARK_PHASE(n)); }

// ---- TFLite (jomjol dig-class100, int8, 32x20x3 -> 100) ----
static tflite::MicroInterpreter* g_interp = nullptr;
static constexpr int kArena = 160 * 1024;
alignas(16) static uint8_t g_arena[kArena];
static void initTFLite() {
    const tflite::Model* m = tflite::GetModel(g_dig_model);
    if (m->version() != TFLITE_SCHEMA_VERSION) { DLOG("schema mismatch\n"); return; }
    static tflite::MicroMutableOpResolver<8> r;
    r.AddConv2D(); r.AddMaxPool2D(); r.AddFullyConnected(); r.AddReshape();
    r.AddAdd(); r.AddMul(); r.AddQuantize(); r.AddDequantize();
    static tflite::MicroInterpreter it(m, r, g_arena, kArena);
    if (it.AllocateTensors() != kTfLiteOk) { DLOG("AllocateTensors FAILED\n"); return; }
    g_interp = &it;
    DLOG("TFLite ready in=%u out=%u\n",
         (unsigned)it.input(0)->bytes, (unsigned)it.output(0)->bytes);
}

// ---- camera power gate ----
static void gateOn() { digitalWrite(CAM_GATE, HIGH); delay(120); }
static void gateOff() {
    SPI.end(); Wire.end();
    const int pins[] = {PIN_MOSI, PIN_MISO, PIN_SCK, PIN_SDA, PIN_SCL, CAM_CS};
    for (unsigned i = 0; i < 6; i++) pinMode(pins[i], INPUT);   // Hi-Z (no phantom sink)
    digitalWrite(CAM_GATE, LOW);                                // camera GND cut
}

// full cold bring-up after gate ON (camera lost all state)
static bool cameraUp() {
    Wire.begin(PIN_SDA, PIN_SCL);
    pinMode(CAM_CS, OUTPUT); digitalWrite(CAM_CS, HIGH);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, CAM_CS);
    myCAM.write_reg(0x07, 0x80); delay(100);   // CPLD reset
    myCAM.write_reg(0x07, 0x00); delay(100);
    myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
    if (myCAM.read_reg(ARDUCHIP_TEST1) != 0x55) return false;
    myCAM.wrSensorReg8_8(0xff, 0x01);
    myCAM.set_format(JPEG);
    myCAM.InitCAM();
    myCAM.OV2640_set_JPEG_size(OV2640_320x240);   // QVGA (same as original Platform A)
    delay(AEC_SETTLE_MS);
    myCAM.clear_fifo_flag();
    return true;
}

// ---- P2: gated capture -> read JPEG, sample into the CNN input buffer ----
static uint32_t phaseCapture() {
    beginPhase(2);
    gateOn();
    if (!cameraUp()) { DLOG("camera FAIL\n"); gateOff(); return 1; }
    myCAM.flush_fifo(); myCAM.clear_fifo_flag(); myCAM.start_capture();
    uint32_t t0 = millis();
    while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK))
        if (millis() - t0 > 3000) { DLOG("cap TIMEOUT\n"); break; }
    uint32_t len = myCAM.read_fifo_length();
    // Read the JPEG out (realistic readout energy) and subsample into g_input.
    myCAM.CS_LOW(); myCAM.set_fifo_burst();
    uint32_t step = (len > 784) ? len / 784 : 1;
    uint32_t bi = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = SPI.transfer(0x00);
        if (i % step == 0 && bi < 784) g_input[bi++] = b / 255.0f;
    }
    myCAM.CS_HIGH();
    gateOff();                     // camera dark until next reading
    DLOG("P2 capture len=%lu\n", (unsigned long)len);
    return len;
}

static void phasePreprocess() {
    beginPhase(3);                 // reference preprocessing load
    volatile float acc = 0; for (int i = 0; i < 784; i++) acc += g_input[i]; (void)acc;
}

// ---- P4: real jomjol inference, 8 wheels ----
static void phaseCNN(char* reading) {
    beginPhase(4);
    int digits[8] = {0};
    if (!g_interp) { reading[0] = 0; return; }
    TfLiteTensor* in  = g_interp->input(0);
    TfLiteTensor* out = g_interp->output(0);
    for (int d = 0; d < 8; d++) {
        float* p = in->data.f;
        for (int y = 0; y < 32; y++) {
            int sy = (y * 28) / 32;
            for (int x = 0; x < 20; x++) {
                int sx = (x * 28) / 20;
                float v = g_input[(sy * 28 + sx) % 784];
                int idx = (y * 20 + x) * 3;
                p[idx] = p[idx + 1] = p[idx + 2] = v;
            }
        }
        if (g_interp->Invoke() != kTfLiteOk) continue;
        int best = 0; float bv = out->data.f[0];
        for (int i = 1; i < 100; i++) if (out->data.f[i] > bv) { bv = out->data.f[i]; best = i; }
        digits[d] = (best / 10) % 10;
    }
    snprintf(reading, 16, "%d%d%d%d%d.%d%d%d",
             digits[0], digits[1], digits[2], digits[3],
             digits[4], digits[5], digits[6], digits[7]);
    DLOG("Reading: %s m3\n", reading);
}

// ---- P5: WiFi connect + UDP send the reading (few bytes) ----
static void phaseWiFi(const char* reading) {
    beginPhase(5);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int a = 0;
    while (WiFi.status() != WL_CONNECTED && a < WIFI_MAX_ATTEMPTS) { delay(250); a++; }
    if (WiFi.status() == WL_CONNECTED) {
        WiFiUDP udp; udp.beginPacket("255.255.255.255", 9999);
        udp.printf("AMR:A:%lu:%s", totalCycles, reading); udp.endPacket();
        DLOG("WiFi OK (%d), sent\n", a);
    } else DLOG("WiFi TIMEOUT\n");
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
}

static void enterSleep() {
    beginPhase(6);
    sendMarker(MARK_CYCLE_END);
    delay(5);
    esp_sleep_enable_timer_wakeup((uint64_t)CYCLE_INTERVAL_SEC * 1000000ULL);
    esp_deep_sleep_start();
}

void setup() {
    setCpuFrequencyMhz(CPU_MHZ);
    DBEGIN();
    Serial1.begin(MARKER_BAUD, SERIAL_8N1, MARKER_RX_PIN, MARKER_TX_PIN);
    delay(20);
    pinMode(CAM_GATE, OUTPUT); digitalWrite(CAM_GATE, LOW);   // camera OFF

    bootCount++; totalCycles++;
    if (bootCount > CYCLE_COUNT) {
        sendMarker(MARK_CYCLE_END); delay(10);
        esp_sleep_enable_timer_wakeup(0xFFFFFFFFFFFFFFFFULL);
        esp_deep_sleep_start();
    }
    if (bootCount == 1) { for (int i = 0; i < 3; i++) { sendMarker(MARK_SYNC); delay(10); } }
    else sendMarker(MARK_SYNC);

    DLOG("\n=== NODE-A (gated ArduCAM + jomjol + WiFi) cycle %d ===\n", bootCount);

    initTFLite();          // model load outside the phase markers

    beginPhase(1);         // P1 init (minimal)

    char reading[16];
    phaseCapture();        // P2 gated ArduCAM capture
    phasePreprocess();     // P3
    phaseCNN(reading);     // P4 jomjol inference
    phaseWiFi(reading);    // P5 WiFi send reading
    enterSleep();          // P6 deep sleep
}
void loop() {}
#endif // NODE_A
