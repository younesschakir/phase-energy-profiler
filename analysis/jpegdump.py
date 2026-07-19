#!/usr/bin/env python3
"""
Capture the hex-dumped JPEG from windowtest.cpp (env:lora_e5_windowtest) over
the CP2102 and verify the TRUE output dimensions.

The firmware prints, per frame:
    JPEG_START len=NNNN
    <hex bytes, no spaces, wrapped>
    JPEG_END

We reconstruct the bytes, save the .jpg, and parse the JPEG SOF marker
(FFC0/FFC1/FFC2) for the real WxH the sensor actually produced.

Usage:
    python jpegdump.py COM9 [out.jpg]           # read live from serial
    python jpegdump.py --file dump.txt [out.jpg] # parse a saved capture
Default out = ../analysis/capture_C_160x40.jpg
"""
import sys, os, re, time

OUT_DEFAULT = os.path.join(os.path.dirname(__file__),
                           "..", "analysis", "capture_C_160x40.jpg")


def parse_sof(data: bytes):
    """Return (w, h, marker) from the first SOF marker, or None."""
    i = 2  # skip SOI FFD8
    n = len(data)
    while i + 9 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        # SOF0/1/2/3, 5-7, 9-11, 13-15 are frame headers carrying dimensions.
        if marker in (0xC0, 0xC1, 0xC2, 0xC3):
            h = (data[i + 5] << 8) | data[i + 6]
            w = (data[i + 7] << 8) | data[i + 8]
            return w, h, marker
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        seg = (data[i + 2] << 8) | data[i + 3]
        i += 2 + seg
    return None


def collect_from_lines(lines):
    """Yield (declared_len, bytes) for each JPEG_START..JPEG_END block."""
    buf, declared, capturing = [], None, False
    for ln in lines:
        ln = ln.strip()
        if ln.startswith("JPEG_START"):
            m = re.search(r"len=(\d+)", ln)
            declared = int(m.group(1)) if m else None
            buf, capturing = [], True
        elif ln.startswith("JPEG_END"):
            hexstr = "".join(buf)
            try:
                data = bytes.fromhex(hexstr)
            except ValueError:
                data = b""
            yield declared, data
            capturing = False
        elif capturing and re.fullmatch(r"[0-9a-fA-F]*", ln):
            buf.append(ln)


def report(declared, data, out_path):
    print(f"  declared len = {declared}, got {len(data)} bytes")
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        print("  !! not a valid JPEG (no SOI FFD8) -- likely wrong DSP mode")
        return False
    sof = parse_sof(data)
    if sof:
        w, h, mk = sof
        print(f"  >> SOF FF{mk:02X}: REAL size = {w} x {h}")
    else:
        print("  !! no SOF marker found")
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"  saved -> {out_path}")
    return True


def main():
    args = [a for a in sys.argv[1:]]
    out = OUT_DEFAULT
    if "--file" in args:
        idx = args.index("--file")
        path = args[idx + 1]
        rest = args[idx + 2:]
        if rest:
            out = rest[0]
        with open(path, "r", errors="ignore") as f:
            lines = f.readlines()
        got = False
        for declared, data in collect_from_lines(lines):
            print("Block:")
            got = report(declared, data, out) or got
        if not got:
            print("No JPEG blocks found in file.")
        return

    if not args:
        print(__doc__)
        return
    port = args[0]
    if len(args) > 1:
        out = args[1]
    import serial  # pyserial
    ser = serial.Serial(port, 115200, timeout=2)
    print(f"Listening on {port} @115200 -- waiting for a frame (Ctrl+C to stop)")
    lines, capturing, deadline = [], False, None
    try:
        while True:
            raw = ser.readline().decode("ascii", errors="ignore")
            if not raw:
                if capturing and deadline and time.time() > deadline:
                    print("  timed out mid-frame")
                    capturing = False
                continue
            s = raw.strip()
            if s.startswith("JPEG_START"):
                capturing, lines = True, [raw]
                deadline = time.time() + 15
                print(s)
                continue
            if capturing:
                lines.append(raw)
                if s.startswith("JPEG_END"):
                    for declared, data in collect_from_lines(lines):
                        report(declared, data, out)
                    return
            elif s.startswith("DIAG") or s.startswith("==="):
                print(s)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
