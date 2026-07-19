// ============================================================
//  STAGED STOP2 bisection — ONE flash finds the poison stage.
//  (env:lora_e5_sleeptest8)
//
//  Every loop: TICK n -> deepSleep(5s) -> wake. Radio features switch on
//  progressively by loop count; where the TICKs STOP = the exact call that
//  kills the honest (no-debugger) RTC-alarm wake:
//    n 0-4   : bare STOP2 (must tick — sanity, = proven sleeptest)
//    n == 5  : radio.begin() once, nothing else
//    n 5-9   : STOP2 with radio merely initialized
//    n >= 10 : + TX each loop (with the Dio1-detach storm fix)
//    n >= 15 : + radio.sleep(true) before / standby() after each sleep
//
//  PROTOCOL: flash -> FULL POWER-CYCLE (both cables out 5 s) -> PuTTY COM9
//  @9600 -> note the LAST line printed.
// ============================================================
#ifdef SLEEP_TEST8
#include <Arduino.h>
#include <RadioLib.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>
#include <SubGhz.h>

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

static int radioStart() {
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    return radio.begin(868.1, 125.0, 7, 5,
                       RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 15, 8);
}

void setup() {
    Serial.setTx(PB_6); Serial.setRx(PB_7); Serial.begin(9600); delay(500);
    Serial.println("\n=== SLEEP TEST 8: staged bisection ===");
    rtc.setClockSource(STM32RTC::LSI_CLOCK);
    rtc.begin();
    LowPower.begin();
    Serial.println("stages: 0-4 bare | 5 begin | 10 +TX | 15 +radio.sleep");
    Serial.flush();
}

uint32_t n = 0;
bool radioUp = false;

void loop() {
    Serial.print("TICK "); Serial.println(n);

    if (n == 5) {
        int st = radioStart();
        radioUp = (st == RADIOLIB_ERR_NONE);
        Serial.print("  [stage] radio.begin = "); Serial.println(st);
    }
    if (n >= 10 && radioUp) {
        uint8_t pl[16]; memset(pl, 0xAA, sizeof(pl));
        int t = radio.transmit(pl, sizeof(pl));
        Serial.print("  TX = "); Serial.println(t);
        radio.clearDio1Action();            // storm fix: detach + NVIC off
        SubGhz.clearPendingInterrupt();
    }
    if (n >= 15 && radioUp) {
        int s = radio.sleep(true);
        Serial.print("  sleep(true) = "); Serial.println(s);
    }

    Serial.println("  zZz"); Serial.flush();
    LowPower.deepSleep(5000);
    Serial.println("  wake");

    if (n >= 15 && radioUp) {
        int w = radio.standby();
        SubGhz.clearPendingInterrupt();
        Serial.print("  standby = "); Serial.println(w);
    }
    Serial.flush();
    n++;
    delay(300);
}
#endif // SLEEP_TEST8
