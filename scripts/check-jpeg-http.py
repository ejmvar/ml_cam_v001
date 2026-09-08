"""Validate a JPEG response received on stdin without writing it to disk."""

import sys


def main() -> int:
    image = sys.stdin.buffer.read()
    if len(image) < 4 or image[:2] != b"\xff\xd8" or image[-2:] != b"\xff\xd9":
        print("JPEG checkpoint failed: missing JPEG start/end markers", file=sys.stderr)
        return 1
    print(f"JPEG checkpoint passed: {len(image)} bytes; SOI/EOI markers present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
