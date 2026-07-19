// ============================================================
//  STOP2 + radio WARM-SLEEP test  (env:lora_e5_sleeptest5)
//
//  The proper STM32WL low-power sequence (vs the old cold-sleep attempts):
//    radio.begin FIRST -> RTC(LSI)/LowPower -> each cycle:
//      TX (verify radio alive) -> radio.sleep(true) WARM/retain
//      -> STOP2 deepSleep (RTC wake) -> radio.standby() to recover
//      -> if standby fails (-707), radio.begin() again (retention lost).
//
//  Goal = confirm the FULL system can reach uA in STOP2 while the radio
//  survives (or cheaply re-inits) across sleep -> the multi-year-autonomy
//  make-or-break. Measure current with a uA-capable meter (multimeter in
//  series / VCC path) DURING the 8s deepSleep window (watch for the "SLEEP>>"
//  print, then the "<<WAKE" print).
//
//  Radio-survives outcomes:
//    A) post-wake standby=0 AND next-loop TX=0  -> retention works across STOP2
//    B) standby!=0, re-begin=0, next TX=0        -> STOP2 wipes radio; re-init
//                                                   each wake (costs a few mJ,
//                                                   still fine for autonomy)
//    C) TX still fails                            -> deeper HSE/TCXO issue ->
//                                                   move node fw to STM32CubeWL
//  Debug out: CP2102 (PB6/PB7) @ 9600 (COM9).
// ============================================================
#ifdef SLEEP_TEST5
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

static int radioStart() {
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    return radio.begin(868.1, 125.0, 7, 5,
                       RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 15, 8);
}

void setup() {
    Serial.setTx(PB_6); Serial.setRx(PB_7); Serial.begin(9600); delay(500);
    Serial.println("\n=== SLEEP TEST 5: STOP2 + radio warm-sleep ===");

    // Radio FIRST (sets up HSE/TCXO for SUBGHZ) before RTC/LowPower touch clocks.
    int st = radioStart();
    Serial.print("radio.begin() = "); Serial.println(st);

    rtc.setClockSource(STM32RTC::LSI_CLOCK);   // Wio-E5 has no LSE crystal
    rtc.begin();
    LowPower.begin();
    Serial.println("init done; cycling TX -> warm-sleep -> STOP2(8s)");
    Serial.flush();
}

uint32_t n = 0;
void loop() {
    Serial.print("TICK "); Serial.println(n++);

    // 1) TX — confirms the radio is alive this cycle.
    uint8_t pl[16]; memset(pl, 0xAA, sizeof(pl));
    int t = radio.transmit(pl, sizeof(pl));
    Serial.print("  TX state = "); Serial.println(t);

    // 2) WARM sleep the radio (config retained in radio RAM, ~uA).
    int s = radio.sleep(true);
    Serial.print("  radio.sleep(true) = "); Serial.println(s);

    // 3) STOP2 — measure uA in this window.
    Serial.println("  SLEEP>> (measure uA now, 8s)"); Serial.flush();
    LowPower.deepSleep(8000);
    Serial.println("  <<WAKE"); Serial.flush();

    // 4) Recover the radio from warm sleep.
    int w = radio.standby();
    Serial.print("  post-wake standby = "); Serial.println(w);
    if (w != RADIOLIB_ERR_NONE) {
        int rb = radioStart();                 // retention lost -> full re-init
        Serial.print("  re-begin = "); Serial.println(rb);
    }
    Serial.flush();
}
#endif // SLEEP_TEST5
