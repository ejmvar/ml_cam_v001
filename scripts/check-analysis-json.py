"""Validate the bounded /analysis JSON contract without retaining images."""

import argparse
import json
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("normal", "grayscale", "reduced_colors"), default="normal")
    parser.add_argument("--quality", type=int, choices=range(10, 31), default=15)
    args = parser.parse_args()
    body = sys.stdin.buffer.read(8193)
    if len(body) == 8193:
        raise ValueError("analysis response exceeds the bounded JSON size")
    document = json.loads(body)
    assert document["mode"] == args.mode
    assert document["quality"] == args.quality
    decoded = document["decoded_pixels"]
    assert decoded["status"] in ("available", "unavailable")
    assert decoded["format"] == "RGB565"
    assert decoded["scale"] == "1:8"
    if decoded["status"] == "available":
        assert decoded["threshold_status"] == "not_defined"
        for pair_name in ("prev_to_current", "current_to_next"):
            pair = decoded[pair_name]
            for key in ("width", "height", "compared_pixels", "changed_pixels", "change_per_mille"):
                assert isinstance(pair[key], int)
            assert pair["changed_pixels"] <= pair["compared_pixels"]
            assert pair["change_per_mille"] <= 1000
        print(f"analysis JSON passed: available {decoded['prev_to_current']['width']}x{decoded['prev_to_current']['height']} RGB565")
    else:
        assert set(decoded) == {"status", "failure_stage", "format", "scale"}
        assert decoded["failure_stage"] in {
            "decoded_workspace_allocation", "jpeg_info", "decoded_bounds",
            "jpeg_decode", "decoded_output", "decoded_analysis",
        }
        print(f"analysis JSON passed: unavailable ({decoded['failure_stage']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
