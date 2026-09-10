"""Bounded reference implementation for the firmware focus heuristic."""


def laplacian_variance_center_roi(pixels, width, height):
    x0, y0, rw, rh = width // 4, height // 4, width // 2, height // 2
    values = []
    for y in range(y0 + 1, y0 + rh - 1):
        for x in range(x0 + 1, x0 + rw - 1):
            values.append(4 * pixels[y][x] - pixels[y - 1][x] - pixels[y + 1][x] - pixels[y][x - 1] - pixels[y][x + 1])
    mean = sum(values) / len(values)
    return sum((value - mean) ** 2 for value in values) / len(values)
