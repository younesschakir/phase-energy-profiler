// ============================================================
//  Marker path isolation test. Built only by env:lora_e5_markertest.
//  Continuously streams phase markers on PA9 -> logger D5, nothing else.
//  Expected on the logger: "# --- Cycle N ---", PHASE_CSV lines, and the
//  CSV phase column cycling 1..6.  Isolates E5-send/wire from the real fw.
// ============================================================
#ifdef MARKER_TEST
#include <Arduino.h>

#define MARK_CYCLE_END  0xA0
#define MARK_PHASE(n)   (0xA0 | (n))

static void sendMarker(uint8_t b) {
    Serial.write(b);
    Serial.flush();
    delay(2);
}

void setup() {
    Serial.setTx(PA_9);      // markers -> logger D5 (same as real firmware)
    Serial.setRx(PB_7);
    Serial.begin(9600);
}

void loop() {
    for (uint8_t p = 1; p <= 6; p++) {
        sendMarker(MARK_PHASE(p));
        delay(150);
    }
    sendMarker(MARK_CYCLE_END);
    delay(800);
}
#endif // MARKER_TEST
