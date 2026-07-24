# Bill of materials — marker-synchronized measurement rig

The complete rig costs under 10 USD (measurement side; the DUTs and camera
are the systems under study, listed separately).

## Measurement rig

| Item | Role | Approx. unit cost (USD) |
|---|---|---|
| INA219 current-sensor breakout | Shunt-based current/voltage monitor in the DUT supply path | 1.50 |
| Wemos D1 Mini (ESP8266) | Logger: streams timestamped current + phase-marker CSV at 500 kbaud | 3.00 |
| Solderless breadboard (half+) | Mounts DUT, gate, and marker wiring | 2.00 |
| Jumper wires + Dupont leads | Supply path, I2C to INA219, marker UART line | 1.00 |
| USB cables (power + logger data) | 5 V supply and host link | (reused) |
| **Rig total** | | **< 10** |

## Camera power gate (per gated node)

| Item | Role | Approx. unit cost (USD) |
|---|---|---|
| FQP30N06L logic-level N-MOSFET | Low-side gate on the camera ground path | 0.60 |
| 10 kΩ resistor | Gate pull-down | 0.02 |

## Devices under study (not part of the rig cost)

| Item | Role |
|---|---|
| Seeed Wio-E5 mini (STM32WLE5) | LoRa DUT (Strategies B/C, deployable E5 node) |
| XIAO ESP32-S3 (Sense / plain) | Wi-Fi DUT (Platform A, deployable A node) |
| ArduCAM Mini 2MP Plus (OV2640) | SPI camera shared by the gated configurations |

## Wiring

See [`wiring.md`](wiring.md) for the pin-level connections (supply path,
INA219 I2C, marker UART, camera SPI/I2C, and the MOSFET gate).
