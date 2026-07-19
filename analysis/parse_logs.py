"""
parse_logs.py -- Parse INA219 logger serial output into structured CSVs.

Handles raw text captured from the D1 Mini logger at 500000 baud
(e.g. via `pio device monitor > raw_log.txt`).

Two output files are produced:
  1. samples CSV  -- all raw measurement points (one row per INA219 read)
  2. phases CSV   -- per-phase per-cycle energy summaries (PHASE_CSV lines)

The phases CSV is the primary input for energy_stats.py and figures.py.

Usage:
    python parse_logs.py --input raw_log.txt
    python parse_logs.py --input raw_A.txt --out-samples samples_A.csv --out-phases phases_A.csv

Author     : Youness Chakir -- Chouaib Doukkali University
Supervisor : Prof. Abdessadek Aaroud
"""

import argparse
import re
import sys
from pathlib import Path

import pandas as pd


# -------------------------------------------------------------------
# Output column schemas
# -------------------------------------------------------------------
SAMPLE_COLS = ["timestamp_ms", "current_mA", "voltage_V", "power_mW", "phase"]
PHASE_COLS  = ["cycle", "phase", "avg_mA", "peak_mA", "dur_ms", "energy_mJ"]

# -------------------------------------------------------------------
# Line classifiers
# -------------------------------------------------------------------
COMMENT_RE = re.compile(r"^\s*#")
HEADER_RE  = re.compile(r"^timestamp_ms")
# Sample line: integer, float, float, float, integer  (5 comma-separated fields)
SAMPLE_RE  = re.compile(
    r"^(\d+),([+-]?\d+\.?\d*),([+-]?\d+\.?\d*),([+-]?\d+\.?\d*),(\d+)$"
)
# PHASE_CSV line: PHASE_CSV,cycle,phase,avg_mA,peak_mA,dur_ms,energy_mJ
PHASE_RE   = re.compile(
    r"^PHASE_CSV,(\d+),(\d+),([+-]?\d+\.?\d*),([+-]?\d+\.?\d*),([+-]?\d+\.?\d*),([+-]?\d+\.?\d*)$"
)


def _parse_sample(line: str) -> dict | None:
    m = SAMPLE_RE.match(line.strip())
    if not m:
        return None
    return {
        "timestamp_ms": int(m.group(1)),
        "current_mA":   float(m.group(2)),
        "voltage_V":    float(m.group(3)),
        "power_mW":     float(m.group(4)),
        "phase":        int(m.group(5)),
    }


def _parse_phase(line: str) -> dict | None:
    m = PHASE_RE.match(line.strip())
    if not m:
        return None
    return {
        "cycle":     int(m.group(1)),
        "phase":     int(m.group(2)),
        "avg_mA":    float(m.group(3)),
        "peak_mA":   float(m.group(4)),
        "dur_ms":    float(m.group(5)),
        "energy_mJ": float(m.group(6)),
    }


def parse_log_file(path: Path) -> tuple[pd.DataFrame, pd.DataFrame]:
    """
    Parse a raw serial log file into two DataFrames.

    Parameters
    ----------
    path : Path
        Path to the raw log text file.

    Returns
    -------
    samples_df : pd.DataFrame
        All raw INA219 measurement samples.
        Columns: timestamp_ms, current_mA, voltage_V, power_mW, phase
    phases_df : pd.DataFrame
        Per-phase-per-cycle energy summaries (from PHASE_CSV lines).
        Columns: cycle, phase, avg_mA, peak_mA, dur_ms, energy_mJ
    """
    samples: list[dict] = []
    phases:  list[dict] = []
    skipped = 0
    n_lines = 0

    with open(path, encoding="utf-8", errors="replace") as f:
        for raw_line in f:
            n_lines += 1
            line = raw_line.rstrip()

            # Skip comments and header
            if COMMENT_RE.match(line) or HEADER_RE.match(line) or not line:
                continue

            # Try PHASE_CSV first (more specific pattern)
            row = _parse_phase(line)
            if row is not None:
                phases.append(row)
                continue

            # Try sample line
            row = _parse_sample(line)
            if row is not None:
                samples.append(row)
                continue

            skipped += 1

    print(f"Parsed {n_lines:,} lines: "
          f"{len(samples):,} samples, {len(phases):,} phase summaries, "
          f"{skipped} unrecognised")

    # Build DataFrames
    if samples:
        samples_df = pd.DataFrame(samples, columns=SAMPLE_COLS)
        samples_df["timestamp_ms"] = samples_df["timestamp_ms"].astype("int64")
        samples_df["phase"]        = samples_df["phase"].astype("int8")
    else:
        samples_df = pd.DataFrame(columns=SAMPLE_COLS)

    if phases:
        phases_df = pd.DataFrame(phases, columns=PHASE_COLS)
        phases_df["cycle"] = phases_df["cycle"].astype("int32")
        phases_df["phase"] = phases_df["phase"].astype("int8")
    else:
        phases_df = pd.DataFrame(columns=PHASE_COLS)

    # Validation warnings
    _validate(phases_df)

    return samples_df, phases_df


def _validate(phases_df: pd.DataFrame) -> None:
    """Print warnings for suspicious data without raising errors."""
    if phases_df.empty:
        print("WARNING: No PHASE_CSV lines found. "
              "Check that the DUT was running and phase markers were received.")
        return

    # Check for cycles with fewer phases than expected
    phase_counts = phases_df.groupby("cycle")["phase"].count()
    max_phases   = phases_df["phase"].max()
    short_cycles = phase_counts[phase_counts < max_phases]
    if not short_cycles.empty:
        print(f"WARNING: {len(short_cycles)} cycle(s) have fewer than "
              f"{max_phases} phases: cycles {list(short_cycles.index)}")

    # Check for zero-duration phases (timing glitch)
    zero_dur = phases_df[phases_df["dur_ms"] == 0]
    if not zero_dur.empty:
        print(f"WARNING: {len(zero_dur)} phase row(s) have dur_ms=0 "
              f"(possible marker timing issue)")

    # Report cycle count
    n_cycles = phases_df["cycle"].nunique()
    print(f"Cycles detected: {n_cycles} | "
          f"Phases per cycle: {max_phases} | "
          f"Total phase rows: {len(phases_df)}")


def main():
    parser = argparse.ArgumentParser(
        description="Parse INA219 logger serial output into structured CSVs."
    )
    parser.add_argument(
        "--input", required=True,
        help="Raw serial log file (captured from pio device monitor)"
    )
    parser.add_argument(
        "--out-samples", default="samples.csv",
        help="Output CSV for raw samples (default: samples.csv)"
    )
    parser.add_argument(
        "--out-phases", default="phase_summary.csv",
        help="Output CSV for phase summaries (default: phase_summary.csv)"
    )
    args = parser.parse_args()

    in_path = Path(args.input)
    if not in_path.exists():
        print(f"ERROR: Input file not found: {in_path}", file=sys.stderr)
        sys.exit(1)

    samples_df, phases_df = parse_log_file(in_path)

    # Write outputs
    samples_df.to_csv(args.out_samples, index=False)
    phases_df.to_csv(args.out_phases,   index=False)

    print(f"Samples written to : {Path(args.out_samples).resolve()}  ({len(samples_df):,} rows)")
    print(f"Phases  written to : {Path(args.out_phases).resolve()}   ({len(phases_df):,} rows)")


if __name__ == "__main__":
    main()
