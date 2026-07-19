// ============================================================
//  STOP2 + radio, COLD-BOOT ROBUST  (env:lora_e5_sleeptest9)
//
//  Fixes the cold-power-on freeze (ST-Link reset masked it):
//    A) __HAL_RCC_BACKUPRESET force/release at boot -> clears any corrupt
//       RTC/backup-domain clock selection (e.g. from the multimeter short)
//       that survives resets and only clears on true power-off.
//    B) radio.begin() FIRST -> powers VDD_TCXO -> HSE32 available before any
//       deepSleep (HSE is sourced from the radio's TCXO; without it the
//       STOP2 wake clock-restore hangs).
//  Then the proven per-cycle sequence (TX -> warm sleep -> STOP2 -> standby).
//
//  PROTOCOL: flash -> FULL POWER-CYCLE -> PuTTY COM9 @9600. Cycling TICKs on
//  a COLD boot = fixed. Measure uA during "SLEEP>>".
// ============================================================
#ifdef SLEEP_TEST9
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
    Serial.println("\n=== SLEEP TEST 9: cold-boot robust STOP2 ===");

    // (B) Radio FIRST -> TCXO/HSE up before any deepSleep.
    int st = radioStart();
    Serial.print("radio.begin = "); Serial.println(st);

    // (A) begin(true) -> resetBackupDomain(): clears a corrupt RTC/backup
    // clock selection (e.g. from the multimeter short) that survives resets.
    rtc.setClockSource(STM32RTC::LSI_CLOCK);
    rtc.begin(true);
    LowPower.begin();
    Serial.println("backup domain reset + RTC(LSI)");
    Serial.println("cycling on COLD boot?");
    Serial.flush();
}

uint32_t n = 0;
void loop() {
    Serial.print("TICK "); Serial.println(n++);

    uint8_t pl[16]; memset(pl, 0xAA, sizeof(pl));
    int t = radio.transmit(pl, sizeof(pl));
    Serial.print("  TX = "); Serial.println(t);

    radio.clearDio1Action();            // detach level-IRQ (storm fix)
    SubGhz.clearPendingInterrupt();
    int s = radio.sleep(true);
    Serial.print("  sleep(true) = "); Serial.println(s);

    Serial.println("  SLEEP>> (8s, measure uA)"); Serial.flush();
    LowPower.deepSleep(8000);
    Serial.println("  <<WAKE");

    int w = radio.standby();
    SubGhz.clearPendingInterrupt();
    Serial.print("  standby = "); Serial.println(w);
    if (w != RADIOLIB_ERR_NONE) {
        int rb = radioStart();
        Serial.print("  re-begin = "); Serial.println(rb);
    }
    Serial.flush();
    delay(300);
}
#endif // SLEEP_TEST9
