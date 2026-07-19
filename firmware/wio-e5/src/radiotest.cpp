// ============================================================
//  E5 radio bring-up test. Built only by env:lora_e5_radiotest.
//  Verifies STM32WLx SX1262 init + TX (RF switch PA4/PA5, HP PA 15 dBm).
//  Debug out: CP2102 (USART1 PB6/PB7) @ 9600 (COM9).
// ============================================================
#ifdef RADIO_TEST
#include <Arduino.h>
#include <RadioLib.h>

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

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(500);
    Serial.println("\n=== E5 RADIO TEST ===");
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    int st = radio.begin(868.1, 125.0, 7, 5,
                         RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 15, 8);
    Serial.print("radio.begin() state = "); Serial.println(st);
    if (st != RADIOLIB_ERR_NONE)
        Serial.println(">> RADIO INIT FAILED");
    else
        Serial.println(">> radio OK");
}

void loop() {
    int st = radio.transmit("E5test");
    Serial.print("TX state = "); Serial.print(st);
    Serial.println(st == RADIOLIB_ERR_NONE ? "  (sent)" : "  (err)");
    delay(2000);
}
#endif // RADIO_TEST
