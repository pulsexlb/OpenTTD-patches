#!/usr/bin/env python3
"""Generate openttdgui_blueprint.png: the 20 Blueprint toolbar icons.

The sprites are 20x20 pixels, 8bpp, using the DOS palette taken from
openttdgui.png (palette index 0 = transparent). Sprite i sits at
(i * 24 + 2, 2), matching the lines appended to openttdgui.nfo.

Sprite order:
  0 copy, 1 paste, 2 rotate ccw, 3 rotate cw, 4 reflect NW-SE,
  5 reflect NE-SW, 6..13 transformation indicator (rotation 0..3,
  then the same four mirrored), 14 mirror signals, 15 toolbar launcher
  (converted from the openttdgui_blueprint_launcher.png artwork, used by
  the main/landscaping toolbar button that opens the blueprint toolbar),
  16 undo last paste, 17 undo history, 18 export/import menu,
  19 paste-options menu.

Run from this directory: python3 openttdgui_blueprint.py
"""

import math
import os

from PIL import Image

SIZE = 20    # sprite size
PITCH = 24   # cell pitch in the sheet

DIR = os.path.dirname(os.path.abspath(__file__))

# Colours as RGB; mapped to the nearest static DOS palette index below.
COLOURS = {
    'K': (16, 16, 16),     # outline
    'd': (80, 80, 80),     # dark grey
    'g': (132, 132, 132),  # mid grey
    'G': (184, 184, 184),  # light grey
    'W': (220, 220, 220),  # near-white (pure white 255 upsets nforenum)
    'N': (44, 152, 44),    # green
    'R': (216, 32, 32),    # red
    'O': (232, 156, 16),   # orange (mirrored transformations)
    'B': (72, 112, 200),   # blue (plain transformations)
}


def build_palette_lookup(palette):
    """Map each legend character to the nearest palette index, skipping
    index 0 (transparent) and the animated colour cycles (227..254)."""
    usable = [i for i in range(1, 256) if not 227 <= i <= 254]
    lookup = {}
    for char, (r, g, b) in COLOURS.items():
        best = min(usable, key=lambda i: (palette[i * 3] - r) ** 2 +
                   (palette[i * 3 + 1] - g) ** 2 + (palette[i * 3 + 2] - b) ** 2)
        lookup[char] = best
    return lookup


def sprite_from_grid(grid, lookup):
    """Turn a list of 20 strings into a 20x20 'P' image ('.' = transparent)."""
    assert len(grid) == SIZE and all(len(row) == SIZE for row in grid), 'grid must be 20x20'
    img = Image.new('P', (SIZE, SIZE), 0)
    px = img.load()
    for y, row in enumerate(grid):
        for x, char in enumerate(row):
            if char != '.':
                px[x, y] = lookup[char]
    return img


def outline(img, lookup):
    """Draw a 1px dark outline around all non-transparent pixels."""
    px = img.load()
    edge = []
    for y in range(SIZE):
        for x in range(SIZE):
            if px[x, y] != 0:
                continue
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < SIZE and 0 <= ny < SIZE and px[nx, ny] not in (0, lookup['K']):
                        edge.append((x, y))
                        break
                else:
                    continue
                break
    for x, y in edge:
        px[x, y] = lookup['K']
    return img


COPY = [
    '....................',
    '..KKKKKKKKK.........',
    '..KWWWWWWWK.........',
    '..KWWWWWWWK.........',
    '..KWggggWWK.........',
    '..KWWWWWWWK.........',
    '..KWWKKKKKKKKKKK....',
    '..KWWKWWWWWWWWWK....',
    '..KWgKWWWWWWWWWK....',
    '..KWWKWWggggWWWK....',
    '..KWWKWWWWWWWWWK....',
    '..KWWKWWggggggWK....',
    '..KWWKWWWWWWWWWK....',
    '..KKKKWWggggWWWK....',
    '.....KWWWWWWWWWK....',
    '.....KWWggggggWK....',
    '.....KWWWWWWWWWK....',
    '.....KKKKKKKKKKK....',
    '....................',
    '....................',
]

PASTE = [
    '....................',
    '........KKKK........',
    '...KKKKKKddKKKKKK...',
    '...KggggKKKKggggK...',
    '...KggggggggggggK...',
    '...KgWWWWWWWWWWgK...',
    '...KgWWWWNNWWWWgK...',
    '...KgWWWWNNWWWWgK...',
    '...KgWWWWNNWWWWgK...',
    '...KgWWWWNNWWWWgK...',
    '...KgWWWWNNWWWWgK...',
    '...KgWWNNNNNNWWgK...',
    '...KgWWWNNNNWWWgK...',
    '...KgWWWWNNWWWWgK...',
    '...KgWWWWWWWWWWgK...',
    '...KgWWWWWWWWWWgK...',
    '...KgWWWWWWWWWWgK...',
    '...KggggggggggggK...',
    '...KKKKKKKKKKKKKK...',
    '....................',
]

def make_toolbar_launcher(palette):
    """The launcher icon, converted from the player-provided artwork in
    openttdgui_blueprint_launcher.png: cropped to its opaque content, then
    scaled to the largest size that fits 20x20 without distorting the
    artwork's proportions and centred, and mapped to the nearest usable
    palette index. Cropping first is what makes the icon fill the button
    like the stock toolbar icons - any transparent padding around the
    supplied artwork is dropped instead of shrinking the picture. Near-white
    pixels connected to the border then become transparent so the artwork
    sits directly on the button face."""
    art = Image.open(os.path.join(DIR, 'openttdgui_blueprint_launcher.png')).convert('RGBA')
    content = art.getchannel('A').getbbox()
    if content is not None:
        art = art.crop(content)
    # Fit the longer side to 20 so the icon is as large as possible while
    # preserving the artwork's aspect ratio, then centre it on a transparent
    # square.
    cw, ch = art.size
    scale = SIZE / max(cw, ch)
    art = art.resize((max(1, round(cw * scale)), max(1, round(ch * scale))), Image.Resampling.LANCZOS)
    canvas = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    canvas.paste(art, ((SIZE - art.width) // 2, (SIZE - art.height) // 2), art)
    art = canvas
    rgba = art.load()

    # Anything the artwork itself leaves transparent stays transparent, wherever
    # it sits: background is often visible *through* a subject (the gaps between
    # a railway track's sleepers), and those enclosed holes must not be filled in.
    transparent = {(x, y) for y in range(SIZE) for x in range(SIZE) if rgba[x, y][3] < 128}

    def near_white(x, y):
        r, g, b, a = rgba[x, y]
        return a < 128 or (r >= 240 and g >= 240 and b >= 240)

    # Opaque near-white is only treated as background where it is reachable from
    # the border, so white *inside* the artwork survives.
    stack = [(x, y) for x in range(SIZE) for y in (0, SIZE - 1) if near_white(x, y)]
    stack += [(x, y) for y in range(SIZE) for x in (0, SIZE - 1) if near_white(x, y)]
    while stack:
        x, y = stack.pop()
        if (x, y) in transparent:
            continue
        transparent.add((x, y))
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < SIZE and 0 <= ny < SIZE and (nx, ny) not in transparent and near_white(nx, ny):
                stack.append((nx, ny))

    # Nearest usable palette index: no transparent 0, no animated cycle, no pure white 255.
    usable = [i for i in range(1, 255) if not 227 <= i <= 254]
    cache = {}
    img = Image.new('P', (SIZE, SIZE), 0)
    px = img.load()
    for y in range(SIZE):
        for x in range(SIZE):
            if (x, y) in transparent:
                continue
            r, g, b, _ = rgba[x, y]
            key = (r, g, b)
            if key not in cache:
                cache[key] = min(usable, key=lambda i: (palette[i * 3] - r) ** 2 +
                                 (palette[i * 3 + 1] - g) ** 2 + (palette[i * 3 + 2] - b) ** 2)
            px[x, y] = cache[key]
    return img

MIRROR_SIGNALS = [
    '....................',
    '..KKKKK......KKKKK..',
    '..KdddK......KdddK..',
    '..KNNNK......KRRRK..',
    '..KNNNK......KRRRK..',
    '..KNNNK......KRRRK..',
    '..KdddK......KdddK..',
    '..KKKKK......KKKKK..',
    '...GG..........GG...',
    '...GG.....KK...GG...',
    '...GG.KKKKKKK..GG...',
    '...GG.....KK...GG...',
    '...GG...KK.....GG...',
    '...GG.KKKKKKK..GG...',
    '...GG...KK.....GG...',
    '...GG..........GG...',
    '...GG..........GG...',
    '...GG..........GG...',
    '....................',
    '....................',
]

# The transformation indicator glyph: a bold 'F' ('X' = fill, coloured later).
GLYPH_F = [
    '....................',
    '....................',
    '....................',
    '....................',
    '......XXXXXXXX......',
    '......XXXXXXXX......',
    '......XXXXXXXX......',
    '......XXX...........',
    '......XXX...........',
    '......XXXXXXX.......',
    '......XXXXXXX.......',
    '......XXX...........',
    '......XXX...........',
    '......XXX...........',
    '......XXX...........',
    '......XXX...........',
    '....................',
    '....................',
    '....................',
    '....................',
]


# Export/import: a blueprint leaving the game (green, up) and coming back into
# it (blue, down), over the shelf they are filed on. Two opposing arrows is the
# only part of this that has to read at 20x20; the colours separate the two
# directions for anyone who cannot tell the arrowheads apart at that size.
EXPORT_IMPORT = [
    '....................',
    '....................',
    '....NN........BB....',
    '...NNNN.......BB....',
    '..NNNNNN......BB....',
    '.NNNNNNNN.....BB....',
    '....NN........BB....',
    '....NN........BB....',
    '....NN........BB....',
    '....NN........BB....',
    '....NN.....BBBBBBBB.',
    '....NN......BBBBBB..',
    '....NN.......BBBB...',
    '....NN........BB....',
    '....................',
    '.gggggggggggggggggg.',
    '.gggggggggggggggggg.',
    '....................',
    '....................',
    '....................',
]

# Paste options: three sliders at different settings. The filters and the
# terraform mode used to be nine buttons in a row; as one menu they need a
# glyph that says "settings behind this button" rather than naming any one of
# them, and sliders say that without borrowing the stock gear icon, which
# already means the game options.
PASTE_OPTIONS = [
    '....................',
    '....................',
    '...........WWW......',
    '...........WWW......',
    '..gggggggggWWWgggg..',
    '...........WWW......',
    '...........WWW......',
    '.....WWW............',
    '.....WWW............',
    '..gggWWWgggggggggg..',
    '.....WWW............',
    '.....WWW............',
    '.............WWW....',
    '.............WWW....',
    '..gggggggggggWWWgg..',
    '.............WWW....',
    '.............WWW....',
    '....................',
    '....................',
    '....................',
]


def make_rotate_ccw(lookup):
    """Three-quarter ring with an anticlockwise arrowhead at the top left."""
    img = Image.new('P', (SIZE, SIZE), 0)
    px = img.load()
    cx = cy = 9.5
    for y in range(SIZE):
        for x in range(SIZE):
            dist = math.hypot(x - cx, y - cy)
            if not 4.7 <= dist <= 6.7:
                continue
            angle = math.degrees(math.atan2(cy - y, x - cx)) % 360
            if 95 <= angle <= 185:
                continue  # gap for the arrowhead
            px[x, y] = lookup['K']
    # Arrowhead pointing left, apex at (1, 4), base at the upper ring end.
    for x in range(1, 8):
        half = (x - 1) * 2 // 3
        for y in range(4 - half, 4 + half + 1):
            if 0 <= y < SIZE:
                px[x, y] = lookup['K']
    return img


UNDO_LIST = [
    '....................',
    '.KKKKKKKKKKKKKKKKKK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KWddddddddddddddWK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KWddddddddddddWWWK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KWBBBBBBBBBBBBBBWK.',
    '.KWBBBBBBBBBBBBBBWK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KWdddddddddddddWWK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KWdddddddddddWWWWK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KWddddddddddddddWK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KWdddddddddWWWWWWK.',
    '.KWWWWWWWWWWWWWWWWK.',
    '.KKKKKKKKKKKKKKKKKK.',
    '....................',
]

def make_undo(lookup):
    """A blueprint page inside an anticlockwise arrow: undo the last paste.

    The arrow is the universal undo glyph; the page is what says *which* undo
    this is. That matters because the button also sits on the construction
    toolbars, where a bare arrow reads as "undo the track I just laid" - which
    is not something this feature can do, and did mislead in testing.
    """
    img = Image.new('P', (SIZE, SIZE), 0)
    px = img.load()

    # The page: a white sheet, outlined dark so it reads as a sheet rather than
    # as a grey smudge inside the arrow, with a few ruled lines.
    for y in range(6, 14):
        for x in range(6, 14):
            px[x, y] = lookup['W']
    for y in range(6, 14):
        px[6, y] = px[13, y] = lookup['K']
    for x in range(6, 14):
        px[x, 6] = px[x, 13] = lookup['K']
    for y in (8, 10, 12):
        for x in range(8, 12):
            px[x, y] = lookup['d']

    # Three-quarter ring around the page, with a gap at the top left for the head.
    cx = cy = 9.5
    for y in range(SIZE):
        for x in range(SIZE):
            dist = math.hypot(x - cx, y - cy)
            if not 7.3 <= dist <= 8.9:
                continue
            angle = math.degrees(math.atan2(cy - y, x - cx)) % 360
            if 100 <= angle <= 190:
                continue
            px[x, y] = lookup['R']

    # Arrowhead closing the ring anticlockwise: apex at the left, base meeting
    # the ring end near the top.
    for i, x in enumerate(range(2, 9)):
        half = min(i * 2 // 3, 3)
        for y in range(3 - half, 3 + half + 1):
            if 0 <= y < SIZE:
                px[x, y] = lookup['R']
    return img


def make_reflect_nwse(lookup):
    """Square cut by the NW-SE diagonal: solid original, hollow mirror image."""
    img = Image.new('P', (SIZE, SIZE), 0)
    px = img.load()
    lo, hi = 3, 16
    for i in range(lo, hi + 1):
        px[i, lo] = px[i, hi] = px[lo, i] = px[hi, i] = lookup['K']
        px[i, i] = lookup['K']
    for y in range(lo + 1, hi):
        for x in range(lo + 1, hi):
            if y > x:
                px[x, y] = lookup['g']
    return img


def make_transform(reflected, rotation, lookup):
    """Transformation indicator: the F glyph, mirrored first, then rotated."""
    colour = 'O' if reflected else 'B'
    grid = [row.replace('X', colour) for row in GLYPH_F]
    img = outline(sprite_from_grid(grid, lookup), lookup)
    if reflected:
        img = img.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    for _ in range(rotation):
        img = img.transpose(Image.Transpose.ROTATE_90)  # anticlockwise
    return img


def main():
    source = Image.open(os.path.join(DIR, 'openttdgui.png'))
    palette = source.getpalette()
    lookup = build_palette_lookup(palette)

    sprites = [
        sprite_from_grid(COPY, lookup),
        sprite_from_grid(PASTE, lookup),
    ]
    ccw = make_rotate_ccw(lookup)
    sprites.append(ccw)
    sprites.append(ccw.transpose(Image.Transpose.FLIP_LEFT_RIGHT))
    nwse = make_reflect_nwse(lookup)
    sprites.append(nwse)
    sprites.append(nwse.transpose(Image.Transpose.FLIP_LEFT_RIGHT))
    for reflected in (False, True):
        for rotation in range(4):
            sprites.append(make_transform(reflected, rotation, lookup))
    sprites.append(sprite_from_grid(MIRROR_SIGNALS, lookup))
    sprites.append(make_toolbar_launcher(palette))
    sprites.append(make_undo(lookup))
    sprites.append(sprite_from_grid(UNDO_LIST, lookup))
    sprites.append(outline(sprite_from_grid(EXPORT_IMPORT, lookup), lookup))
    sprites.append(outline(sprite_from_grid(PASTE_OPTIONS, lookup), lookup))

    sheet = Image.new('P', (len(sprites) * PITCH, PITCH), 255)
    sheet.putpalette(palette)
    for i, sprite in enumerate(sprites):
        box = Image.new('P', (SIZE, SIZE), 0)
        box.paste(sprite)
        sheet.paste(box, (i * PITCH + 2, 2))

    out = os.path.join(DIR, 'openttdgui_blueprint.png')
    sheet.save(out, optimize=False)
    print(f'wrote {out} ({len(sprites)} sprites)')


if __name__ == '__main__':
    main()
