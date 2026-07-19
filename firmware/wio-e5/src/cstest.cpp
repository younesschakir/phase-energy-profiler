// ============================================================
//  CS drive test.  Built only by env:lora_e5_cstest.
//  Toggles PA0 (the CS pin): HIGH 3s, LOW 3s, forever, printing state.
//  Measure with a multimeter at the E5 PA0 pin AND the camera CS pin:
//  both should flip 0V <-> 3.3V every 3s in sync.
//    Debug out: CP2102 (USART1 PB6/PB7) @ 9600  (COM9)
// ============================================================
#ifdef CSTEST
#include <Arduino.h>

#define CAM_CS  PB_9      // was PA_0 (dead output on this board)

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(300);
    pinMode(CAM_CS, OUTPUT);
}

void loop() {
    // Fast ~1kHz square wave. A multimeter averages this to ~1.6V if the
    // pin is really toggling; 0V = stuck low, 3.3V = stuck high.
    for (uint32_t i = 0; i < 2000; i++) {
        digitalWrite(CAM_CS, HIGH);
        delayMicroseconds(500);
        digitalWrite(CAM_CS, LOW);
        delayMicroseconds(500);
    }
    Serial.println("PB9 fast-toggling ~1kHz (measure E5 PB9: expect ~1.6V)");
}
#endif // CSTEST
