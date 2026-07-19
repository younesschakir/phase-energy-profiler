// ============================================================
//  STOP2 + radio: CANDIDATE FIX SET  (env:lora_e5_sleeptest7)
//
//  sleeptest6 proved radio.begin()+sleep(true) alone breaks the HONEST
//  (no-debugger) STOP2 wake (bare STOP2 cycles fine). Candidates applied
//  here, all together (bisect down later if it works):
//    1. SubGhz.disableInterrupt() + clearPendingInterrupt() before STOP2
//       (a pending/enabled SUBGHZ IRQ can hardfault the clock-restore
//        wake path -> looks like a freeze)
//    2. LL_C2_PWR_SetPowerMode(LL_PWR_MODE_SHUTDOWN): the radio acts as a
//       second power domain (C2) that the Arduino core NEVER configures;
//       ST's own stack sets it before low-power entry (RAK forum lead).
//       May cost the radio's warm-sleep retention -> standby-fail path
//       re-begins (acceptable: outcome B).
//    3. Clear + disable the RF-busy wakeup (PWR SCR/CR3).
//  After wake: re-enable SUBGHZ IRQ -> radio.standby() -> re-begin if dead
//  -> TX to prove the radio, every cycle.
//
//  PROTOCOL: flash -> FULL POWER-CYCLE (debugger sets DBG_STOP until POR)
//  -> watch COM9 @9600. Cycling TICKs + TX=0 = the fix works.
// ============================================================
#ifdef SLEEP_TEST7
#include <Arduino.h>
#include <RadioLib.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>
#include <SubGhz.h>
#include "stm32wlxx_ll_pwr.h"

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
    Serial.println("\n=== SLEEP TEST 7: STOP2 + radio + C2/IRQ fix set ===");
    int st = radioStart();
    Serial.print("radio.begin() = "); Serial.println(st);
    rtc.setClockSource(STM32RTC::LSI_CLOCK);
    rtc.begin();
    LowPower.begin();
    Serial.println("cycling: TX -> warm-sleep -> fixes -> STOP2(8s)");
    Serial.flush();
}

uint32_t n = 0;
void loop() {
    Serial.print("TICK "); Serial.println(n++);

    uint8_t pl[16]; memset(pl, 0xAA, sizeof(pl));
    int t = radio.transmit(pl, sizeof(pl));
    Serial.print("  TX = "); Serial.println(t);

    // ROOT CAUSE (proven by halting the frozen chip + NVIC dump: SUBGHZ
    // IRQ 50 was RE-ENABLED): the SUBGHZ IRQ is LEVEL-triggered, and
    // RadioLib's STM32WLx::clearIrqStatus() re-enables it in NVIC whenever
    // a Dio1 callback is still attached. Once the radio sleeps its IRQ line
    // can never be cleared -> interrupt storm -> freeze.
    // FIX: fully DETACH the callback (also NVIC-disables) before sleeping;
    // never re-enable manually — the next transmit() re-attaches itself.
    radio.clearDio1Action();                      // detach + NVIC disable
    SubGhz.clearPendingInterrupt();

    int s = radio.sleep(true);                    // warm sleep (retention)
    Serial.print("  sleep(true) = "); Serial.println(s);
    SET_BIT(PWR->SCR, PWR_SCR_CWRFBUSYF);         // clear RF-busy wake flag
    CLEAR_BIT(PWR->CR3, PWR_CR3_EWRFBUSY);        // no wake on RF-busy

    Serial.print("  ISER1 pre-sleep = 0x");       // bit18 (IRQ50) must be 0
    Serial.println(NVIC->ISER[1], HEX);
    Serial.println("  SLEEP>> (8s)"); Serial.flush();
    LowPower.deepSleep(8000);
    Serial.println("  <<WAKE"); Serial.flush();

    int w = radio.standby();                      // wake radio (BUSY falls)
    SubGhz.clearPendingInterrupt();               // drop any wake glitch
    Serial.print("  ISER1 post-wake = 0x");
    Serial.println(NVIC->ISER[1], HEX);
    Serial.print("  standby = "); Serial.println(w);
    if (w != RADIOLIB_ERR_NONE) {
        int rb = radioStart();                    // retention lost -> re-init
        Serial.print("  re-begin = "); Serial.println(rb);
    }
    Serial.flush();
    delay(300);
}
#endif // SLEEP_TEST7
