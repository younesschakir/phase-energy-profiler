// ============================================================
//  STOP2 sleep + RADIO diagnostic. Built only by env:lora_e5_sleeptest2.
//  Same as sleeptest (LSI) but with the STM32WLx radio initialised and
//  put to sleep before each deepSleep -- to see if radio.begin/clock config
//  breaks the RTC wake timing. Time the TICK interval: ~6s = OK.
//  Debug out: CP2102 (PB6/PB7) @ 9600 (COM9).
// ============================================================
#ifdef SLEEP_TEST2
#include <Arduino.h>
#include <RadioLib.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>

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

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(500);
    Serial.println("\n=== SLEEP TEST 2 (LSI + RADIO) ===");
    rtc.setClockSource(STM32RTC::LSI_CLOCK);
    rtc.begin();
    LowPower.begin();
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    int st = radio.begin(868.1, 125.0, 7, 5,
                         RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 15, 8);
    Serial.print("radio.begin() = "); Serial.println(st);
    Serial.println("ticking every ~6s (5s deepSleep + 1s delay)");
    Serial.flush();
}

uint32_t n = 0;
void loop() {
    Serial.print("TICK "); Serial.println(n++);
    Serial.flush();
    radio.sleep(false);          // as in the real firmware, before sleeping
    delay(1000);
    LowPower.deepSleep(5000);
}
#endif // SLEEP_TEST2
