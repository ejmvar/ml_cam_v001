"""Reset the board and print only approved Wi-Fi/HTTP checkpoint lines."""

import sys
import time

import serial


ALLOWED_MARKERS = (
    "Phase 1 flags:",
    "Wi-Fi connected and IP acquired",
    "Image checkpoint URL: http://",
    "HTTP checkpoint server ready:",
    "Camera initialized",
    "Discarded one warm-up JPEG frame",
    "Capture validation:",
    "ML transform failure:",
    "stage=",
    "worker stage=",
)


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(f"usage: {sys.argv[0]} PORT SECONDS [no-reset]", file=sys.stderr)
        return 2

    port = sys.argv[1]
    deadline = time.monotonic() + float(sys.argv[2])
    with serial.Serial(port, 115200, timeout=0.2) as connection:
        if len(sys.argv) == 3:
            connection.dtr = False
            connection.rts = True
            time.sleep(0.1)
            connection.rts = False
            connection.dtr = True
        while time.monotonic() < deadline:
            line = connection.readline().decode("utf-8", errors="replace")
            if any(marker in line for marker in ALLOWED_MARKERS):
                print(line, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
