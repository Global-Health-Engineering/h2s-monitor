#!/usr/bin/env python3
"""
Captures DATA lines from the H2S sensor over USB serial and saves them as CSV.
Usage: python3 capture_serial.py [port] [output.csv]
       python3 capture_serial.py          # auto-detects port, names file by timestamp
"""

import serial
import serial.tools.list_ports
import sys
import os
from datetime import datetime


def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "usbmodem" in p.device or "usbserial" in p.device or "COM" in p.device:
            return p.device
    if ports:
        return ports[0].device
    return None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        print("ERROR: No serial port found. Pass port as first argument.")
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    data_dir = os.path.join(script_dir, "..", "data")
    os.makedirs(data_dir, exist_ok=True)
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(data_dir, f"h2s_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv")

    header = "datetime,timestamp_ms,ppm,temperature_c,battery_mv,status\n"

    print(f"Port:   {port}")
    print(f"Output: {out_path}")
    print("Waiting for DATA lines (Ctrl+C to stop)...\n")

    with serial.Serial(port, 115200, timeout=1) as ser, \
         open(out_path, "w") as f:
        f.write(header)
        f.flush()
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="ignore").strip()
                if line.startswith("DATA,"):
                    csv_line = line[5:] + "\n"
                    f.write(csv_line)
                    f.flush()
                    print(csv_line, end="")
                else:
                    print(f"  [{line}]")  # show other serial output in brackets
        except KeyboardInterrupt:
            print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
