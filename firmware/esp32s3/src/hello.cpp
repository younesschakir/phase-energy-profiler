#ifdef HELLO_ONLY
#include <Arduino.h>
void setup() {
    Serial.begin(115200);
}
void loop() {
    static uint32_t n = 0;
    Serial.print("hello "); Serial.println(n++);
    delay(500);
}
#endif
