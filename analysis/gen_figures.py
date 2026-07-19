#!/usr/bin/env python3
"""
Article figure generator — Paper 3 (phase-decomposed energy measurement).
Reads the measured phases_*.csv / samples_*.csv and produces all figures
into analysis/figures/article/ as 300-dpi PNG + vector PDF.

Conventions (Elsevier + CVD-safe Okabe-Ito subset, validated):
  entity colors fixed across every figure; one axis; recessive grids;
  direct labels; text in neutral ink.
"""
import csv, os, math
import statistics as st
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import matplotlib.image as mpimg

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "figures", "article")
os.makedirs(OUT, exist_ok=True)

# ---------- style ----------
plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Arial", "Helvetica", "DejaVu Sans"],
    "font.size": 8, "axes.titlesize": 8.5, "axes.labelsize": 8,
    "xtick.labelsize": 7.5, "ytick.labelsize": 7.5, "legend.fontsize": 7.5,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#dddddd", "grid.linewidth": 0.5,
    "axes.axisbelow": True, "figure.dpi": 120,
})
INK = "#222222"

# entity colors (fixed across all figures)
C_A      = "#0072B2"   # Strategy/Platform A (WiFi + edge CNN)
C_B      = "#D55E00"   # Strategy B (LoRa full-frame)
C_C      = "#009E73"   # Strategy C (LoRa ROI)
C_BGATE  = "#E69F00"   # B-gated
C_ANODE  = "#56B4E9"   # deployable A node
C_ENODE  = "#CC79A7"   # deployable E5 node
# function/phase colors (their own legend space, consistent across figures)
F_CAM   = "#56B4E9"    # camera (init+capture)
F_RADIO = "#D55E00"    # radio TX+RX
F_PROC  = "#CC79A7"    # processing / CNN
F_OTH   = "#999999"    # other (wake, sleep-entry)

SC = 3.54   # single-column width (in), 90 mm
DC = 7.08   # ~double-column (in)

def save(fig, name):
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(OUT, f"{name}.{ext}"),
                    dpi=300, bbox_inches="tight")
    plt.close(fig)
    print("saved", name)

def load_phases(fname):
    """-> {phase: [energy_mJ,...]}, {phase:(mean_mA, dur_ms)}"""
    e = defaultdict(list); meta = {}
    with open(os.path.join(HERE, fname)) as f:
        for r in csv.DictReader(f):
            # exclude the one truncated-capture cycle (defective; see paper 4.3)
            if fname == "phases_node_B.csv" and r["cycle"] == "6":
                continue
            p = int(r["phase"])
            e[p].append(float(r["energy_mJ"]))
            meta.setdefault(p, []).append((float(r["avg_mA"]), float(r["dur_ms"])))
    stats = {p: (st.mean(x[0] for x in v), st.mean(x[1] for x in v))
             for p, v in meta.items()}
    return {p: st.mean(v) for p, v in e.items()}, \
           {p: (st.pstdev(v) if len(v) > 1 else 0) for p, v in e.items()}, stats

# measured per-phase means
A_e,  A_sd,  A_m  = load_phases("phases_A.csv")        # 7 phases (P6=WiFi)
B_e,  B_sd,  B_m  = load_phases("phases_B.csv")
Ce,   C_sd,  C_m  = load_phases("phases_C.csv")
N_e,  N_sd,  N_m  = load_phases("phases_node.csv")     # E5 node
NB_e, NB_sd, NB_m = load_phases("phases_node_B.csv")   # B gated
NA_e, NA_sd, NA_m = load_phases("phases_node_a.csv")   # A node

# ============================================================
# FIG 1 — measurement architecture (block diagram)
# ============================================================
def fig_rig():
    fig, ax = plt.subplots(figsize=(DC, 2.6))
    ax.set_xlim(0, 10); ax.set_ylim(0, 3.4); ax.axis("off")
    def box(x, y, w, h, label, fc="#f2f2f2", ec=INK):
        ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.06",
                                    fc=fc, ec=ec, lw=0.9))
        ax.text(x + w/2, y + h/2, label, ha="center", va="center",
                fontsize=7.5, color=INK)
    def arrow(x1, y1, x2, y2, label=None, color=INK, style="-|>"):
        ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle=style,
                     mutation_scale=9, lw=1.0, color=color))
        if label:
            ax.text((x1+x2)/2, (y1+y2)/2 + 0.12, label, ha="center",
                    fontsize=6.8, color=color)
    box(0.1, 1.9, 1.5, 0.9, "5 V source\n(USB)")
    box(2.3, 1.9, 2.0, 0.9, "INA219\ncurrent sensor\n(shunt in supply path)")
    box(5.3, 1.9, 2.1, 0.9, "Device under test\nESP32-S3 / STM32WLE5\n+ ArduCAM (gated)")
    box(8.0, 1.9, 1.9, 0.9, "Camera power\ngate (MOSFET)")
    box(2.3, 0.3, 2.0, 0.9, "Logger (D1 Mini)\n500 kbaud CSV\nts, mA, V, mW, phase")
    box(5.3, 0.3, 2.1, 0.9, "Host PC\ncycle segmentation\n+ per-phase energy")
    arrow(1.6, 2.35, 2.3, 2.35)
    arrow(4.3, 2.35, 5.3, 2.35)
    ax.text(4.8, 2.95, "all DUT current", ha="center", fontsize=6.8, color=INK)
    arrow(7.4, 2.35, 8.0, 2.35)
    arrow(3.0, 1.9, 3.0, 1.25, color="#555555")
    ax.text(2.9, 1.55, "I(t) @ ~1 kHz", ha="right", fontsize=6.8, color="#555555")
    arrow(5.5, 1.9, 4.5, 1.15, color=C_A)
    ax.text(5.6, 1.5, "UART phase markers\n(0xA1..0xA6, 9600 Bd)",
            fontsize=6.8, color=C_A)
    arrow(4.3, 0.75, 5.3, 0.75)
    save(fig, "fig1_rig")
fig_rig()

# ============================================================
# FIG 2 — annotated current waveforms (one cycle, both nodes)
# ============================================================
def one_cycle(samples_file, pad_pre=1.0, pad_post=4.0):
    """extract (t, mA, bands) for the 2nd complete cycle in the file"""
    rows = []
    with open(os.path.join(HERE, samples_file)) as f:
        next(f)
        for line in f:
            p = line.strip().split(",")
            if len(p) != 5: continue
            try: rows.append((float(p[0]), float(p[1]), int(p[4])))
            except ValueError: continue
    # find P1 starts
    starts = [i for i in range(1, len(rows))
              if rows[i][2] == 1 and rows[i-1][2] != 1]
    if len(starts) < 3: return None
    i0 = starts[1]; i1 = starts[2]
    t0 = rows[i0][0]
    # window: pad before P1 to pad after last active phase
    lo = next(i for i in range(i0, 0, -1) if rows[i][0] < t0 - pad_pre*1000)
    # end of P6
    iend = i0
    for i in range(i0, i1):
        if rows[i][2] != 0: iend = i
    t_end = rows[iend][0]
    hi = next((i for i in range(iend, len(rows)) if rows[i][0] > t_end + pad_post*1000), len(rows)-1)
    seg = rows[lo:hi]
    t = [(r[0]-t0)/1000.0 for r in seg]
    mA = [min(r[1], 400) for r in seg]
    ph = [r[2] for r in seg]
    # phase bands
    bands = []; cur = ph[0]; bs = t[0]
    for i in range(1, len(seg)):
        if ph[i] != cur:
            bands.append((cur, bs, t[i])); cur = ph[i]; bs = t[i]
    bands.append((cur, bs, t[-1]))
    return t, mA, bands

def fig_waveforms():
    fig, axes = plt.subplots(2, 1, figsize=(DC, 4.2), sharex=False)
    jobs = [("samples_node.csv",   "E5 node (STM32WLE5 + LoRa, gated ROI image)", axes[0], C_ENODE),
            ("samples_node_a.csv", "A node (ESP32-S3 + WiFi, gated camera, on-device CNN)", axes[1], C_ANODE)]
    ph_col = {1: F_OTH, 2: F_CAM, 3: F_OTH, 4: None, 5: None, 6: F_OTH}
    for fname, title, ax, lc in jobs:
        r = one_cycle(fname)
        if not r: continue
        t, mA, bands = r
        # phase shading; P4/P5 differ per platform
        for pnum, b0, b1 in bands:
            if pnum == 0: continue
            if fname.endswith("node.csv"):
                col = {2: F_CAM, 4: F_RADIO, 5: F_RADIO}.get(pnum, F_OTH)
            else:
                col = {2: F_CAM, 4: F_PROC, 5: F_RADIO}.get(pnum, F_OTH)
            ax.axvspan(b0, b1, color=col, alpha=0.18, lw=0)
        ax.plot(t, mA, lw=0.7, color=lc)
        ax.set_ylabel("Current (mA)")
        ax.set_title(title, loc="left", fontsize=8)
        ax.set_xlim(t[0], t[-1])
    # annotations
    axes[0].annotate("P2 capture (camera cold\nstart + ROI capture)", xy=(0.15, 55),
                     fontsize=6.8, color=INK)
    axes[0].annotate("P4 LoRa TX (7 pkts)", xy=(3.1, 120), fontsize=6.8, color=INK)
    axes[0].annotate("STOP2 sleep", xy=(5.6, 40), fontsize=6.8, color=INK)
    axes[1].annotate("P2 capture", xy=(0.6, 190), fontsize=6.8, color=INK)
    axes[1].annotate("P4 CNN\n(jomjol)", xy=(2.35, 120), fontsize=6.8, color=INK)
    axes[1].annotate("P5 WiFi", xy=(3.35, 260), fontsize=6.8, color=INK)
    axes[1].annotate("deep sleep", xy=(4.8, 60), fontsize=6.8, color=INK)
    axes[1].set_xlabel("Time (s)")
    fig.tight_layout()
    save(fig, "fig6_waveforms")
fig_waveforms()

# ============================================================
# FIG 3 — per-phase energy, un-gated strategies A/B/C (log y)
# ============================================================
def fig_phases_ungated():
    # function-level grouping
    # A: P1 init,P2 warmup,P3 capture,P4 preproc,P5 CNN,P6 WiFi,P7 entry
    A = {"Camera": A_e.get(2,0)+A_e.get(3,0), "Processing": A_e.get(4,0)+A_e.get(5,0),
         "Radio (TX+RX)":  A_e.get(6,0), "Other": A_e.get(1,0)+A_e.get(7,0)}
    B = {"Camera": B_e.get(2,0), "Processing": B_e.get(3,0),
         "Radio (TX+RX)":  B_e.get(4,0)+B_e.get(5,0), "Other": B_e.get(1,0)+B_e.get(6,0)}
    C = {"Camera": Ce.get(2,0), "Processing": Ce.get(3,0),
         "Radio (TX+RX)":  Ce.get(4,0)+Ce.get(5,0), "Other": Ce.get(1,0)+Ce.get(6,0)}
    cats = ["Camera", "Processing", "Radio (TX+RX)", "Other"]
    fig, ax = plt.subplots(figsize=(SC, 2.6))
    w = 0.26; xs = range(len(cats))
    for off, (name, d, col) in enumerate([
            ("A: WiFi + edge CNN", A, C_A),
            ("B: LoRa full-frame", B, C_B),
            ("C: LoRa ROI", C, C_C)]):
        vals = [max(d[c], 0.1) for c in cats]
        bars = ax.bar([x + (off-1)*w for x in xs], vals, w, color=col,
                      label=name, edgecolor="white", lw=0.5)
        for b, v in zip(bars, vals):
            if v > 0.5:
                lab = f"{v:.0f}" if v >= 10 else f"{v:.1f}"
                ax.text(b.get_x()+b.get_width()/2, v*1.15, lab,
                        ha="center", fontsize=6.2, color=INK)
    ax.set_yscale("log"); ax.set_ylim(1, 60000)
    ax.set_xticks(list(xs)); ax.set_xticklabels(cats)
    ax.set_ylabel("Energy per cycle (mJ, log)")
    ax.legend(frameon=False, loc="upper left", fontsize=6.8,
              handlelength=1.2, labelspacing=0.3)
    save(fig, "fig2_phases_ungated")
fig_phases_ungated()

# ============================================================
# FIG 4 — model prediction vs measurement
# ============================================================
def fig_validation():
    pairs = [("Strategy B\nfull-frame cycle", 6092, 6779, C_B),
             ("Strategy C\nROI cycle",        577.9, 1288, C_C),
             ("Edge CNN\ninference",          712.8, 222.8, C_A)]
    fig, ax = plt.subplots(figsize=(SC, 2.6))
    w = 0.34
    for i, (name, pred, meas, col) in enumerate(pairs):
        b1 = ax.bar(i - w/2, pred, w, color="#bbbbbb", edgecolor="white",
                    label="Model prediction" if i == 0 else None)
        b2 = ax.bar(i + w/2, meas, w, color=col, edgecolor="white",
                    label="Measured" if i == 0 else None)
        dev = (meas - pred) / pred * 100
        ax.text(i + w/2, meas*1.06, f"{dev:+.0f}%", ha="center",
                fontsize=7, color=INK, weight="bold")
        ax.text(i - w/2, pred*1.06, f"{pred:.0f}", ha="center", fontsize=6.2, color="#555")
        ax.text(i + w/2, meas*0.5, f"{meas:.0f}", ha="center", fontsize=6.2, color="white")
    ax.set_yscale("log"); ax.set_ylim(100, 20000)
    ax.set_xticks(range(len(pairs)))
    ax.set_xticklabels([p[0] for p in pairs], fontsize=7)
    ax.set_ylabel("Energy (mJ, log)")
    ax.legend(frameon=False, loc="upper right")
    save(fig, "fig3_validation")
fig_validation()

# ============================================================
# FIG 5 — the gated energy ladder (stacked, function-level)
# ============================================================
def fig_ladder():
    # (camera, radio, cnn, other)
    variants = [
        ("B un-gated\n(as-built)",
         B_e.get(2,0), B_e.get(4,0)+B_e.get(5,0), 0, B_e.get(1,0)+B_e.get(3,0)+B_e.get(6,0)),
        ("B gated\n(full frame)",
         NB_e.get(2,0), NB_e.get(4,0)+NB_e.get(5,0), 0,
         NB_e.get(1,0)+NB_e.get(3,0)+NB_e.get(6,0)),
        ("E5 node\n(gated ROI)",
         N_e.get(2,0), N_e.get(4,0)+N_e.get(5,0), 0,
         N_e.get(1,0)+N_e.get(3,0)+N_e.get(6,0)),
        ("A node\n(CNN + reading)",
         NA_e.get(2,0), NA_e.get(5,0), NA_e.get(4,0),
         NA_e.get(1,0)+NA_e.get(3,0)+NA_e.get(6,0)),
    ]
    fig, ax = plt.subplots(figsize=(SC, 2.8))
    xs = range(len(variants))
    segs = [("Camera", 1, F_CAM), ("Radio", 2, F_RADIO),
            ("CNN", 3, F_PROC), ("Other", 4, F_OTH)]
    bottoms = [0]*len(variants)
    for label, idx, col in segs:
        vals = [v[idx] for v in variants]
        ax.bar(xs, vals, 0.55, bottom=bottoms, color=col, label=label,
               edgecolor="white", lw=0.8)
        bottoms = [b+v for b, v in zip(bottoms, vals)]
    for x, tot in zip(xs, bottoms):
        ax.text(x, tot+120, f"{tot:.0f} mJ", ha="center", fontsize=7,
                weight="bold", color=INK)
    ax.set_xticks(list(xs))
    ax.set_xticklabels([v[0] for v in variants], fontsize=6.8)
    ax.set_ylabel("Active energy per reading (mJ)")
    ax.set_ylim(0, 8400)
    ax.legend(frameon=False, ncol=2, loc="upper right",
              bbox_to_anchor=(1.0, 0.97), columnspacing=0.9, handlelength=1.1)
    save(fig, "fig7_ladder")
fig_ladder()

# ============================================================
# FIG 6 — sleep-floor decomposition (log horizontal bars)
# ============================================================
def fig_sleepfloor():
    rows = [  # (label, mA, color)
        ("Integrated camera, no gate\n(XIAO Sense, as-built)", 93, C_A),
        ("Module camera, no gate\n(Wio-E5 + ArduCAM)", 137, C_B),
        ("Camera gated, MCU awake\n(delay-gap firmware)", 13, C_C),
        ("E5 node: gated + STOP2\n(dev-board overhead)", 5.5, C_ENODE),
        ("A node: gated + deep sleep", 0.19, C_ANODE),
        ("Bare chipset (datasheet)\nSTM32WLE5 + SX1262 retention", 0.004, "#666666"),
    ]
    fig, ax = plt.subplots(figsize=(SC, 2.7))
    ys = range(len(rows))[::-1]
    for y, (lab, v, col) in zip(ys, rows):
        ax.barh(y, v, 0.6, color=col, edgecolor="white")
        ax.text(v*1.25, y, f"{v:g} mA", va="center", fontsize=7, color=INK)
    ax.set_xscale("log"); ax.set_xlim(0.001, 700)
    ax.set_yticks(list(ys))
    ax.set_yticklabels([r[0] for r in rows], fontsize=6.6)
    ax.set_xlabel("Sleep-floor current (mA, log)")
    save(fig, "fig5_sleepfloor")
fig_sleepfloor()

# ============================================================
# FIG 7 — autonomy vs reading frequency
# ============================================================
def fig_autonomy():
    BATT_J = 19.0 * 3.6 * 3600   # cell energy, 19 Ah * 3.6 V (ER34615)
    SHELF_D = 15*365
    def days(e_mJ, isl_mA, nd, V, eta):
        daily = nd*e_mJ/1000.0 + isl_mA/1000.0*V*86400
        return min(eta*BATT_J/daily, SHELF_D)
    nds = list(range(1, 25))
    # (label, E_read mJ, Isl mA, rail V, eta, color, linestyle)
    curves = [
        ("B un-gated (as-built)",    6779, 150,   5.2, 0.85, C_B,    "--"),
        ("E5 node, dev board",       2338, 5.5,   5.2, 0.85, C_ENODE, "-"),
        ("A node, dev board",        1553, 0.19,  5.2, 0.85, C_ANODE, "-"),
        ("E5 node, deployment PCB (direct cell, 3 µA)", 1529, 0.003, 3.4, 1.0, C_ENODE, ":"),
    ]
    fig, ax = plt.subplots(figsize=(SC, 2.7))
    for name, e, isl, V, eta, col, ls in curves:
        ax.plot(nds, [days(e, isl, n, V, eta)/365 for n in nds], ls, color=col,
                lw=1.6, label=name)
    ax.axhline(SHELF_D/365, color="#888888", lw=0.8, ls="-.")
    ax.text(1.4, SHELF_D/365*1.15, "battery shelf life", fontsize=6.5,
            color="#666666")
    ax.axhline(5, color="#bbbbbb", lw=0.8)
    ax.text(1.4, 3.4, "utility 5-yr threshold", fontsize=6.5, color="#888888")
    ax.text(12, 0.0125, "≈ 3 days", fontsize=6.5, color=C_B,
            ha="center")
    ax.set_yscale("log")
    ax.set_ylim(0.005, 40)
    ax.set_xlim(1, 24)
    ax.set_xlabel("Readings per day")
    ax.set_ylabel("Autonomy (years, log)")
    ax.legend(frameon=False, fontsize=6.4, loc="center left",
              bbox_to_anchor=(0.02, 0.42))
    save(fig, "fig8_autonomy")
fig_autonomy()

# ============================================================
# FIG 4 — capture-variant payload sizes (measured JPEG bytes)
# ============================================================
def fig_captures():
    # measured JPEG byte counts of the capture variants on the fixed lab scene
    variants = [
        ("Hardware ROI zoom-crop\n(160x120, detail-preserving)", 2576, C_C),
        ("QQVGA color (default)",                                1825, C_B),
        ("QQVGA grayscale",                                     1777, C_BGATE),
        ("Grayscale + strong\ncompression (QS = 0x40)",        1689, C_ENODE),
    ]
    labels = [v[0] for v in variants]
    vals   = [v[1] for v in variants]
    cols   = [v[2] for v in variants]
    fig, ax = plt.subplots(figsize=(DC, 2.3))
    y = list(range(len(variants)))[::-1]
    bars = ax.barh(y, vals, color=cols, height=0.62, edgecolor="white", lw=0.6)
    for yi, v in zip(y, vals):
        ax.text(v + 30, yi, f"{v} B", va="center", ha="left", fontsize=7.2, color=INK)
    # reduction annotations relative to the QQVGA color baseline
    base = 1825
    for yi, v in zip(y[1:], vals[1:]):
        if v < base:
            ax.text(v/2, yi, f"-{100*(base-v)/base:.1f}%", va="center", ha="center",
                    fontsize=6.6, color="white")
    ax.set_yticks(y); ax.set_yticklabels(labels, fontsize=7)
    ax.set_xlabel("Transmitted JPEG payload (bytes, measured)")
    ax.set_xlim(0, max(vals) * 1.18)
    ax.grid(axis="x", color="#dddddd", lw=0.5); ax.grid(axis="y", visible=False)
    fig.tight_layout()
    save(fig, "fig4_captures")
fig_captures()

print("\nAll figures ->", OUT)
