"""Classify the final HTTP response status captured by wget --server-response."""

import re
import sys


def main() -> int:
    text = sys.stdin.read()
    statuses = re.findall(r"^\s*HTTP/\S+\s+(\d{3})(?:\s|$)", text, re.MULTILINE)
    if not statuses:
        print("HTTP status unavailable: wget did not receive a response", file=sys.stderr)
        return 3
    status = int(statuses[-1])
    if not 200 <= status < 300:
        print(f"HTTP error response: {status}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
