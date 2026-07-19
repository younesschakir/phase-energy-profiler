// ============================================================
//  Mini 2MP bring-up probe (classic ArduCAM Mini 2MP / OV2640)
//  Built only by env:lora_e5_minitest (-DMINI_PROBE). Excluded elsewhere.
//
//  Wiring on Wio-E5 mini:
//    SPI2 (ArduChip): CS=PB9, MOSI=PA10, MISO=PB14, SCK=PB13
//    I2C  (OV2640):   SDA=PA15, SCL=PB15
//    Power: VCC=3V3, GND=GND     Debug: CP2102 (USART1 PB6/PB7) @ 9600
//
//  Two independent checks:
//    1. SPI: write/read the ArduChip test register 0x00 (0x55/0xAA)
//    2. I2C: read the OV2640 chip ID (bank1: PID 0x0A=0x26, VER 0x0B=0x42)
// ============================================================
#ifdef MINI_PROBE
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

// CS = physical PA0. MUST use the no-underscore Arduino-pin macro `PA0` (=22):
// pinMode/digitalWrite have no PinName overload, so `PA_0` (enum 0) would be
// treated as Arduino D0 = physical PB7. SPI/I2C below keep PinName (PA_10 etc.)
// because setMOSI/setSDA DO have PinName overloads.
#define CAM_CS      PA0
#define SPI_MOSI    PA_10
#define SPI_MISO    PB_14
#define SPI_SCK     PB_13
#define I2C_SDA     PA_15
#define I2C_SCL     PB_15
#define OV2640_ADDR 0x30          // 7-bit SCCB address (0x60/0x61 8-bit)

static void     spiWriteReg(uint8_t addr, uint8_t val);
static uint8_t  spiReadReg(uint8_t addr);
static void     sccbWrite(uint8_t reg, uint8_t val);
static uint8_t  sccbRead(uint8_t reg);

void setup() {
    Serial.setTx(PB_6);
    Serial.setRx(PB_7);
    Serial.begin(9600);
    delay(300);
    Serial.println("\n=== MINI 2MP PROBE ===");

    // SPI2 for the ArduChip
    SPI.setMOSI(SPI_MOSI);
    SPI.setMISO(SPI_MISO);
    SPI.setSCLK(SPI_SCK);
    SPI.begin();
    // Configure CS as a plain GPIO *after* SPI.begin(): PB9 is also SPI2_NSS,
    // and SPI.begin() can grab it back as hardware NSS, killing our manual CS.
    pinMode(CAM_CS, OUTPUT);
    digitalWrite(CAM_CS, HIGH);

    // I2C for the OV2640 sensor
    Wire.setSDA(I2C_SDA);
    Wire.setSCL(I2C_SCL);
    Wire.begin();
}

// Repeats forever so the result can be caught whenever COM9 is opened
// (setup-only output is missed unless a RESET tap lands inside the read window).
void loop() {
    Serial.println("\n=== MINI 2MP PROBE ===");

    // --- CPLD reset: REQUIRED before the SPI test (this was the missing step
    // that made the ArduChip read 0xFF). Proven on the ESP32-S3 with Arducam's
    // official library: write_reg(0x07,0x80); write_reg(0x07,0x00). ---
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    spiWriteReg(0x07, 0x80);
    SPI.endTransaction();
    delay(100);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    spiWriteReg(0x07, 0x00);
    SPI.endTransaction();
    delay(100);

    // --- 1. SPI test: sweep clock speeds (MODE0) on the ArduChip test reg 0x00 ---
    const uint32_t speeds[5] = {125000, 250000, 500000, 1000000, 4000000};
    for (int s = 0; s < 5; s++) {
        SPI.beginTransaction(SPISettings(speeds[s], MSBFIRST, SPI_MODE0));
        spiWriteReg(0x00, 0x55);
        uint8_t r1 = spiReadReg(0x00);
        spiWriteReg(0x00, 0xAA);
        uint8_t r2 = spiReadReg(0x00);
        SPI.endTransaction();
        Serial.print("SPI @");
        Serial.print(speeds[s]);
        Serial.print("Hz: 0x55->0x");
        Serial.print(r1, HEX);
        Serial.print(" 0xAA->0x");
        Serial.print(r2, HEX);
        Serial.println((r1 == 0x55 && r2 == 0xAA) ? "   <== ArduChip OK!" : "");
    }

    // --- 2. I2C test: OV2640 chip ID ---
    sccbWrite(0xFF, 0x01);         // select sensor register bank 1
    delay(2);
    uint8_t pid = sccbRead(0x0A);
    uint8_t ver = sccbRead(0x0B);
    Serial.print("OV2640 PID(0x0A)=0x");
    Serial.print(pid, HEX);
    Serial.print("  VER(0x0B)=0x");
    Serial.println(ver, HEX);
    Serial.println((pid == 0x26) ? "  => OV2640 detected (I2C OK)"
                                 : "  => OV2640 NOT detected");

    Serial.println("=== done ===");
    delay(3000);
}

static void spiWriteReg(uint8_t addr, uint8_t val) {
    digitalWrite(CAM_CS, LOW);
    SPI.transfer(addr | 0x80);     // classic ArduCAM write bit
    SPI.transfer(val);
    digitalWrite(CAM_CS, HIGH);
}
static uint8_t spiReadReg(uint8_t addr) {
    digitalWrite(CAM_CS, LOW);
    SPI.transfer(addr & 0x7F);     // read
    uint8_t v = SPI.transfer(0x00);
    digitalWrite(CAM_CS, HIGH);
    return v;
}
static void sccbWrite(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(OV2640_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}
static uint8_t sccbRead(uint8_t reg) {
    Wire.beginTransmission(OV2640_ADDR);
    Wire.write(reg);
    Wire.endTransmission();         // STOP (OV2640 SCCB read style)
    Wire.requestFrom(OV2640_ADDR, 1);
    return Wire.available() ? Wire.read() : 0xEE;
}
#endif  // MINI_PROBE
