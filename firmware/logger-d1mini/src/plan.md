Paper 3 — Measurement Procedure, Expected Outputs & Q1 Paper Strategy
Context
All firmware and Python analysis scripts are now implemented. This plan covers:

Code changes to fix before measuring
Pre-measurement pipeline validation (synthetic data)
Hardware measurement procedure for both platforms
Expected outputs (tables, figures, numbers)
Paper structure for a Q1 journal submission
Part 0 — Code Changes to Make Before Measuring
Three issues were found during review:

0.1 — figures.py: Wrong --cycles-per-day default (MUST FIX)
File: analysis/figures.py line 391

# CURRENT (wrong for AMR):
--cycles-per-day, default=48    # every 30 minutes — industrial sensor, not water meter

# FIX:
--cycles-per-day, default=24    # hourly reading — target of this system (supervisor confirmed)
Why this matters: The autonomy figure directly cites these numbers. At 24 readings/day:

Platform C (~578 mJ/cycle): 578 × 24 = 13,872 mJ/day → 2000 mAh LiPo (3.7V) → 5.3 years
Platform A (~802 mJ/cycle): 802 × 24 = 19,248 mJ/day → same battery → 3.8 years
Both platforms achieve multi-year autonomy at hourly readings — a strong result for the paper.
Default of 48 (every 30 min) would cut autonomy in half and misrepresent the system.

Also add a second reference line: besides the existing 1-year and 5-year lines, add a dashed line at years = (cap × 3.7 × 3600) / (E_mJ × 2 × 365) as a "2×/day" (every 12h) comparison scenario, so reviewers can see the trade-off.

0.2 — energy_stats.py: Add systematic error note
The INA219 has ±0.5% accuracy and the shunt resistor has ±1% tolerance (typical for 0.1 Ω metal film). Total systematic error: ±1.5%. This should be printed in the output:

Note: ±1.5% systematic error from INA219 accuracy + shunt tolerance
      not included in 95% CI (CI reflects only inter-cycle variability).
Add this after the "Positive = measured higher..." note in pretty_print().

0.3 — dut_esp32s3: WiFi credentials (MUST DO before flashing)
File: dut_esp32s3/src/main.cpp lines 99–100

#define MY_WIFI_SSID     "YOUR_SSID"       // ← replace with real SSID
#define MY_WIFI_PASSWORD "YOUR_PASSWORD"   // ← replace with real password
Replace YOUR_SSID and YOUR_PASSWORD before pio run -e c1 -t upload.

Part 1 — Pre-Measurement Pipeline Validation (Synthetic Data)
Run this before touching any hardware. Validates that all Python scripts work correctly.

cd workspace/analysis

# Step 1: Generate synthetic logs
python synthetic_data.py --platform A --cycles 30 --seed 42 --out synthetic_A.txt
python synthetic_data.py --platform C --cycles 30 --seed 42 --out synthetic_C.txt

# Step 2: Parse logs to CSVs
python parse_logs.py --input synthetic_A.txt --out-samples samples_A.csv --out-phases phases_A.csv
python parse_logs.py --input synthetic_C.txt --out-samples samples_C.csv --out-phases phases_C.csv

# Step 3: Statistics + Paper 2 comparison
python energy_stats.py --phases phases_A.csv --platform A --out-stats stats_A.csv
python energy_stats.py --phases phases_C.csv --platform C --out-stats stats_C.csv

# Step 4: Figures (all three)
python figures.py \
    --samples samples_A.csv --phases phases_A.csv --platform A \
    --phases-b phases_C.csv --platform-b C \
    --out-dir figures_synthetic/ --cycles-per-day 24
What to verify:

phases_A.csv → 210 rows (7 phases × 30 cycles), phases_C.csv → 180 rows (6 × 30)
stats_A.csv → 7 rows; Err % column near 0% (within ±5% noise)
figures_synthetic/fig1_waveform_platformA.png — waveform with 7 coloured phase bands
figures_synthetic/fig2_energy_breakdown.png — two stacked bars (A vs C), Paper 2 dashed lines
figures_synthetic/fig3_autonomy_platformA.png — two lines crossing 1-year mark
If all four steps pass without errors and figures look correct → pipeline is ready for real data.

Part 2 — Hardware Measurement Procedure
2.1 — Wiring for Both Platforms
INA219 placement (identical for both platforms):

[Power source USB cable]
  │
  ├─ VBUS ────────────────────────── INA219 VIN+
  │                                       │
  │                                   INA219 VIN-
  │                                       │
  └─ D+/D- ─── [cut these two wires] ─── DUT USB connector
                                          DUT 5V internal rail
INA219 SDA ─── D1 Mini D2 (GPIO4)
INA219 SCL ─── D1 Mini D1 (GPIO5)
INA219 VCC ─── D1 Mini 3.3V
INA219 GND ─── GND (common with DUT)
DUT UART TX ─── D1 Mini D5 (GPIO14)   ← phase markers
  Platform A: ESP32-S3 GPIO43 (UART1 TX)
  Platform C: LoRa-E5  PA9   (UART1 TX)
D1 Mini USB ─── PC (logger output)  ← intact, full cable
Shunt resistor:

Run A (active phases): 0.1 Ω, rated ≥ 1W, ±1% tolerance metal film
Run B (sleep current): 10 Ω, rated ≥ 0.25W, ±1% tolerance
2.2 — Run A: Active Phase Measurement (Both Platforms)
This captures all active phases at sufficient resolution (0.1 mA LSB).

Platform A — ESP32-S3 + WiFi + CNN
Edit dut_esp32s3/src/main.cpp: set MY_WIFI_SSID, MY_WIFI_PASSWORD
Verify logger_d1mini/platformio.ini has SHUNT_CONFIG=0
Wire 0.1 Ω shunt between INA219 VIN+ and VIN-
Flash logger: cd logger_d1mini && pio run -e logger -t upload
Open logger monitor to verify: pio device monitor -e logger
Must see: # INA219 OK, # Shunt config: Run A -- 0.1 ohm
CSV lines should appear at >800/second rate
Close monitor (Ctrl+C)
Flash ESP32-S3: cd dut_esp32s3 && pio run -e c1 -t upload
Capture 30 cycles to file:
cd logger_d1mini
pio device monitor -e logger > ../data/raw_A_active.txt
Wait for 30 cycles to complete (ESP32-S3 auto-stops, logger keeps running)
Duration estimate: 30 × (50+300+100+50+1200+800+30000) ms ≈ 30 × 32.5 s ≈ 16 minutes
Ctrl+C the monitor when the last cycle summary appears
Verify file: check it contains PHASE_CSV,30,... (last cycle 30)
Platform C — LoRa-E5 + LoRaWAN
Verify logger_d1mini/platformio.ini still has SHUNT_CONFIG=0
Wire the same 0.1 Ω shunt (now between INA219 and LoRa-E5 5V)
Reconnect logger UART RX to LoRa-E5 PA9 (TX)
Flash LoRa-E5: cd lora_e5 && pio run -e lora_e5 -t upload
Capture 30 cycles:
cd logger_d1mini
pio device monitor -e logger > ../data/raw_C_active.txt
Duration estimate: 30 × (5+330+50+1640+100+30000) ms ≈ 30 × 32.1 s ≈ 16 minutes
2.3 — Run B: Sleep Current Measurement (Separate)
Sleep current on both platforms is in the microampere range (ESP32-S3 deep sleep ≈ 10–20 µA, STM32WLE5 STOP2 ≈ 2 µA). The 32V/2A range has a 0.1 mA LSB — too coarse to resolve µA currents.

Procedure for Run B:

Swap shunt resistor: 0.1 Ω → 10 Ω
Change logger_d1mini/platformio.ini: SHUNT_CONFIG=1
Reflash logger: pio run -e logger -t upload
Verify: # Shunt config: Run B -- 10 ohm (sleep only, 16V/400mA range)
For Platform A: the ESP32-S3 spends P7 in deep sleep (~30 s per cycle)
Capture a 5-cycle run is sufficient for sleep current
pio device monitor -e logger > ../data/raw_A_sleep.txt
For Platform C: LoRa-E5 spends P6 in STOP2 mode
Same procedure: ../data/raw_C_sleep.txt
Why Run B matters for the paper:

Deep sleep dominates the duty cycle for infrequent readings (30 s sleep per 2 s active)
The sleep energy per day can exceed active energy when readings are rare
Accurate µA measurement is required to compute accurate yearly energy budget
Note: Run B sleep measurements are for long-term autonomy accuracy only. The active-phase energy dominates the per-cycle total in the Paper 2 comparison table.

2.4 — Quality Checks During Measurement
After each platform, verify before stopping:

# CYCLE 30 appears in logger output
Total energy printed per cycle is within ±20% of Paper 2 predicted total
Platform A: expect ~700–900 mJ/cycle
Platform C: expect ~500–650 mJ/cycle
No WARNING: X cycle(s) have fewer than N phases in parse_logs.py output
Peak current during P5 (CNN, Platform A) should hit 150–200 mA
Peak current during P4 (LoRa TX, Platform C) should hit 80–130 mA
Part 3 — Analysis Pipeline (Real Data)
cd workspace/analysis

# Parse raw logs
python parse_logs.py --input ../data/raw_A_active.txt \
    --out-samples samples_A.csv --out-phases phases_A.csv

python parse_logs.py --input ../data/raw_C_active.txt \
    --out-samples samples_C.csv --out-phases phases_C.csv

# Statistics tables
python energy_stats.py --phases phases_A.csv --platform A --out-stats stats_A.csv
python energy_stats.py --phases phases_C.csv --platform C --out-stats stats_C.csv

# All figures
python figures.py \
    --samples samples_A.csv --phases phases_A.csv --platform A \
    --phases-b phases_C.csv --platform-b C \
    --cycles-per-day 24 --out-dir figures/

# Also generate Platform C waveform (Fig 1 for Platform C separately)
python figures.py \
    --samples samples_C.csv --phases phases_C.csv --platform C \
    --cycles-per-day 24 --out-dir figures/
Part 4 — Expected Outputs (Realistic Numbers)
Table I — Per-Phase Energy Platform A (30 cycles, ±5% noise around Paper 2)
Ph	Phase	Mean (mJ)	Std	95% CI	Paper 2 (mJ)	Error %
P1	Wake-up	6.6	0.3	[6.4, 6.8]	6.6	~0%
P2	Cam Init	49.5	2.5	[48.6, 50.4]	49.5	~0%
P3	Capture	9.9	0.5	[9.7, 10.1]	9.9	~0%
P4	Preprocess	13.2	0.7	[12.9, 13.5]	13.2	~0%
P5	CNN Infer.	712.8	35.6	[699.5, 726.1]	712.8	~0%
P6	WiFi TX	600.0	30.0	[588.9, 611.1]	N/A	N/A
P7	Sleep	~0	—	—	0.0	N/A
TOTAL	—	~1392			801.7	+74%
Note: Total will be higher than Paper 2 because WiFi TX (P6) was NOT in Paper 2 model.
The Paper 2 total (801.7 mJ) excluded WiFi TX by design. Your measurement includes it.
The correct comparison: exclude P6 from your measured total → should be within ±10% of 801.7 mJ.

Table II — Per-Phase Energy Platform C (30 cycles)
Ph	Phase	Mean (mJ)	Std	95% CI	Paper 2 (mJ)	Error %
P1	Wake-up	0.17	0.01	[0.16, 0.18]	0.17	~0%
P2	Capture	50.4	2.5	[49.5, 51.3]	50.4	~0%
P3	Preprocess	13.2	0.7	[12.9, 13.5]	13.2	~0%
P4	LoRa TX	639.0	32.0	[627.1, 650.9]	639.0	~0%
P5	RX Window	4.95	0.2	[4.88, 5.02]	4.95	~0%
P6	Sleep	~0	—	—	0.0	N/A
TOTAL	—	~708			577.9	+22%
Note: Totals from synthetic data will be near Paper 2 by construction (±5% noise).
Real hardware measurements will differ — that's the scientific contribution.
Expect real Platform C to differ by ±10–30% from Paper 2 due to actual vs datasheet specs.

Figures Summary
Figure	File	Content
Fig 1a	fig1_waveform_platformA.png	Current waveform 1 representative cycle, 7 phase bands
Fig 1b	fig1_waveform_platformC.png	Current waveform Platform C, 6 phase bands
Fig 2	fig2_energy_breakdown.png	Stacked bars A vs C, Paper 2 dashed totals, error bars
Fig 3a	fig3_autonomy_platformA.png	Autonomy days vs capacity (500–5000 mAh), measured vs Paper 2
Fig 3b	fig3_autonomy_platformC.png	Same for Platform C
Part 5 — Paper Structure for Q1 Journal
Target Journal (Recommended)
Primary target: Measurement (Elsevier, Q1, IF ≈ 5.7, ISSN 0263-2241)

Scope: Measurement systems, instrumentation, data processing
Perfect fit: INA219 instrumentation system + statistical energy profiling methodology
Typical length: 8–12 pages, 3–5 figures, 2–4 tables
Processing time: ~8 weeks to first decision
Alternative: IEEE Sensors Journal (Q1, IF ≈ 4.3)

Scope: Sensor systems, IoT devices
Good fit if you emphasize the INA219 measurement system as a contribution
Avoid (for now): IEEE IoT Journal (very competitive, IF ~10 — better for Paper 4)

Paper Title (Draft)
"Phase-Decomposed Experimental Energy Profiling of Camera-Based AMR over LoRaWAN:
Validation of Datasheet-Predicted Models via INA219 Instrumentation"

Abstract Structure (150 words)
Problem: battery lifetime of IoT AMR systems hard to predict accurately with datasheet models
Gap: existing models (Paper 2) not experimentally validated at phase level
Method: INA219-based instrumentation with UART phase markers, 30-cycle statistical analysis
Result: per-phase energy measured on two platforms, 95% CI, compared to Paper 2 predictions
Finding: Phase X dominates (Y%) with Z% deviation from predictions
Implication: model validated / corrected, optimization target identified
Recommended Section Structure
I. Introduction (1 page)

AMR market growth + battery life challenge
Datasheet models common but untested experimentally
Contribution list:
Phase-decomposed energy profiling methodology (INA219 + UART markers)
Experimental validation of Paper 2 datasheet models on two IoT platforms
95% CI per-phase statistics over 30 measurement cycles
Identification of dominant energy phases and optimization targets
II. Background and Related Work (1 page)

Existing IoT energy profiling techniques
LoRaWAN AMR energy literature
Why phase-level granularity is novel vs existing work
Cite Paper 2 as the model being validated
III. System Architecture (1 page)

Platform A: ESP32-S3 + OV2640 + CNN + WiFi
Platform C: LoRa-E5 + ArduCam + LoRaWAN
Phase structure (7-phase and 6-phase cycles)
1/3-page figure: block diagram of measurement system
IV. Measurement Methodology (1.5 pages)

INA219 instrumentation (dual-range, shunt selection)
Phase synchronization via UART markers
30-cycle repetition for statistical validity
Energy integration formula: E = ∫I(t)·V(t)·dt
Statistical analysis: mean, std, 95% CI via Student's t
Uncertainty budget: ±0.5% INA219 + ±1% shunt = ±1.5% systematic
V. Experimental Results (2.5 pages)

V.A: Platform A per-phase results (Table I + Fig 1a waveform)
V.B: Platform C per-phase results (Table II + Fig 1b waveform)
V.C: Comparison with Paper 2 predictions (Fig 2 stacked bar)
V.D: Battery autonomy projection (Fig 3)
VI. Discussion (1 page)

Which phases dominate (CNN: 89% of Platform A active energy)
Which phases match predictions vs deviate (and why — real JPEG size vs estimate)
Platform comparison: C is X% more energy efficient than A
Optimization recommendations: CNN compression, LoRa SF reduction, camera duty cycle
Limitations: single temperature point, no aging model, synthetic WiFi traffic
VII. Conclusion (0.5 page)

Methodology validated, framework reusable for any phase-structured IoT device
Cite future work (Paper 4): multi-node AMR network energy modeling
Key Arguments for Novelty (Reviewers Will Ask)
Novelty vs Paper 2 (same author):
Paper 2 = analytical prediction from datasheets
Paper 3 = experimental measurement with statistical rigor
They are complementary: Paper 3 proves Paper 2's model is valid (or corrects it)
This is the standard experimental validation paper pattern (model → measurement → correction)
Novelty vs existing IoT energy profiling:
Most papers measure total energy, not per-phase
UART marker synchronization is lightweight (no RTOS hooks, no power rail cuts)
Dual-range shunt (0.1 Ω / 10 Ω) covers 5 decades of current (µA to 500 mA)
30 cycles + 95% CI is more statistically rigorous than typical 3–5 measurement papers
Practical contribution:
INA219 + D1 Mini logger is ~€8 total hardware
Anyone can replicate this setup
The Python pipeline is open-source and format-documented
Framework generalizes beyond AMR to any IoT device with phase-structured workloads
Three Key Quantitative Claims (Must be in Abstract and Conclusion)
These will be the numbers reviewers check. Fill in with real measured values:

"Platform C consumes X mJ/cycle, representing a Y% reduction vs Platform A (Z mJ/cycle)"
"Measured values deviate from Paper 2 datasheet predictions by A–B% (excluding WiFi TX phase)"
"At 24 readings/day (hourly), Platform C achieves N years autonomy on a 2000 mAh 3.7V LiPo battery (vs M years for Platform A)"
Part 6 — Verification Checklist Before Submission
not done
30 complete cycles for Platform A (phases_A.csv has 210 rows)
not done
30 complete cycles for Platform C (phases_C.csv has 180 rows)
not done
No cycles missing any phases (check parse_logs.py warnings)
not done
Run B sleep current measured for both platforms
not done
stats_A.csv: all 95% CI intervals are finite (n=30 in each phase)
not done
Fig 1: phase labels visible, time axis in ms, current axis in mA
not done
Fig 2: Paper 2 totals shown as dashed lines, error bars on total height
not done
Fig 3: x-axis 500–5000 mAh, y-axis in years (not days), both 2/day and 4/day curves
not done
All figures at 300 DPI, serif font, no grid lines in background
not done
Table I/II include: n=30, mean, std, 95% CI, Paper 2 prediction, relative error %
not done
Paper cites: Paper 2 (model source), INA219 datasheet, Adafruit library, RadioLib
Critical Files
File	Change
analysis/figures.py line 391	Change default=48 → default=2 (cycles/day)
analysis/energy_stats.py pretty_print()	Add ±1.5% systematic error note
dut_esp32s3/src/main.cpp lines 99-100	Insert real WiFi credentials
logger_d1mini/platformio.ini	Toggle SHUNT_CONFIG=0/1 per run
That's the complete file. Two stale spots to keep in mind as you work from it: the line numbers in Part 0 and the Critical Files table are out of date (e.g., the cycles-per-day arg is now at line 402, already =24), and the Critical Files table's "→ default=2" contradicts the "=24" decision in Part 0.1 — go with 24.