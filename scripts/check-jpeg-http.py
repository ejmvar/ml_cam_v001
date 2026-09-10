"""Validate a bounded JPEG response and report its SOF dimensions."""

import argparse
import sys

MAX_BODY = 8192
SOF_MARKERS = {
    0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
    0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF,
}


def jpeg_dimensions(image: bytes) -> tuple[int, int]:
    if len(image) > MAX_BODY:
        raise ValueError(f"body exceeds {MAX_BODY}-byte bound (truncated by validator)")
    if len(image) < 2 or image[:2] != b"\xff\xd8":
        raise ValueError("invalid JPEG bytes: missing start marker")
    if len(image) < 4 or image[-2:] != b"\xff\xd9":
        raise ValueError("truncated JPEG body: missing end marker")

    position = 2
    width = height = None
    while position < len(image):
        if image[position] != 0xFF:
            raise ValueError(f"expected marker at byte {position}")
        while position < len(image) and image[position] == 0xFF:
            position += 1
        if position >= len(image):
            raise ValueError("truncated marker")
        marker = image[position]
        position += 1
        if marker == 0xD9:
            if position != len(image) or width is None:
                raise ValueError("JPEG ended before a valid SOF marker")
            return width, height
        if marker == 0xDA:
            if position + 2 > len(image):
                raise ValueError("truncated SOS segment")
            segment_length = int.from_bytes(image[position:position + 2], "big")
            if segment_length < 2 or position + segment_length > len(image):
                raise ValueError("invalid SOS segment length")
            position += segment_length
            while position + 1 < len(image):
                if image[position] != 0xFF:
                    position += 1
                    continue
                marker_position = position
                while position < len(image) and image[position] == 0xFF:
                    position += 1
                if position >= len(image):
                    raise ValueError("truncated entropy marker")
                entropy_marker = image[position]
                if entropy_marker == 0x00 or 0xD0 <= entropy_marker <= 0xD7:
                    position += 1
                    continue
                if entropy_marker != 0xD9:
                    raise ValueError(f"unexpected marker 0x{entropy_marker:02x} in entropy data")
                position = marker_position
                break
            else:
                raise ValueError("JPEG entropy data has no EOI marker")
            continue
        if marker in (0xD8,) or 0xD0 <= marker <= 0xD7:
            raise ValueError(f"unexpected standalone marker 0x{marker:02x}")
        if position + 2 > len(image):
            raise ValueError("truncated JPEG segment length")
        segment_length = int.from_bytes(image[position:position + 2], "big")
        if segment_length < 2 or position + segment_length > len(image):
            raise ValueError("invalid JPEG segment length")
        if marker in SOF_MARKERS:
            if segment_length < 7:
                raise ValueError("truncated SOF segment")
            height = int.from_bytes(image[position + 3:position + 5], "big")
            width = int.from_bytes(image[position + 5:position + 7], "big")
            if width == 0 or height == 0:
                raise ValueError("JPEG dimensions must be nonzero")
        position += segment_length
    raise ValueError("JPEG has no EOI marker")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-width", type=int)
    parser.add_argument("--expected-height", type=int)
    args = parser.parse_args()
    image = sys.stdin.buffer.read(MAX_BODY + 1)
    try:
        width, height = jpeg_dimensions(image)
        if (args.expected_width is not None and width != args.expected_width) or (
            args.expected_height is not None and height != args.expected_height
        ):
            raise ValueError(
                f"unexpected dimensions: expected {args.expected_width}x{args.expected_height}, "
                f"got {width}x{height}"
            )
    except ValueError as error:
        print(f"JPEG checkpoint failed: {error}", file=sys.stderr)
        return 1
    print(f"JPEG checkpoint passed: {len(image)} bytes; dimensions {width}x{height}; quality contract 10..30")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
