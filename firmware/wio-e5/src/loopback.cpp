// ============================================================
//  E5 SPI2 loopback self-test.  Built only by env:lora_e5_loopback.
//  Jumper MOSI (PA10) directly to MISO (PB14) at the E5 header.
//  A working SPI2 echoes every byte it sends.
//    Debug out: CP2102 (USART1 PB6/PB7) @ 9600  (COM9)
// ============================================================
#ifdef LOOPBACK
#include <Arduino.h>
#include <SPI.h>

#define SPI_MOSI  PA_10
#define SPI_MISO  PB_14
#define SPI_SCK   PB_13

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(300);
    SPI.setMOSI(SPI_MOSI);
    SPI.setMISO(SPI_MISO);
    SPI.setSCLK(SPI_SCK);
    SPI.begin();
}

void loop() {
    const uint8_t tx[6] = {0x55, 0xAA, 0x00, 0xFF, 0x3C, 0x81};
    int ok = 0;
    Serial.println("\n=== SPI2 LOOPBACK (jumper PA10<->PB14) ===");
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 6; i++) {
        uint8_t r = SPI.transfer(tx[i]);
        Serial.print("  sent 0x"); Serial.print(tx[i], HEX);
        Serial.print(" -> got 0x"); Serial.print(r, HEX);
        if (r == tx[i]) { Serial.println("  ok"); ok++; }
        else            { Serial.println("  MISMATCH"); }
    }
    SPI.endTransaction();
    Serial.print(ok == 6 ? ">> SPI2 TRANSPORT OK (6/6)"
                         : ">> SPI2 TRANSPORT FAIL ");
    if (ok != 6) { Serial.print(ok); Serial.print("/6"); }
    Serial.println();
    delay(3000);
}
#endif // LOOPBACK
