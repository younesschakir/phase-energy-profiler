# Phase-Decomposed Energy Measurement of Camera-Based Water-Meter Reading Nodes

Firmware, measurement tooling, analysis scripts, and per-cycle datasets for the paper:

> Y. Chakir, A. Aaroud, *Phase-decomposed energy measurement of camera-based
> water-meter reading nodes: model correction, hardware limits, and a measured
> basis for multi-year autonomy* (under review).

The companion modeling study and systematic review are:
- Y. Chakir, A. Aaroud, "Feasibility analysis of ultra-low-power image transmission over LoRaWAN for analog water meter reading," IEEE, 2026.
- Y. Chakir, A. Aaroud, "The autonomy–accuracy–universality trilemma in camera-based water meter reading," *Array* 30 (2026) 100960.

## What this is

A sub-$10, reproducible rig and method for attributing every millijoule of an
embedded camera node's reading cycle to its phase (wake, capture, processing,
transmission, reception, sleep entry). The device under test injects UART phase
markers directly into a shunt-based current log, so segmentation needs no
waveform heuristics. Measured here on six configurations across two MCU
platforms (ESP32-S3 / STM32WLE5), two radios (Wi-Fi / LoRa), and two
power-management regimes (as-built / camera-gated + retentive deep sleep).

## Repository layout

```
firmware/
  esp32s3/         XIAO ESP32-S3 Sense — Strategy A + deployable A node
                   (PlatformIO; envs: c1, c1_jomjol, node_a, ...)
  wio-e5/          Wio-E5 mini (STM32WLE5) — Strategies B/C + E5 node
                   (envs: lora_e5, lora_e5_stratB, lora_e5_node, ...)
  logger-d1mini/   Wemos D1 Mini + INA219 logger (500 kBd CSV stream)
analysis/
  capture_run.py   captures an N-cycle run from the logger into samples/phases CSVs
  gen_figures.py   regenerates every figure in the paper from the CSVs
  jpegdump.py      reconstructs & size-checks JPEGs dumped over the debug UART
  data/            per-cycle phase datasets (phases_*.csv) + summary tables
                   + capture-evidence images
docs/
  ov2640_register_sweep.md   the windowing register exploration (supplementary)
  wiring.md                  rig + camera-gate wiring
```

Full raw current waveforms (`samples_*.csv`, ~110 MB uncompressed) are in the
Zenodo deposit as `raw_waveforms_v1.0-paper.zip`, not in this repository.

### How the paper's payload sizes relate to this data

The packet counts and byte figures in Table 2 of the paper are **derived from
each configuration's measured transmit airtime** (the `dur_ms` of phase 4 in
`phases_*.csv`) using the standard LoRa airtime formulation, not read from the
firmware. Two consequences worth knowing before comparing numbers:

* Byte figures carry the granularity of one 222-byte fragment, so e.g. the E5
  node's "~1.4 kB / 7 packets" means 1333–1554 B.
* The firmware also logs the frame-buffer length it read from the camera. For
  the un-gated strategies that length is the ArduCAM's 512-byte-page-rounded
  value (it includes padding that is transmitted), while the gated firmware
  truncates at the JPEG end-of-image marker. A firmware-reported length will
  therefore not always equal the airtime-derived payload; where the paper
  reports directly measured JPEG bytes (Figure 4 and Section 5.3), it says so.

Per-phase energies, currents and durations in the paper's Tables 3 and 5 come
straight from `phases_*.csv`, so any of them can be checked against
`V x I x t` with the values printed in those tables.

## Reproducing a measurement

1. **Rig**: 5 V USB source → INA219 (shunt in supply path) → DUT 5 V pin.
   Common ground everywhere. DUT marker UART → logger D5. See `docs/wiring.md`.
2. **Logger**: flash `firmware/logger-d1mini`; it streams
   `ts,mA,V,mW,phase` CSV at 500 kBd.
3. **DUT**: set your Wi-Fi credentials in the ESP32-S3 sources
   (`MY_WIFI_SSID` / `MY_WIFI_PASSWORD`), then flash the desired PlatformIO env,
   e.g. `pio run -e node_a -t upload`.
4. **Capture**: `python analysis/capture_run.py <outdir> <cycles> <timeout_s> <label> [COMport]`.
5. **Figures**: `python analysis/gen_figures.py` (reads `analysis/data/`).

Measurement pitfalls to respect (Section 4.3 of the paper): disconnect the
debug probe and power-cycle before any sleep measurement (debug bits fake
STOP2); re-init the marker UART after deep sleep; park gated-camera pins
high-impedance, never driven low.

## Third-party components

- `firmware/wio-e5/lib/ArduCAM` and `lib/Arducam_Mega` are vendored copies of
  ArduCAM's Arduino libraries with two documented STM32 portability patches
  (SPI transactions in `bus_write`/`bus_read`; `ARDUINO_ARCH_STM32` in
  `Platform.h`). Original license headers are preserved.
- `firmware/esp32s3/models/dig-class100.tflite` is the digit-classification
  model from the [AI-on-the-edge-device](https://github.com/jomjol/AI-on-the-edge-device)
  project (jomjol), redistributed with attribution; see that repository for
  its license terms.
- TensorFlow Lite Micro + ESP-NN are pulled by PlatformIO
  (`nickjgniklu/ESP_TF`), RadioLib v6.6.0 by the Wio-E5 envs.

## License

- Code (firmware, scripts): MIT — see `LICENSE`.
- Datasets (`analysis/data/`, Zenodo deposit): CC BY 4.0.

## Citation

See `CITATION.cff`, or cite the paper above.
