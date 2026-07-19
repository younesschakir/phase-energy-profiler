// ============================================================
//  STOP2 + radio TX/RX diagnostic. env:lora_e5_sleeptest3.
//  Mimics the full firmware's radio usage each cycle: transmit + receive
//  + sleep, THEN deepSleep. Isolates whether TX/RX (esp. the RX window)
//  breaks the STOP2 wake timing. Time the TICK interval: ~6-7s = OK.
//  Debug out: CP2102 (PB6/PB7) @ 9600 (COM9).
// ============================================================
#ifdef SLEEP_TEST4
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
    Serial.setTx(PB_6); Serial.setRx(PB_7); Serial.begin(9600); delay(500);
    Serial.println("\n=== SLEEP TEST 3 (LSI + radio + LowPower.sleep) ===");
    rtc.setClockSource(STM32RTC::LSI_CLOCK);
    rtc.begin();
    LowPower.begin();
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    int st = radio.begin(868.1, 125.0, 7, 5,
                         RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 15, 8);
    Serial.print("radio.begin() = "); Serial.println(st);
    Serial.flush();
}

uint32_t n = 0;
void loop() {
    Serial.print("TICK "); Serial.println(n++);
    Serial.flush();

    uint8_t pl[16]; memset(pl, 0xAA, sizeof(pl));
    unsigned long tA = millis();
    radio.transmit(pl, sizeof(pl));           // like P4
    unsigned long tB = millis();

    uint8_t rx[32];
    int rs = radio.receive(rx, sizeof(rx));   // like P5 (RX window)
    unsigned long tC = millis();

    Serial.print("  tx_ms="); Serial.print(tB - tA);
    Serial.print(" rx_ms="); Serial.print(tC - tB);
    Serial.print(" rxState="); Serial.println(rs);
    Serial.flush();

    radio.sleep(false);
    delay(1000);
    LowPower.sleep(5000);
}
#endif // SLEEP_TEST4
