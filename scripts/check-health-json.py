"""Validate the lightweight health response without writing it to disk."""

import argparse
import json
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("normal", "grayscale", "reduced_colors"), default="normal")
    parser.add_argument("--quality", type=int, choices=range(10, 31), default=15)
    args = parser.parse_args()
    body = sys.stdin.buffer.read(1024)
    if not body or len(body) == 1024:
        raise ValueError("health response is empty or exceeds the bounded JSON size")
    document = json.loads(body)
    expected = {"status": "ready", "mode": args.mode, "quality": args.quality}
    if document != expected:
        raise ValueError(f"unexpected health contract: {document!r}")
    print(f"health JSON passed: ready, {args.mode}, quality {args.quality}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, json.JSONDecodeError) as error:
        print(f"health JSON contract failed: {error}", file=sys.stderr)
        raise SystemExit(1)
