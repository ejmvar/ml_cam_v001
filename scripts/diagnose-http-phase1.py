#!/usr/bin/env python3
"""Run one bounded, in-memory Phase 1 endpoint contract check."""

import argparse
import json
import time
import urllib.error
import urllib.request


MAX_JPEG = 8192


def jpeg_dimensions(body):
    if len(body) < 4 or len(body) > MAX_JPEG or body[:2] != b"\xff\xd8" or body[-2:] != b"\xff\xd9":
        raise ValueError("invalid bounded JPEG markers or size")
    index = 2
    while index + 1 < len(body):
        if body[index] != 0xFF:
            index += 1
            continue
        index += 1
        while index < len(body) and body[index] == 0xFF:
            index += 1
        marker = body[index]
        index += 1
        if marker in (0xD8, 0xD9, 0x01) or 0xD0 <= marker <= 0xD7:
            continue
        if index + 2 > len(body):
            break
        length = int.from_bytes(body[index:index + 2], "big")
        if length < 2 or index + length > len(body):
            break
        if marker in set(range(0xC0, 0xC4)) | set(range(0xC5, 0xC8)) | set(range(0xC9, 0xCC)) | set(range(0xCD, 0xD0)):
            return int.from_bytes(body[index + 5:index + 7], "big"), int.from_bytes(body[index + 3:index + 5], "big")
        index += length
    raise ValueError("JPEG SOF dimensions missing")


def get(base, path, timeout):
    with urllib.request.urlopen(base + path, timeout=timeout) as response:
        return response.status, dict(response.headers), response.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("device_ip")
    parser.add_argument("--quality", type=int, default=15, choices=range(10, 31))
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    base = "http://" + args.device_ip
    query = f"?mode=normal&quality={args.quality}"

    status, _, body = get(base, "/info", args.timeout)
    if status != 200:
        raise RuntimeError(f"/info returned HTTP {status}")
    info = json.loads(body)
    interval = int(info["interval_seconds"])
    if not 1 <= interval <= 60:
        raise RuntimeError("interval is outside 1..60")
    print(f"info passed: interval={interval}s modes={','.join(info['modes'])}")

    status, headers, body = get(base, "/ml/motion-edges.jpg?mode=motion_edges&quality=" + str(args.quality), args.timeout)
    if status != 200:
        raise RuntimeError(f"fresh motion edges returned HTTP {status}")
    dimensions = jpeg_dimensions(body)
    if dimensions != (16, 8) or headers.get("X-Image-Mode") != "motion_edges":
        raise RuntimeError(f"motion edge contract failed: dimensions={dimensions}")
    print(f"fresh motion edges passed: dimensions={dimensions} source={headers.get('X-Image-Source')}")

    status, headers, body = get(base, "/ml/motion-edges/latest.jpg?mode=motion_edges&quality=" + str(args.quality), args.timeout)
    if status != 200 or jpeg_dimensions(body) != (16, 8) or headers.get("X-Image-Source") != "motion-cache":
        raise RuntimeError("cached motion edge contract failed")
    print("cached motion edges passed: source=motion-cache")

    time.sleep(interval)
    status, headers, body = get(base, "/capture/periodic/latest.jpg", args.timeout)
    if status != 200 or jpeg_dimensions(body) != (320, 240):
        raise RuntimeError(f"periodic latest contract failed: HTTP {status}")
    if headers.get("X-Image-Source") != "periodic-cache" or headers.get("X-Capture-Interval-S") != str(interval):
        raise RuntimeError("periodic metadata contract failed")
    if not headers.get("X-Producer-Core", "").isdigit():
        raise RuntimeError("periodic producer core metadata missing")
    print(f"periodic latest passed: dimensions=320x240 core={headers['X-Producer-Core']}")

    for path, dimensions in (("/capture.jpg" + query, (320, 240)), ("/capture/qqvga.jpg" + query, (160, 120)), ("/capture/hqvga.jpg" + query, (240, 176))):
        status, _, body = get(base, path, args.timeout)
        if status != 200 or jpeg_dimensions(body) != dimensions:
            raise RuntimeError(f"{path} failed: HTTP {status}")
    print("normal resolution endpoints passed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        raise SystemExit(f"phase1 diagnostic failed: {error}")
