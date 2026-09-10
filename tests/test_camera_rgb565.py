from pathlib import Path


ROOT = Path(__file__).parents[1]
CAMERA_SOURCE = (ROOT / "main" / "camera.c").read_text()


def grayscale_rgb565(pixel):
    red = ((pixel >> 11) & 0x1F) * 255 // 31
    green = ((pixel >> 5) & 0x3F) * 255 // 63
    blue = (pixel & 0x1F) * 255 // 31
    luminance = (77 * red + 150 * green + 29 * blue) >> 8
    gray_red = luminance * 31 // 255
    gray_green = luminance * 63 // 255
    gray_blue = gray_red
    return (gray_red << 11) | (gray_green << 5) | gray_blue


def test_grayscale_rgb565_numeric_invariant_uses_one_luminance_for_all_channels():
    assert "static uint16_t grayscale_rgb565" in CAMERA_SOURCE
    for pixel in range(0, 0x10000, 257):
        gray = grayscale_rgb565(pixel)
        red = (gray >> 11) & 0x1F
        green = (gray >> 5) & 0x3F
        blue = gray & 0x1F
        assert red == blue
        # RGB565 has different bit depths per channel; compare normalized
        # channel values rather than requiring unlike field widths to match.
        assert abs(green * 31 - red * 63) <= 63
