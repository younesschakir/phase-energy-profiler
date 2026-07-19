"""
energy_stats.py -- Per-phase energy statistics and Paper 2 comparison.

Reads the phase_summary CSV produced by parse_logs.py and computes
descriptive statistics (mean, std, min, max, 95% CI) per phase over
all measurement cycles.

Prints a comparison table against Paper 2 analytical predictions and
writes results to a CSV for use in the paper.

Usage:
    python energy_stats.py --phases phase_summary.csv --platform A
    python energy_stats.py --phases phases_C.csv --platform C --out-stats stats_C.csv

Author     : Youness Chakir -- Chouaib Doukkali University
Supervisor : Prof. Abdessadek Aaroud
"""

import argparse
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from scipy import stats as sp_stats


# -------------------------------------------------------------------
# Paper 2 analytical predictions
# Source: "Feasibility Analysis of Ultra-Low-Power Image Transmission
#          over LoRaWAN for Analog Water Meter Reading" (ICCSC 2026)
# Platform A = Strategy A (ESP32-S3 + CNN + LoRa in Paper 2, WiFi here)
# Platform C = Strategy C1 (LoRa-E5 + ArduCam, 160x40 ROI, 4 packets)
# NaN = not modelled in Paper 2 for that platform/phase.
# -------------------------------------------------------------------
PAPER2: dict[str, dict[int, dict[str, float]]] = {
    "A": {
        # Phase: {energy_mJ, dur_ms} from Paper 2 Table III / Eq. 11
        1: {"energy_mJ":   6.6,  "dur_ms":   50.0},  # MCU wake-up: 40mA x 50ms @ 3.3V
        2: {"energy_mJ":  49.5,  "dur_ms":  300.0},  # Camera init: 50mA x 300ms
        3: {"energy_mJ":   9.9,  "dur_ms":  100.0},  # Image capture: 30mA x 100ms
        4: {"energy_mJ":  13.2,  "dur_ms":   50.0},  # Preprocess: 80mA x 50ms
        5: {"energy_mJ": 712.8,  "dur_ms": 1200.0},  # CNN inference: 180mA x 1200ms
        6: {"energy_mJ": float("nan"), "dur_ms": float("nan")},  # WiFi TX: not in Paper 2
        7: {"energy_mJ":   0.0,  "dur_ms": float("nan")},        # Deep sleep (~0 active energy)
    },
    "C": {
        1: {"energy_mJ":   0.17, "dur_ms":    5.0},  # MCU wake: 10mA x 5ms
        2: {"energy_mJ":  50.4,  "dur_ms":  330.0},  # Cam init+capture: 50mA x 300ms + 30mA x 30ms
        3: {"energy_mJ":  13.2,  "dur_ms":   50.0},  # Preprocess: 80mA x 50ms
        4: {"energy_mJ": 639.0,  "dur_ms": 1640.0},  # LoRa TX: 118mA x 410ms x 4 pkts
        5: {"energy_mJ":   4.95, "dur_ms":  100.0},  # RX window: 15mA x 100ms
        6: {"energy_mJ":   0.0,  "dur_ms": float("nan")},  # STOP2 sleep (~0 active)
    },
}

PAPER2_TOTAL: dict[str, float] = {
    "A": 801.7,  # mJ (Paper 2 Strategy A total, WiFi TX excluded)
    "C": 577.9,  # mJ (Paper 2 Strategy C1 total)
}

PHASE_NAMES: dict[str, dict[int, str]] = {
    "A": {
        1: "Wake-up",
        2: "Cam Init",
        3: "Capture",
        4: "Preprocess",
        5: "CNN Infer.",
        6: "WiFi TX",
        7: "Sleep",
    },
    "C": {
        1: "Wake-up",
        2: "Capture",
        3: "Preprocess",
        4: "LoRa TX",
        5: "RX Window",
        6: "Sleep",
    },
}


def compute_phase_stats(phases_df: pd.DataFrame) -> pd.DataFrame:
    """
    Compute descriptive statistics per phase over all cycles.

    Parameters
    ----------
    phases_df : pd.DataFrame
        Output of parse_logs.py with columns:
        cycle, phase, avg_mA, peak_mA, dur_ms, energy_mJ

    Returns
    -------
    stats_df : pd.DataFrame
        One row per phase with columns:
        phase, n_cycles,
        energy_mean, energy_std, energy_min, energy_max,
        energy_ci95_lo, energy_ci95_hi,
        dur_mean, dur_std, dur_min, dur_max
    """
    rows = []

    for phase_id, group in phases_df.groupby("phase"):
        n = len(group)
        e_vals   = group["energy_mJ"].values
        dur_vals = group["dur_ms"].values

        e_mean = float(np.mean(e_vals))
        e_std  = float(np.std(e_vals, ddof=1)) if n > 1 else 0.0

        # 95% confidence interval using Student's t-distribution (ddof=n-1)
        if n > 1:
            ci = sp_stats.t.interval(
                0.95, df=n - 1,
                loc=e_mean,
                scale=e_std / math.sqrt(n)
            )
            e_ci_lo, e_ci_hi = float(ci[0]), float(ci[1])
        else:
            e_ci_lo = e_ci_hi = e_mean

        rows.append({
            "phase":         int(phase_id),
            "n_cycles":      n,
            "energy_mean":   e_mean,
            "energy_std":    e_std,
            "energy_min":    float(np.min(e_vals)),
            "energy_max":    float(np.max(e_vals)),
            "energy_ci95_lo": e_ci_lo,
            "energy_ci95_hi": e_ci_hi,
            "dur_mean":      float(np.mean(dur_vals)),
            "dur_std":       float(np.std(dur_vals, ddof=1)) if n > 1 else 0.0,
            "dur_min":       float(np.min(dur_vals)),
            "dur_max":       float(np.max(dur_vals)),
        })

    return pd.DataFrame(rows).sort_values("phase").reset_index(drop=True)


def compare_to_paper2(stats_df: pd.DataFrame, platform: str) -> pd.DataFrame:
    """
    Merge measured statistics with Paper 2 predictions.

    Adds columns:
        paper2_energy_mJ, paper2_dur_ms, energy_rel_error_pct
    """
    predictions = PAPER2.get(platform, {})
    names       = PHASE_NAMES.get(platform, {})

    merged = stats_df.copy()
    merged.insert(1, "phase_name",
                  merged["phase"].map(lambda p: names.get(p, f"P{p}")))

    merged["paper2_energy_mJ"] = merged["phase"].map(
        lambda p: predictions.get(p, {}).get("energy_mJ", float("nan"))
    )
    merged["paper2_dur_ms"] = merged["phase"].map(
        lambda p: predictions.get(p, {}).get("dur_ms", float("nan"))
    )

    def rel_error(row):
        pred = row["paper2_energy_mJ"]
        if math.isnan(pred) or pred == 0:
            return float("nan")
        return 100.0 * (row["energy_mean"] - pred) / pred

    merged["energy_rel_error_pct"] = merged.apply(rel_error, axis=1)

    return merged


def pretty_print(merged: pd.DataFrame, platform: str) -> None:
    """Print a formatted comparison table to stdout."""
    width = 92
    print()
    print("=" * width)
    print(f"  Platform {platform} -- Per-Phase Energy: Measured vs Paper 2 Predictions")
    print("=" * width)
    hdr = (
        f"{'Ph':<3} {'Name':<12} {'N':>4}  "
        f"{'Mean mJ':>9} {'Std':>7} {'95% CI':>16}  "
        f"{'Paper2 mJ':>10} {'Err %':>7}"
    )
    print(hdr)
    print("-" * width)

    total_measured = 0.0
    for _, row in merged.iterrows():
        ph    = int(row["phase"])
        name  = str(row["phase_name"])
        n     = int(row["n_cycles"])
        mean  = row["energy_mean"]
        std   = row["energy_std"]
        ci_lo = row["energy_ci95_lo"]
        ci_hi = row["energy_ci95_hi"]
        p2    = row["paper2_energy_mJ"]
        err   = row["energy_rel_error_pct"]

        p2_str  = f"{p2:>9.2f}" if not math.isnan(p2)  else f"{'N/A':>9}"
        err_str = f"{err:>+7.1f}%" if not math.isnan(err) else f"{'N/A':>8}"

        print(
            f"P{ph:<2} {name:<12} {n:>4}  "
            f"{mean:>9.2f} {std:>7.2f} [{ci_lo:>7.2f},{ci_hi:>7.2f}]  "
            f"{p2_str} {err_str}"
        )
        total_measured += mean

    print("-" * width)

    paper2_total = PAPER2_TOTAL.get(platform, float("nan"))
    if not math.isnan(paper2_total):
        total_err = 100.0 * (total_measured - paper2_total) / paper2_total
        print(
            f"{'TOTAL':<16} {'':>4}  "
            f"{total_measured:>9.2f} {'':>7} {'':>16}  "
            f"{paper2_total:>9.2f} {total_err:>+7.1f}%"
        )
    else:
        print(f"{'TOTAL':<16} {'':>4}  {total_measured:>9.2f}")

    print("=" * width)
    print()
    print("Note: energy_rel_error_pct = (measured - predicted) / predicted x 100%")
    print("      Positive = measured higher than Paper 2 datasheet prediction")
    print("      N/A      = no Paper 2 prediction available for this phase")
    print()
    print("Uncertainty: 95% CI reflects inter-cycle variability only (random error).")
    print("             Systematic error: INA219 accuracy ±0.5% + shunt tolerance ±1%")
    print("             = ±1.5% total systematic -- not included in CI bounds.")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Compute per-phase energy statistics and compare with Paper 2."
    )
    parser.add_argument(
        "--phases", required=True,
        help="Phase summary CSV from parse_logs.py"
    )
    parser.add_argument(
        "--platform", required=True, choices=["A", "C"],
        help="Platform A (ESP32-S3 + WiFi + CNN) or C (LoRa-E5 + LoRaWAN)"
    )
    parser.add_argument(
        "--out-stats", default="stats.csv",
        help="Output statistics CSV (default: stats.csv)"
    )
    args = parser.parse_args()

    in_path = Path(args.phases)
    if not in_path.exists():
        print(f"ERROR: Input file not found: {in_path}", file=sys.stderr)
        sys.exit(1)

    phases_df = pd.read_csv(in_path)
    if phases_df.empty:
        print("ERROR: Phase summary CSV is empty.", file=sys.stderr)
        sys.exit(1)

    print(f"Loaded {len(phases_df)} phase rows from: {in_path}")
    print(f"Cycles: {phases_df['cycle'].nunique()} | "
          f"Phases: {phases_df['phase'].nunique()}")

    stats_df = compute_phase_stats(phases_df)
    merged   = compare_to_paper2(stats_df, args.platform)

    pretty_print(merged, args.platform)

    merged.to_csv(args.out_stats, index=False, float_format="%.4f")
    print(f"Statistics written to: {Path(args.out_stats).resolve()}")


if __name__ == "__main__":
    main()
