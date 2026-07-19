# OV2640 windowing register exploration (supplementary to Section 5.3)

Goal: obtain a JPEG output smaller than QQVGA (target 160x40 digit strip) from
the ArduCAM Mini 2MP Plus (OV2640). Every configuration below was applied on
real hardware and the *actual* output size verified by parsing the JPEG SOF
marker of the captured frame (`analysis/jpegdump.py`). Firmware:
`firmware/wio-e5/src/windowtest.cpp` (env `lora_e5_windowtest`).

Key mechanism notes established first:

- Size changes only latch when written inside the sensor's
  `0xE0 = 0x04` (DSP reset) ... `0xE0 = 0x00` (release) bracket. Standard
  ArduCAM size tables do this; ad-hoc register pokes outside the bracket are
  ignored.
- The apply path was proven sound by reconstructing the stock 176x144 table
  from the 160x120 table via the same override mechanism -> output was exactly
  176x144. Fallbacks below are therefore genuine DSP rejections, not
  write failures.
- SCCB register *reads* are unreliable on this setup (mostly 0xFF); do not
  diagnose by readback.

## Sweep results

| # | Path | Configuration (all writes in-bracket) | Requested out | Actual out |
|---|------|----------------------------------------|---------------|-----------|
| 1 | DSP zoom | stock `OV2640_160x120_JPEG` table | 160x120 | 160x120 (OK) |
| 2 | DSP zoom | stock table + ZMOW/ZMOH of 176x144 | 176x144 | 176x144 (OK, control) |
| 3 | DSP zoom | ZMOH -> 40 only (squish) | 160x40 | 320x240 (fallback) |
| 4 | DSP zoom | full esp32-camera crop set: CTRLI=0x80 (no divider), CTRL2=0x3D, VSIZE/YOFF/VHYX for an 800x200 band, ZMOH=10 | 160x40 | 320x240 (fallback) |
| 5 | CIF datapath | complete CIF mode table (COM7=0x20, REG32=0x89, HSIZE8/VSIZE8=CIF), full-frame window | 160x120 | 160x120 (OK) |
| 6 | CIF datapath | same + 400x100 (4:1) input band, ZMOW=40/ZMOH=10 | 160x40 | 320x240 (fallback) |
| 7 | CIF datapath | 400x200 band (2:1), ZMOH=20 | 160x80 | 320x240 (fallback) |
| 8 | CIF datapath | 400x240 band (5:3), ZMOH=24 | 160x96 | 320x240 (fallback) |
| 9 | CIF datapath | centered 200x150 (4:3) sub-window, ZMOW=40/ZMOH=30 | 160x120 | 160x120 (OK — true optical zoom-crop; distinct, larger-detail image) |
| 10 | DSP zoom | full-frame 4:3, ZMOW=20/ZMOH=15 | 80x60 | 320x240 (fallback) |
| 11 | Sensor array | UXGA base + HSTART/HSTOP/VSTART/VSTOP cropped to a ~160x40 patch, DSP 1:1 passthrough (CTRLI=0) | 160x40 | 320x240 (fallback; byte count changed, so the array crop DID alter readout — the output-size floor is downstream, in the JPEG output stage) |

## Conclusion

Across rows 1-11: every requested output below ~120 rows, or far from 4:3
aspect, silently reverts to the QVGA default, regardless of whether the
reduction is attempted in the DSP zoom stage, via a full mode-table datapath,
or at the sensor array. Within the floor, true optical cropping works (row 9).
The claim in the paper is scoped to these configurations; an undocumented
register combination cannot be excluded.

Reproduce: flash `lora_e5_windowtest`, edit the `cfg*/ovr[]` tables in
`windowtest.cpp`, read the frame with `python analysis/jpegdump.py COMx`.
