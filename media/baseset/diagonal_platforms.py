# SPDX-License-Identifier: GPL-2.0-only
"""Generate the 32 indexed half-platform sprites used by DrawDiagonalRailStation."""

from math import ceil
from pathlib import Path
from struct import pack


# Indexed concrete samples at the reference's native scale, from the outside
# edge towards the lip. The two lighter greys are aggregate in the slab.
CONCRETE = tuple(tuple(int(c, 16) for c in row) for row in (
    "999999898", "88899b887", "998898999", "9b9979999",
    "989989988", "8798888b9", "998999999", "998989b87",
    "999999999", "98888899b", "999898898", "b99b97996",
    "999897669", "889764999", "b76699999", "669999999",
))
HORIZONTAL_CONCRETE = tuple(tuple(int(c, 16) for c in row) for row in (
    "9979998989998999", "b9898899b8879888", "8888999998989989", "998b98b8789b9979",
))


def rasterize(points):
    """Pixel centres in a polygon, including its top/left but not bottom/right edges."""
    for y in range(ceil(min(p[1] for p in points) - 0.5), ceil(max(p[1] for p in points) - 0.5)):
        row = y + 0.5
        crossings = []
        for (x1, y1), (x2, y2) in zip(points, points[1:] + points[:1]):
            if min(y1, y2) <= row < max(y1, y2):
                crossings.append(x1 + (row - y1) * (x2 - x1) / (y2 - y1))
        if crossings:
            for x in range(ceil(min(crossings) - 0.5), ceil(max(crossings) - 0.5)):
                yield x, y


def platform_half(vertical, second_end, positive_side, shape):
    """Work in half-tile coordinates. At a bend the shared cut lies on the diamond edge."""
    direction = 1 if second_end else -1
    side = 1 if positive_side else -1
    # Same order as the C++ sprite selector: continuation, cap, inside bend, outside bend.
    bend = (0, 0, -1, 1)[shape]

    def corner(section, distance, ordinary):
        # In these coordinates the ordinary edge has slope -bend, while the
        # diagonal edge has constant 'across'. Their intersection is the miter.
        across = distance
        along = 8 if section else 0
        if section and bend:
            # The ordinary base sprites are registered one screen pixel right
            # and half a pixel down at 1x. Account for this in their own segment.
            mouth_along = 8 + bend * ordinary + direction * 0.5
            mouth_across = ordinary + side * 0.5
            if section == 1:
                along = mouth_along + bend * (mouth_across - distance)
            else:
                along = mouth_along + 2
                across = mouth_across - 2 * bend
        x, y = (side * across, direction * along) if vertical else (direction * along, side * across)
        return (2 * x, y - 4)  # Upper surface of the built-in platform in the 2:1 projection.

    def strip(section, inner, outer, ordinary_inner, ordinary_outer):
        return [corner(section, inner, ordinary_inner), corner(section + 1, inner, ordinary_inner),
                corner(section + 1, outer, ordinary_outer), corner(section, outer, ordinary_outer)]

    pixels = {}

    def concrete(x, y):
        # Measure texture coordinates from the same rail centre for both halves.
        # Mirroring across the rail keeps aggregate and slab joints in phase.
        if vertical:
            across = max(0, min(8, int(16 - abs(x + 0.5))))
            return CONCRETE[(y + 4) % len(CONCRETE)][across]
        row = max(0, min(3, int(8 - abs(y + 4.5))))
        return HORIZONTAL_CONCRETE[row][x % 16]

    # Only external, camera-facing edges have a fascia. Never paint a transverse
    # wall at the centre cut, a through joint, or the mouth of the ordinary platform.
    sections = range(2 if bend else 1)
    for section in sections:
        top = strip(section, 3, 8, 3, 8)
        winding = 1 if sum(x * b - a * y for (x, y), (a, b) in zip(top, top[1:] + top[:1])) > 0 else -1
        for i in (0, 1, 2):
            if i == 1 and shape != 1:
                continue
            a, b = top[i], top[i + 1]
            if (a[0] - b[0]) * winding <= 0:
                continue
            face = [a, b, (b[0], b[1] + 3), (a[0], a[1] + 3)]
            for x, y in rasterize(face):
                edge_y = a[1] + (b[1] - a[1]) * (x + 0.5 - a[0]) / (b[0] - a[0])
                pixels[x, y] = 4 if y + 0.5 - edge_y < 1 else 3

    for section in sections:
        for x, y in rasterize(strip(section, 3, 8, 3, 8)):
            pixels[x, y] = concrete(x, y)
    for section in sections:
        # The ordinary edge rises one pixel per two columns. Meet that staircase
        # exactly once, with the entire straight run on a single row or column.
        for x, y in rasterize(strip(section, 3, 3.5 if vertical else 4, 3.5, 4)):
            pixels[x, y] = 13

    if shape == 1:
        # One worn transverse coping stone on an exposed end.
        for x, y in rasterize(strip(0, 3, 8, 3, 8)):
            along = (y + 4.5) if vertical else (x + 0.5) / 2
            if direction * along >= 7:
                pixels[x, y] = min(8, concrete(x, y))

    return pixels


def encode(pixels):
    left = min(x for x, y in pixels)
    top = min(y for x, y in pixels)
    width = max(x for x, y in pixels) - left + 1
    height = max(y for x, y in pixels) - top + 1
    data = bytes(pixels.get((x, y), 0) for y in range(top, top + height) for x in range(left, left + width))
    encoded = bytearray()
    for offset in range(0, len(data), 127):
        literal = data[offset:offset + 127]
        encoded.append(len(literal))
        encoded.extend(literal)
    return pack("<HBBHhh", len(data) + 8, 1, height, width, left, top) + encoded


if __name__ == "__main__":
    sprites = (encode(platform_half(vertical, end, side, shape))
               for vertical in range(2) for end in range(2) for side in range(2) for shape in range(4))
    Path(__file__).with_suffix(".grf").write_bytes(b"".join(sprites) + b"\x00\x00")
