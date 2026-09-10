"""Print only approved, secret-free checkpoint lines from a serial monitor."""

import sys


ALLOWED_MARKERS = (
    "Phase 1 flags:",
    "Wi-Fi connected and IP acquired",
    "Image checkpoint URL: http://",
    "HTTP checkpoint server ready:",
    "Camera initialized",
    "Discarded one warm-up JPEG frame",
    "Capture validation:",
    "ML transform failure:",
)


def main() -> int:
    for line in sys.stdin:
        if any(marker in line for marker in ALLOWED_MARKERS):
            print(line, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
