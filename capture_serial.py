#!/usr/bin/env python3
"""
capture_serial.py — Capture frank-cpc serial output and log it with timestamps.

Usage:
    python3 capture_serial.py [/dev/cu.usbmodem21301] [output.log]

The script reads all output from the device until Ctrl-C.
It prints lines to stdout and also saves them to the log file.
"""
import sys
import os
import time
import datetime

PORT  = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem21301"
LOGF  = sys.argv[2] if len(sys.argv) > 2 else "frank-cpc-capture.log"
BAUD  = 115200

def open_serial(port, baud):
    try:
        import serial
        return serial.Serial(port, baud, timeout=1)
    except ImportError:
        pass
    # Fallback: raw file-like open (works on macOS/Linux with stty pre-configured)
    import subprocess
    subprocess.run(["stty", "-f", port, str(baud), "cs8", "-cstopb", "-parenb",
                    "cread", "clocal", "raw", "-echo"], check=False)
    return open(port, "rb", buffering=0)

def main():
    print(f"Opening {PORT} at {BAUD} baud → {LOGF}")
    ser = open_serial(PORT, BAUD)

    start = time.time()
    buf   = b""
    lines_seen = 0

    with open(LOGF, "wb") as lf:
        print("Capturing (Ctrl-C to stop)...\n" + "-"*60)
        try:
            while True:
                chunk = ser.read(256)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    ts    = f"[+{time.time()-start:7.2f}s] "
                    text  = ts + line.decode("utf-8", errors="replace").rstrip("\r")
                    print(text)
                    sys.stdout.flush()
                    lf.write((text + "\n").encode())
                    lf.flush()
                    lines_seen += 1
        except KeyboardInterrupt:
            print(f"\n--- captured {lines_seen} lines → {LOGF}")

if __name__ == "__main__":
    main()
