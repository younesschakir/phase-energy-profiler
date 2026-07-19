// ============================================================
//  STOP2 sleep diagnostic. Built only by env:lora_e5_sleeptest.
//  Forces RTC->LSI (Wio-E5 has no 32.768kHz LSE) and prints a TICK
//  each loop around deepSleep(5000). Time the interval between TICKs
//  externally: ~6s (5s sleep + 1s delay) = timing OK; way off = clock bug.
//  Debug out: CP2102 (USART1 PB6/PB7) @ 9600 (COM9).
// ============================================================
#ifdef SLEEP_TEST
#include <Arduino.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>

STM32RTC& rtc = STM32RTC::getInstance();

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(500);
    Serial.println("\n=== STOP2 SLEEP TEST (LSI) ===");
    rtc.setClockSource(STM32RTC::LSI_CLOCK);   // no LSE on the Wio-E5 module
    rtc.begin();
    LowPower.begin();
    Serial.println("init ok; ticking every ~6s (5s deepSleep + 1s delay)");
    Serial.flush();
}

uint32_t n = 0;
void loop() {
    Serial.print("TICK "); Serial.println(n++);
    Serial.flush();
    delay(1000);
    LowPower.deepSleep(5000);   // request 5s STOP2
}
#endif // SLEEP_TEST
