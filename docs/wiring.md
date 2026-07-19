# Rig and node wiring

## Measurement rig (all configurations)

```
5 V USB source ──> INA219 VIN+ ──[shunt]── VIN- ──> DUT 5 V pin
DUT GND ── logger GND ── source GND          (one common ground, mandatory)
DUT marker UART TX ──> logger D5 (GPIO14)     9600 Bd, bytes 0xA1..0xA6 / 0xA0
Logger (D1 Mini) USB ──> host PC              streams ts,mA,V,mW,phase @ 500 kBd
```

Rules that cost us debugging days — respect them:
- ALL DUT current must flow through the shunt: unplug the DUT's own USB and
  any debug probe during measurement runs, or current reads ~0.
- Disconnect the SWD probe AND power-cycle before any sleep measurement:
  an attached debugger sets DBG bits that keep clocks running in STOP2
  (we measured 34 mA "sleep" that was really 5.5 mA).
- Deep sleep powers down the marker UART: firmware must re-init it after
  each wake (already done in the node firmware).

## Marker pins

| Platform | Marker TX | Debug/text UART |
|---|---|---|
| XIAO ESP32-S3 | GPIO43 (Serial1) | native USB CDC |
| Wio-E5 mini | PA9 (USART1) | PB6/PB7 -> onboard CP2102 (measurement builds use PA9) |

## ArduCAM Mini 2MP Plus wiring

| Camera pin | XIAO ESP32-S3 (A node) | Wio-E5 mini (B/C/nodes) |
|---|---|---|
| CS   | D3 / GPIO4 | PA0 |
| MOSI | D10 / GPIO9 | PA10 (SPI2) |
| MISO | D9 / GPIO8 | PB14 (SPI2) |
| SCK  | D8 / GPIO7 | PB13 (SPI2) |
| SDA  | D4 / GPIO5 | PA15 (I2C) |
| SCL  | D5 / GPIO6 | PB15 (I2C) |
| VCC  | 3V3 | 3V3 |
| GND  | MOSFET drain (gated nodes) or GND | MOSFET drain (gated nodes) or GND |

## Camera power gate (deployable nodes)

Low-side N-channel MOSFET (FQP30N06L, TO-220, front view pins G-D-S):

```
Camera GND wire ──> Drain (middle pin / tab)
Source ──> real GND rail
Gate ──> gate GPIO  +  10 kOhm resistor Gate->GND (keeps camera OFF at boot/sleep)
```

| Platform | Gate GPIO |
|---|---|
| XIAO ESP32-S3 (node_a) | D1 / GPIO2 |
| Wio-E5 mini (node) | PB9 |

Firmware parks all camera-facing pins high-impedance while gated; driving them
low instead re-introduces ~20 mA of phantom feed through the module's I/O
clamps. Only the camera's GND wire goes through the FET — never the system
ground or the shunt path.

## STM32duino pin-name rule (Wio-E5)

`SPI`/`Wire`/`Serial` setters take PinName constants (`PA_10`);
`pinMode`/`digitalWrite`/ArduCAM CS take the no-underscore Arduino macros
(`PA0`). Mixing them silently addresses the wrong physical pin.
