"""Make one bounded, in-memory fresh JPEG request per normal resolution."""

import argparse
import importlib.util
import sys
import time
import urllib.error
import urllib.request

from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
_SPEC = importlib.util.spec_from_file_location("jpeg_checker", Path(__file__).parent / "check-jpeg-http.py")
assert _SPEC is not None and _SPEC.loader is not None
_CHECKER = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_CHECKER)
jpeg_dimensions = _CHECKER.jpeg_dimensions


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("device_ip")
    parser.add_argument("--quality", type=int, choices=range(10, 31), default=15)
    parser.add_argument("--timeout", type=int, choices=range(1, 61), default=10)
    args = parser.parse_args()
    endpoints = (("qqvga", "/capture/qqvga.jpg", (160, 120)),
                 ("hqvga", "/capture/hqvga.jpg", (240, 176)),
                 ("qvga", "/capture/qvga.jpg", (320, 240)))
    for name, path, expected in endpoints:
        url = f"http://{args.device_ip}{path}?quality={args.quality}"
        started = time.monotonic()
        try:
            with urllib.request.urlopen(url, timeout=args.timeout) as response:
                status = response.status
                body = response.read(8193)
        except (urllib.error.URLError, OSError) as error:
            print(f"{name}: transport failure: {error}", file=sys.stderr)
            return 1
        elapsed = (time.monotonic() - started) * 1000
        try:
            dimensions = jpeg_dimensions(body)
        except ValueError as error:
            print(f"{name}: HTTP {status}; invalid JPEG: {error}", file=sys.stderr)
            return 1
        if dimensions != expected:
            print(f"{name}: HTTP {status}; dimensions {dimensions[0]}x{dimensions[1]} != {expected[0]}x{expected[1]}", file=sys.stderr)
            return 1
        print(f"{name}: HTTP {status}; dimensions {dimensions[0]}x{dimensions[1]}; elapsed_ms={elapsed:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
