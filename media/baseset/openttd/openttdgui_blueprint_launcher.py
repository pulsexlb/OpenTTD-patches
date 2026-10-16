#!/usr/bin/env python3
"""Draw openttdgui_blueprint_launcher.png: the Blueprint toolbar opener icon.

Two vertical railway tracks with a thick rainbow arrow pointing from the dark
left track into the light right one. Drawn at the sprite's native 20x20 rather
than shrunk from a larger image: stock OpenTTD toolbar icons are near-solid
blocks (the stock rail icon fills 304 of its 320 pixels), so line art or
downscaled artwork reads undersized on the button. This sits at 316/400 and
needs no resampling at all.

openttdgui_blueprint.py picks the result up as sprite 15 of the icon sheet.
Run from this directory, then regenerate the sheet:

    python3 openttdgui_blueprint_launcher.py
    python3 openttdgui_blueprint.py
"""

import os
import sys

from PIL import Image

SIZE = 20
TRANSPARENT = (0, 0, 0, 0)
OUTLINE = (16, 16, 16, 255)

# Left (dark) track.
D_RAIL = (168, 168, 168, 255)
D_BED = (56, 40, 32, 255)
D_TIE = (104, 56, 32, 255)
# Right (light) track. Warm tint on purpose: a neutral grey would look like a
# backdrop to any background-removal pass run over the artwork.
L_RAIL = (236, 228, 200, 255)
L_BED = (168, 148, 116, 255)
L_TIE = (204, 176, 136, 255)

RAINBOW = [(216, 32, 32, 255), (240, 140, 16, 255), (248, 220, 48, 255),
           (48, 160, 48, 255), (64, 112, 208, 255), (150, 60, 180, 255)]

# Arrow geometry: six stripe rows, shaft in the gap between the tracks, head
# reaching into the right-hand track so it clearly points from left to right.
TOP = 7
SHAFT_X0, HEAD_X0, TIP_X = 6, 12, 16


def draw_track(px, x0, rail, bed, tie):
    """A 6px wide vertical track: outline, two rails, sleepers every 3 rows."""
    for y in range(SIZE):
        sleeper = (y % 3 == 1)
        for i, x in enumerate(range(x0, x0 + 6)):
            if i in (0, 5):
                px[x, y] = OUTLINE
            elif sleeper:
                px[x, y] = tie
            elif i in (1, 4):
                px[x, y] = rail
            else:
                px[x, y] = bed


def draw_icon():
    img = Image.new("RGBA", (SIZE, SIZE), TRANSPARENT)
    px = img.load()

    draw_track(px, 0, D_RAIL, D_BED, D_TIE)
    draw_track(px, 14, L_RAIL, L_BED, L_TIE)

    for i, colour in enumerate(RAINBOW):
        for x in range(SHAFT_X0, HEAD_X0):
            px[x, TOP + i] = colour

    centre = TOP + 2.5
    for x in range(HEAD_X0, TIP_X + 1):
        f = (x - HEAD_X0) / (TIP_X - HEAD_X0)
        half = (1 - f) * 4.5
        y0, y1 = round(centre - half), round(centre + half)
        span = max(1, y1 - y0 + 1)
        for y in range(y0, y1 + 1):
            if 0 <= y < SIZE:
                # Spread the six stripes across the head's height so the rainbow
                # continues smoothly out of the shaft instead of clamping.
                idx = min(len(RAINBOW) - 1, max(0, int((y - y0) / span * len(RAINBOW))))
                px[x, y] = RAINBOW[idx]

    # Outline the arrow so it reads against both tracks.
    arrow = {(x, y) for y in range(SIZE) for x in range(SIZE) if px[x, y] in RAINBOW}
    for (x, y) in arrow:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < SIZE and 0 <= ny < SIZE and (nx, ny) not in arrow:
                    px[nx, ny] = OUTLINE
    return img


def main():
    img = draw_icon()
    default = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "openttdgui_blueprint_launcher.png")
    out = sys.argv[1] if len(sys.argv) > 1 else default
    img.save(out)
    opaque = sum(1 for p in img.get_flattened_data() if p[3] > 0)
    print(f"wrote {out} ({opaque}/{SIZE * SIZE} pixels opaque)")


if __name__ == "__main__":
    main()
