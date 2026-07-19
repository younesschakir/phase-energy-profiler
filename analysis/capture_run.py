#!/usr/bin/env python3
"""
Capture an N-cycle phase-decomposed energy run from the D1-Mini/INA logger.

The logger streams CSV `timestamp_ms,current_mA,voltage_V,power_mW,phase` on
COM3 @500000, where `phase` is 0..6 (0 = inter-cycle sleep, 1..6 = the node's
P1..P6, set from the UART phase markers on PA9->D5).

Writes:
  <analysis_dir>/samples_<label>.csv   raw stream (all rows)
  <analysis_dir>/phases_<label>.csv    per (cycle,phase): avg_mA,peak_mA,dur_ms,energy_mJ

A cycle = one pass entering phase 1. Phase 0 (sleep) is excluded from the
per-phase summary (matches phases_B/C.csv). Stops after N complete cycles or
`timeout` seconds.

Usage:
  python capture_run.py <analysis_dir> <cycles> <timeout_s> <label> [COMport]
  e.g. python capture_run.py ../analysis 30 1200 node COM3
"""
import sys, os, time, serial


def main():
    if len(sys.argv) < 5:
        print(__doc__); return
    outdir  = sys.argv[1]
    ncycles = int(sys.argv[2])
    timeout = float(sys.argv[3])
    label   = sys.argv[4]
    port    = sys.argv[5] if len(sys.argv) > 5 else "COM3"

    os.makedirs(outdir, exist_ok=True)
    samples_path = os.path.join(outdir, f"samples_{label}.csv")
    phases_path  = os.path.join(outdir, f"phases_{label}.csv")

    ser = serial.Serial(port, 500000, timeout=1)
    fsamp = open(samples_path, "w", newline="")
    fsamp.write("timestamp_ms,current_mA,voltage_V,power_mW,phase\n")

    # per-phase accumulator for the current segment
    seg = None            # (cycle, phase, [ (ts,mA,mW), ... ])
    cycle = 0
    prev_phase = None
    started = False       # ignore the partial cycle we may start mid-stream
    phase_rows = []       # finished (cycle,phase,avg,peak,dur,energy)
    t0 = time.time()

    def flush_segment(s):
        c, ph, rows = s
        if ph == 0 or len(rows) < 1:
            return
        ts = [r[0] for r in rows]; mA = [r[1] for r in rows]; mW = [r[2] for r in rows]
        dur = (ts[-1] - ts[0]) if len(ts) > 1 else 1.0
        if dur <= 0: dur = 1.0
        avg = sum(mA)/len(mA)
        peak = max(mA)
        energy = (sum(mW)/len(mW)) * dur / 1000.0   # mean power(mW) * dur(s) = mJ
        phase_rows.append((c, ph, avg, peak, dur, energy))

    try:
        while time.time() - t0 < timeout and cycle < ncycles:
            raw = ser.readline().decode("ascii", errors="ignore").strip()
            if not raw:
                continue
            parts = raw.split(",")
            if len(parts) != 5:
                continue
            try:
                ts = float(parts[0]); mA = float(parts[1])
                v  = float(parts[2]); mW = float(parts[3]); ph = int(parts[4])
            except ValueError:
                continue
            fsamp.write(raw + "\n")

            # cycle boundary: entering phase 1 from something else
            if ph == 1 and prev_phase != 1:
                if started:
                    cycle += 1
                    print(f"  cycle {cycle}/{ncycles}")
                else:
                    started = True
                    cycle = 1
                    print(f"  cycle 1/{ncycles} (recording)")

            if started:
                if seg is None or seg[1] != ph:
                    if seg is not None:
                        flush_segment(seg)
                    seg = (cycle, ph, [])
                seg[2].append((ts, mA, mW))
            prev_phase = ph
    except KeyboardInterrupt:
        print("interrupted")
    finally:
        if seg is not None:
            flush_segment(seg)
        ser.close()
        fsamp.close()
        with open(phases_path, "w", newline="") as fp:
            fp.write("cycle,phase,avg_mA,peak_mA,dur_ms,energy_mJ\n")
            for c, ph, avg, peak, dur, en in phase_rows:
                fp.write(f"{c},{ph},{avg:.3f},{peak:.3f},{dur:.1f},{en:.4f}\n")
        print(f"\nsaved {samples_path}\nsaved {phases_path}\n"
              f"{cycle} cycles, {len(phase_rows)} phase-rows")


if __name__ == "__main__":
    main()
