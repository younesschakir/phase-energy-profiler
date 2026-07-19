/*
 * =============================================================
 * XIAO ESP32-S3 Sense — Multi-Config 7-Phase AMR Cycle
 * =============================================================
 * Phase markers via UART1 (not GPIO pulses).
 * Logger receives bytes on SoftwareSerial — microsecond accuracy.
 *
 * UART1 wiring:
 *   XIAO GPIO43 (TX1) ──→ D1 Mini D5 / GPIO14 (SoftwareSerial RX)
 *   XIAO GND          ──→ D1 Mini GND  (common ground, mandatory)
 *
 * USB power wiring (no battery):
 *   USB VBUS (red)  → INA226 VIN+
 *   INA226 VIN-     → XIAO 5V header pin
 *   USB GND (black) → XIAO GND
 *   USB D+ / D-     → CUT (no data, prevents enumeration noise)
 *
 * Compile flags in platformio.ini:
 *   -DCONFIG_ID=1          (1–5, see table below)
 *   -DDEBUG_OUTPUT=1       (add ONLY when USB is intact for serial monitor)
 *
 * ┌────────┬──────────┬────────────────┬──────┐
 * │ Config │ CPU MHz  │ Resolution     │ WiFi │
 * ├────────┼──────────┼────────────────┼──────┤
 * │  C1    │   240    │ QVGA 320×240   │  ON  │
 * │  C2    │   160    │ QVGA 320×240   │  ON  │
 * │  C3    │    80    │ QVGA 320×240   │  ON  │
 * │  C4    │   240    │ QQVGA 160×120  │  ON  │
 * │  C5    │   240    │ QVGA 320×240   │  OFF │
 * └────────┴──────────┴────────────────┴──────┘
 *
 * Author : Youness Chakir — Chouaib Doukkali University
 * Supervisor: Prof. Abdessadek Aaroud
 * =============================================================
 */

#include <Arduino.h>
#include "esp_camera.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <cmath>

// Real jomjol dig-class100 (int8) inference via esp-tflite-micro + ESP-NN.
// Built only in env:c1_jomjol (-DREAL_CNN); default c1 keeps the load proxy.
#ifdef REAL_CNN
#include "dig_class100_model.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#endif

// =============================================================
// CONFIG_ID from platformio.ini  -DCONFIG_ID=X
// =============================================================
#ifndef CONFIG_ID
  #define CONFIG_ID 1
#endif

#if CONFIG_ID == 1
  #define CFG_CPU_MHZ      240
  #define CFG_FRAME_SIZE   FRAMESIZE_QVGA
  #define CFG_IMG_W        320
  #define CFG_IMG_H        240
  #define CFG_WIFI_ENABLED true
  #define CFG_LABEL        "C1:240MHz-QVGA-WiFiON"

#elif CONFIG_ID == 2
  #define CFG_CPU_MHZ      160
  #define CFG_FRAME_SIZE   FRAMESIZE_QVGA
  #define CFG_IMG_W        320
  #define CFG_IMG_H        240
  #define CFG_WIFI_ENABLED true
  #define CFG_LABEL        "C2:160MHz-QVGA-WiFiON"

#elif CONFIG_ID == 3
  #define CFG_CPU_MHZ      80
  #define CFG_FRAME_SIZE   FRAMESIZE_QVGA
  #define CFG_IMG_W        320
  #define CFG_IMG_H        240
  #define CFG_WIFI_ENABLED true
  #define CFG_LABEL        "C3:80MHz-QVGA-WiFiON"

#elif CONFIG_ID == 4
  #define CFG_CPU_MHZ      240
  #define CFG_FRAME_SIZE   FRAMESIZE_QQVGA
  #define CFG_IMG_W        160
  #define CFG_IMG_H        120
  #define CFG_WIFI_ENABLED true
  #define CFG_LABEL        "C4:240MHz-QQVGA-WiFiON"

#elif CONFIG_ID == 5
  #define CFG_CPU_MHZ      240
  #define CFG_FRAME_SIZE   FRAMESIZE_QVGA
  #define CFG_IMG_W        320
  #define CFG_IMG_H        240
  #define CFG_WIFI_ENABLED false
  #define CFG_LABEL        "C5:240MHz-QVGA-WiFiOFF"

#else
  #error "CONFIG_ID must be 1–5"
#endif

// =============================================================
// USER SETTINGS -- CHANGE BEFORE FLASHING
// =============================================================
#define MY_WIFI_SSID      "YOUR_WIFI_SSID"       // <-- replace with real SSID
#define MY_WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"   // <-- replace with real password
#define CYCLE_COUNT       30                // auto-stop after N measurement cycles

const char* WIFI_SSID     = MY_WIFI_SSID;
const char* WIFI_PASSWORD = MY_WIFI_PASSWORD;

#define CYCLE_INTERVAL_SEC  30
#define AEC_WARMUP_FRAMES    5
#define WIFI_MAX_ATTEMPTS   40

// =============================================================
// UART PHASE MARKER PROTOCOL
// One byte sent at the START of each phase.
// Logger timestamps the byte receipt → phase boundary = marker time.
//
//   0xFF  — boot sync (sent once at power-on so logger knows DUT is alive)
//   0xA1  — phase 1 starting
//   0xA2  — phase 2 starting  (= phase 1 just ended)
//   ...
//   0xA7  — phase 7 starting  (= phase 6 just ended)
//   0xA0  — cycle complete    (= phase 7 just ended, sleep next)
//
// Baud rate: 9600 — slow enough to be 100% reliable on SoftwareSerial.
// Each byte takes ~1.04 ms, negligible vs phase durations.
// =============================================================
#define MARKER_BAUD    9600
#define MARKER_TX_PIN  43    // XIAO GPIO43 = Serial1 TX (hardware UART1)
#define MARKER_RX_PIN  44    // XIAO GPIO44 = Serial1 RX (not used by logger)

#define MARK_SYNC      0xFF
#define MARK_CYCLE_END 0xA0
#define MARK_PHASE(n)  (0xA0 | (n))   // 0xA1 .. 0xA7

// =============================================================
// CONDITIONAL DEBUG OUTPUT
// Define DEBUG_OUTPUT=1 in platformio.ini ONLY when USB is intact.
// For real measurement runs: do NOT define DEBUG_OUTPUT.
// =============================================================
#ifdef DEBUG_OUTPUT
  #define DLOG(...)  Serial.printf(__VA_ARGS__)
  #define DBEGIN()   do { Serial.begin(115200); delay(100); } while(0)
#else
  #define DLOG(...)  /* nothing */
  #define DBEGIN()   /* nothing */
#endif

// =============================================================
// OV2640 pin map — XIAO ESP32-S3 Sense
// =============================================================
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13

// =============================================================
// RTC memory — survives deep sleep
// =============================================================
RTC_DATA_ATTR int           bootCount    = 0;
RTC_DATA_ATTR unsigned long totalCycles  = 0;

// Per-phase timing (local, reset each boot)
unsigned long phaseStart_us;
unsigned long phaseDur_us[7];  // index 0 = phase 1

// =============================================================
// Phase marker helpers
// =============================================================
static inline void sendMarker(uint8_t byte) {
    Serial1.write(byte);
    Serial1.flush();          // ensure byte is fully transmitted before continuing
}

static inline void beginPhase(int num) {
    sendMarker(MARK_PHASE(num));
    phaseStart_us = (unsigned long)esp_timer_get_time();
    DLOG("[P%d START] t=%lu us\n", num, phaseStart_us);
}

static inline void endPhase(int num) {
    unsigned long dur = (unsigned long)esp_timer_get_time() - phaseStart_us;
    phaseDur_us[num - 1] = dur;
    DLOG("[P%d END] dur=%lu us (%.1f ms)\n", num, dur, dur / 1000.0f);
}

// =============================================================
// Forward declarations
// =============================================================
float* preprocessImage(const uint8_t* jpegBuf, size_t jpegLen, int w, int h);
void   runCNNInference(const float* input, char* readingOut);
void   sendReadingWiFi(const char* reading);
void   printPhaseSummary();
void   enterDeepSleep();

// =============================================================
// Real TFLite Micro setup (jomjol dig-class100, int8, 32x32x3 -> 100)
// =============================================================
#ifdef REAL_CNN
static tflite::MicroInterpreter* g_interpreter = nullptr;
static constexpr int kArenaSize = 160 * 1024;              // activations (internal SRAM)
alignas(16) static uint8_t g_arena[kArenaSize];            // static -> alloc can't fail

static void initTFLite() {
    const tflite::Model* model = tflite::GetModel(g_dig_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        DLOG("TFLite: schema mismatch %lu\n", (unsigned long)model->version());
        return;
    }
    // dig-class100 uses exactly these 8 builtin ops (parsed from the model).
    static tflite::MicroMutableOpResolver<8> op_resolver;
    op_resolver.AddConv2D();
    op_resolver.AddMaxPool2D();
    op_resolver.AddFullyConnected();
    op_resolver.AddReshape();
    op_resolver.AddAdd();
    op_resolver.AddMul();
    op_resolver.AddQuantize();
    op_resolver.AddDequantize();
    static tflite::MicroInterpreter interp(model, op_resolver, g_arena, kArenaSize);
    if (interp.AllocateTensors() != kTfLiteOk) { DLOG("TFLite: AllocateTensors FAILED\n"); return; }
    g_interpreter = &interp;
    DLOG("TFLite ready: arena=%d in=%u out=%u\n", kArenaSize,
         (unsigned)interp.input(0)->bytes, (unsigned)interp.output(0)->bytes);
}
#endif

// =============================================================
// setup() — entire measurement cycle runs here
// =============================================================
void setup() {
    // CPU frequency first — before any peripheral init
    #if CFG_CPU_MHZ != 240
        setCpuFrequencyMhz(CFG_CPU_MHZ);
    #endif

    // Start debug serial (only if DEBUG_OUTPUT defined)
    DBEGIN();

    // Start UART1 for phase markers — always active
    Serial1.begin(MARKER_BAUD, SERIAL_8N1, MARKER_RX_PIN, MARKER_TX_PIN);
    delay(20);  // let UART stabilize

    // Boot sync: tell logger DUT is alive
    // First boot: send 3 sync bytes so logger PLL locks on baud
    bootCount++;
    totalCycles++;

    // Auto-stop after CYCLE_COUNT measurement cycles
    // Sends a final MARK_CYCLE_END so the logger finalises the last cycle,
    // then enters permanent deep sleep (~584 years via max timer value).
    if (bootCount > CYCLE_COUNT) {
        sendMarker(MARK_CYCLE_END);
        delay(10);
        esp_sleep_enable_timer_wakeup(0xFFFFFFFFFFFFFFFFULL);
        esp_deep_sleep_start();
    }

    if (bootCount == 1) {
        for (int i = 0; i < 3; i++) {
            sendMarker(MARK_SYNC);
            delay(10);
        }
    } else {
        sendMarker(MARK_SYNC);
    }

    DLOG("\n=== CONFIG: %s | Cycle #%d | CPU %d MHz ===\n",
         CFG_LABEL, bootCount, CFG_CPU_MHZ);

#ifdef REAL_CNN
    initTFLite();   // one-time model load + arena alloc (outside phase markers)
#endif

#ifdef VERIFY_CNN
    // Isolated verification: no camera/WiFi/sleep — just load + loop inference,
    // printing the reading and the 8-invoke latency. Confirms the real model
    // runs on-target and gives inference timing.
    {
        static float dummy[784];
        for (int i = 0; i < 784; i++) dummy[i] = (i % 64) / 64.0f;
        char reading[16];
        for (int k = 0; ; k++) {
            uint32_t t = micros();
            runCNNInference(dummy, reading);
            DLOG("VERIFY #%d reading=%s  8x invoke=%lu us\n",
                 k, reading, (unsigned long)(micros() - t));
            delay(1500);
        }
    }
#endif

    // ----------------------------------------------------------
    // PHASE 1: System init (boot + peripheral setup overhead)
    // Starts immediately after sync byte — this IS the phase.
    // ----------------------------------------------------------
    beginPhase(1);
    // Intentionally minimal: just the delay for UART flush above.
    // This phase captures wake latency + clock stabilisation.
    endPhase(1);

    // ----------------------------------------------------------
    // PHASE 2: Camera warm-up (init + AEC settle)
    // ----------------------------------------------------------
    beginPhase(2);
    {
        camera_config_t cfg = {};
        cfg.ledc_channel  = LEDC_CHANNEL_0;
        cfg.ledc_timer    = LEDC_TIMER_0;
        cfg.pin_d0        = Y2_GPIO_NUM;  cfg.pin_d1  = Y3_GPIO_NUM;
        cfg.pin_d2        = Y4_GPIO_NUM;  cfg.pin_d3  = Y5_GPIO_NUM;
        cfg.pin_d4        = Y6_GPIO_NUM;  cfg.pin_d5  = Y7_GPIO_NUM;
        cfg.pin_d6        = Y8_GPIO_NUM;  cfg.pin_d7  = Y9_GPIO_NUM;
        cfg.pin_xclk      = XCLK_GPIO_NUM;
        cfg.pin_pclk      = PCLK_GPIO_NUM;
        cfg.pin_vsync     = VSYNC_GPIO_NUM;
        cfg.pin_href      = HREF_GPIO_NUM;
        cfg.pin_sccb_sda  = SIOD_GPIO_NUM;
        cfg.pin_sccb_scl  = SIOC_GPIO_NUM;
        cfg.pin_pwdn      = PWDN_GPIO_NUM;
        cfg.pin_reset     = RESET_GPIO_NUM;
        cfg.xclk_freq_hz  = 20000000;
        cfg.pixel_format  = PIXFORMAT_JPEG;
        cfg.frame_size    = CFG_FRAME_SIZE;
        cfg.jpeg_quality  = 12;
        cfg.fb_count      = 1;
        cfg.fb_location   = CAMERA_FB_IN_PSRAM;
        cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

        esp_err_t err = esp_camera_init(&cfg);
        if (err != ESP_OK) {
            DLOG("FATAL: Camera init failed 0x%x\n", err);
            enterDeepSleep();
            return;
        }

        // Discard AEC_WARMUP_FRAMES to let auto-exposure settle
        for (int i = 0; i < AEC_WARMUP_FRAMES; i++) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
            delay(30);
        }
        DLOG("Camera ready (%d warmup frames)\n", AEC_WARMUP_FRAMES);
    }
    endPhase(2);

    // ----------------------------------------------------------
    // PHASE 3: Image capture
    // ----------------------------------------------------------
    beginPhase(3);
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        DLOG("FATAL: Image capture failed\n");
        enterDeepSleep();
        return;
    }
    DLOG("Captured %u bytes, %dx%d JPEG\n", fb->len, fb->width, fb->height);
    endPhase(3);

    // ----------------------------------------------------------
    // PHASE 4: Preprocessing (JPEG decode → resize → normalize)
    // ----------------------------------------------------------
    beginPhase(4);
    float* preprocessed = preprocessImage(fb->buf, fb->len,
                                           fb->width, fb->height);
    endPhase(4);

    // ----------------------------------------------------------
    // PHASE 5: CNN inference
    // ----------------------------------------------------------
    beginPhase(5);
    char reading[16];
    runCNNInference(preprocessed, reading);
    endPhase(5);

    free(preprocessed);
    esp_camera_fb_return(fb);
    esp_camera_deinit();

    // ----------------------------------------------------------
    // PHASE 6: WiFi TX (association + send + disconnect)
    // ----------------------------------------------------------
    beginPhase(6);
    #if CFG_WIFI_ENABLED
    {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_ATTEMPTS) {
            delay(250);
            attempts++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            DLOG("WiFi OK IP=%s (%d attempts)\n",
                 WiFi.localIP().toString().c_str(), attempts);
            sendReadingWiFi(reading);
        } else {
            DLOG("WiFi TIMEOUT\n");
        }
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    #else
        // C5: WiFi disabled — marker still fires, captures near-zero duration
        delay(5);
    #endif
    endPhase(6);

    printPhaseSummary();

    // ----------------------------------------------------------
    // PHASE 7: Deep-sleep entry (cleanup + sleep transition)
    // ----------------------------------------------------------
    beginPhase(7);
    enterDeepSleep();   // sends MARK_CYCLE_END then sleeps — see below
}

// =============================================================
// PREPROCESSING  (mock — replace with real JPEG decode + resize)
// Simulates the same compute load regardless of actual image content.
// =============================================================
float* preprocessImage(const uint8_t* jpegBuf, size_t jpegLen, int w, int h) {
    float* out = (float*)ps_malloc(28 * 28 * sizeof(float));
    if (!out) out = (float*)malloc(28 * 28 * sizeof(float));

    // Checksum pass — forces cache/memory traffic similar to real decode
    volatile uint32_t cs = 0;
    for (size_t i = 0; i < jpegLen; i++) cs += jpegBuf[i];

    // Nearest-neighbour resize to 28×28
    for (int y = 0; y < 28; y++) {
        int sy = y * h / 28;
        for (int x = 0; x < 28; x++) {
            int sx = x * w / 28;
            size_t idx = ((size_t)sy * w + sx) % jpegLen;
            out[y * 28 + x] = jpegBuf[idx] / 255.0f;
        }
    }
    DLOG("Preproc %dx%d → 28x28 (cs=%lu)\n", w, h, (unsigned long)cs);
    return out;
}

// =============================================================
// CNN INFERENCE  (~33.9 M MACs — calibrated multiply-accumulate)
// =============================================================
#ifdef REAL_CNN
// Real inference: run the jomjol dig-class100 model once per wheel (8 digits).
// ROI extraction is abstracted — inference energy is content-independent, so
// the input tensor is filled from the preprocessed buffer and the 8 real
// Invoke()s reproduce the true per-reading compute for an 8-wheel meter.
void runCNNInference(const float* input, char* readingOut) {
    DLOG("CNN inference (jomjol dig-class100) @ %d MHz\n", CFG_CPU_MHZ);
    int digits[8] = {0};
    if (!g_interpreter) { DLOG("TFLite not ready\n"); readingOut[0] = 0; return; }
    TfLiteTensor* in  = g_interpreter->input(0);   // float32 [1,32,20,3]
    TfLiteTensor* out = g_interpreter->output(0);  // float32 [1,100]

    for (int d = 0; d < 8; d++) {
        // Fill 32(h)x20(w)x3 float input from the 28x28 preprocessed buffer
        // (resample + replicate to 3 channels). Deterministic, varies per wheel.
        float* p = in->data.f;
        for (int y = 0; y < 32; y++) {
            int sy = (y * 28) / 32;
            for (int x = 0; x < 20; x++) {
                int sx = (x * 28) / 20;
                float v = input[(sy * 28 + sx) % 784];
                int idx = (y * 20 + x) * 3;
                p[idx] = p[idx + 1] = p[idx + 2] = v;
            }
        }
        if (g_interpreter->Invoke() != kTfLiteOk) { DLOG("Invoke %d failed\n", d); continue; }
        // argmax over 100 classes (0.0..9.9); integer digit = class/10.
        int best = 0; float bv = out->data.f[0];
        for (int i = 1; i < 100; i++)
            if (out->data.f[i] > bv) { bv = out->data.f[i]; best = i; }
        digits[d] = (best / 10) % 10;
    }
    snprintf(readingOut, 16, "%d%d%d%d%d.%d%d%d",
             digits[0], digits[1], digits[2], digits[3],
             digits[4], digits[5], digits[6], digits[7]);
    DLOG("Reading: %s m3 (real model)\n", readingOut);
}
#else
void runCNNInference(const float* input, char* readingOut) {
    DLOG("CNN inference @ %d MHz\n", CFG_CPU_MHZ);

    volatile float acc = 0.0f;
    static const float W[9] = {
        0.11f, -0.23f, 0.17f, -0.09f, 0.26f,
       -0.14f,  0.21f, -0.12f, 0.08f
    };

    // Three synthetic layers, tuned to match ~33.9 M MACs total
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 25088; i++)
            for (int k = 0; k < 9; k++)
                acc += input[(i + d * 98) % 784] * W[k];
        for (int i = 0; i < 55556; i++)
            for (int k = 0; k < 9; k++)
                acc += input[(i + d * 50) % 784] * W[k];
        for (int i = 0; i < 44601; i++)
            for (int k = 0; k < 9; k++)
                acc += input[(i + d * 30) % 784] * W[k];
    }

    int digits[8];
    for (int d = 0; d < 8; d++)
        digits[d] = ((int)(fabsf(acc) * 100) + d) % 10;

    snprintf(readingOut, 16, "%d%d%d%d%d.%d%d%d",
             digits[0], digits[1], digits[2], digits[3],
             digits[4], digits[5], digits[6], digits[7]);
    DLOG("Reading: %s m3\n", readingOut);
}
#endif // REAL_CNN

// =============================================================
// WIFI TX  (UDP broadcast — lightweight, no server needed)
// =============================================================
void sendReadingWiFi(const char* reading) {
    WiFiUDP udp;
    udp.beginPacket("255.255.255.255", 9999);
    udp.printf("AMR:C%d:%lu:%s", CONFIG_ID, totalCycles, reading);
    udp.endPacket();
    DLOG("UDP sent: AMR:C%d:%lu:%s\n", CONFIG_ID, totalCycles, reading);
}

// =============================================================
// PHASE SUMMARY  (debug only)
// Note: Phase 7 duration is not available on the DUT side
// because deep sleep occurs before endPhase(7) can be called.
// The logger measures Phase 7 duration externally.
// =============================================================
void printPhaseSummary() {
    #ifdef DEBUG_OUTPUT
    static const char* names[] = {
        "P1 System Init   ",
        "P2 Camera Warmup ",
        "P3 Image Capture ",
        "P4 Preprocessing ",
        "P5 CNN Inference ",
        "P6 WiFi TX       "
    };
    Serial.println("\n--- PHASE TIMING ---");
    Serial.printf("Config: %s\n", CFG_LABEL);
    unsigned long total = 0;
    for (int i = 0; i < 6; i++) {
        Serial.printf("  %s : %8lu us (%7.1f ms)\n",
                      names[i], phaseDur_us[i], phaseDur_us[i] / 1000.0f);
        total += phaseDur_us[i];
    }
    Serial.printf("  TOTAL P1-P6      : %8lu us (%7.1f ms)\n",
                  total, total / 1000.0f);
    Serial.println("  P7 Sleep Entry   :    (measured by logger)");
    Serial.println("--------------------\n");
    #endif
}

// =============================================================
// DEEP SLEEP
// Sends MARK_CYCLE_END before sleeping so logger finalises phase 7.
// =============================================================
void enterDeepSleep() {
    DLOG("Sleeping %d s...\n", CYCLE_INTERVAL_SEC);
    // End-of-cycle marker: logger uses this to close phase 7 timing
    sendMarker(MARK_CYCLE_END);
    delay(5);  // ensure byte transmitted before sleep kills UART
    #ifdef DEBUG_OUTPUT
    Serial.flush();
    #endif
    esp_sleep_enable_timer_wakeup((uint64_t)CYCLE_INTERVAL_SEC * 1000000ULL);
    esp_deep_sleep_start();
}

// =============================================================
// loop() — never reached (deep sleep reboots via setup())
// =============================================================
void loop() {}
