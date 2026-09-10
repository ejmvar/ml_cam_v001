#!/usr/bin/env python3
"""Make one bounded HTTP request and classify transport, status, or body failure."""

import argparse
import socket
import subprocess
import sys
import urllib.error
import urllib.request


MAX_BODY = 8192


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("kind", choices=("health", "analysis", "jpeg"))
    parser.add_argument("--mode", choices=("normal", "grayscale", "reduced_colors"), default="normal")
    parser.add_argument("--quality", type=int, choices=range(10, 31), default=15)
    parser.add_argument("--expected-width", type=int)
    parser.add_argument("--expected-height", type=int)
    parser.add_argument("--timeout", type=int, choices=range(1, 61), default=10)
    args = parser.parse_args()

    try:
        with urllib.request.urlopen(args.url, timeout=args.timeout) as response:
            status = response.status
            body = response.read(MAX_BODY + 1)
    except urllib.error.HTTPError as error:
        print(f"HTTP status failure: {error.code} {error.reason}", file=sys.stderr)
        return 2
    except urllib.error.URLError as error:
        reason = error.reason
        if isinstance(reason, socket.gaierror):
            category = "DNS"
        elif isinstance(reason, (socket.timeout, TimeoutError)):
            category = "timeout"
        else:
            category = "TCP/ARP/network"
        print(f"transport failure ({category}): {reason}", file=sys.stderr)
        return 3
    except (socket.timeout, TimeoutError) as error:
        print(f"transport failure (timeout): {error}", file=sys.stderr)
        return 3
    except OSError as error:
        print(f"transport failure (TCP/ARP/network): {error}", file=sys.stderr)
        return 3

    if status < 200 or status >= 300:
        print(f"HTTP status failure: {status}", file=sys.stderr)
        return 2
    if len(body) > MAX_BODY:
        print(f"body contract failure: response exceeds {MAX_BODY}-byte bound", file=sys.stderr)
        return 4

    checker = {
        "health": "check-health-json.py",
        "analysis": "check-analysis-json.py",
        "jpeg": "check-jpeg-http.py",
    }[args.kind]
    command = [sys.executable, f"scripts/{checker}"]
    if args.kind != "jpeg":
        command.extend(("--mode", args.mode, "--quality", str(args.quality)))
    else:
        width = args.expected_width if args.expected_width is not None else (320 if args.mode == "normal" else 40)
        height = args.expected_height if args.expected_height is not None else (240 if args.mode == "normal" else 24)
        command.extend(("--expected-width", str(width), "--expected-height", str(height)))
    result = subprocess.run(command, input=body, capture_output=True, timeout=args.timeout)
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        print(f"{args.kind} contract failure: {detail or 'validator rejected response'}", file=sys.stderr)
        return 4
    print(result.stdout.decode("utf-8", errors="replace").strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
