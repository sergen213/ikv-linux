#!/usr/bin/env python3
"""Wave 29 TopkapiPerf3 parity: compare screenshot pairs (2743x1515 client area); VIEW=ring|close|park selects the regions.
usage: [VIEW=ring] compare.py OUT_PREFIX LABEL_A:A.png LABEL_B:B.png [LABEL_C:C.png ...]
For every pair (first image vs each other) prints whole-frame and per-region mean absolute difference (0-255) and the
share of pixels differing by more than 8/255 (max channel), writes OUT_PREFIX-diff-<B>.png (difference x8) and a
contact sheet OUT_PREFIX-sheet.png with the regions of every image side by side (rows: regions, columns: images)."""
import os, sys
from PIL import Image, ImageChops, ImageDraw
import numpy as np

# Regions in the 2743x1515 ring capture (camera yaw 0.6 pitch 0.35 dist 46 at --at 0,285).
SETS = {'full': {'frame': (0, 0, 2743, 1515)}}
SETS['ring'] = {
    'canopy+trunk': (1312, 52, 2012, 577),
    'left tree': (0, 0, 490, 525),
    'grass': (0, 350, 1050, 875),
    'tower floor': (900, 950, 2200, 1350),
    'stone': (1730, 470, 1945, 1190),
}
# Close view (--at -8,262, camera 0.6,0.25,14): stones, near grass, trees behind, cobbles, the player character.
SETS['close'] = {
    'trees': (0, 0, 1260, 560),
    'canopy right': (1260, 0, 2743, 450),
    'grass': (0, 600, 1200, 860),
    'stone': (800, 170, 1000, 880),
    'ground': (0, 880, 980, 1400),
    'character': (990, 450, 1950, 1400),
}
REGIONS = SETS[os.environ.get('VIEW', 'ring')]


def stats(a, b):
    s = b.astype(np.int16) - a.astype(np.int16)
    d = np.abs(s)
    return float(d.mean()), float((d.max(axis=2) > 8).mean() * 100), float(s.mean())


def main():
    out = sys.argv[1]
    items = [arg.split(':', 1) for arg in sys.argv[2:]]
    imgs = [(label, Image.open(path).convert('RGB')) for label, path in items]
    base_label, base = imgs[0]
    A = np.asarray(base)
    for label, img in imgs[1:]:
        B = np.asarray(img)
        m, p, b = stats(A, B)
        row = [f'{base_label} vs {label}: frame mad={m:.3f} >8={p:.2f}% bias={b:+.3f}']
        for name, (x0, y0, x1, y1) in REGIONS.items():
            m, p, b = stats(A[y0:y1, x0:x1], B[y0:y1, x0:x1])
            row.append(f'{name} mad={m:.3f} >8={p:.2f}% bias={b:+.3f}')
        print(' | '.join(row))
        diff = ImageChops.difference(base, img).point(lambda v: min(255, v * 8))
        diff.save(f'{out}-diff-{label}.png')
    pad, label_h = 8, 28
    cols = []
    for label, img in imgs:
        tiles = [img.crop(box) for box in REGIONS.values()]
        cols.append((label, tiles))
    widths = [max(tiles[i].width for _, tiles in cols) for i in range(len(REGIONS))]
    heights = [cols[0][1][i].height for i in range(len(REGIONS))]
    W = max(widths) * len(cols) + pad * (len(cols) + 1)
    H = sum(heights) + pad * (len(REGIONS) + 1) + label_h
    sheet = Image.new('RGB', (W, H), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for c, (label, tiles) in enumerate(cols):
        x = pad + c * (max(widths) + pad)
        draw.text((x, 6), label, fill=(255, 255, 0))
        y = label_h
        for i, tile in enumerate(tiles):
            sheet.paste(tile, (x, y))
            y += heights[i] + pad
    sheet.save(f'{out}-sheet.png')


if __name__ == '__main__':
    main()
