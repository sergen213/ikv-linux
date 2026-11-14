#!/usr/bin/env python3
"""walk.py STYLE VIEW [A=off1 B=on N=off2]: walk-sequence parity (parity.py --walk shots every 2 ticks). Per tick: mean |B-A|
and |N-A| (run-to-run noise: animation, particles), share of pixels > 8/255, plus a contact sheet parity/cmp/<style>-<view>-walk.png
(rows: ticks 116..164 every 8; columns A, B, |B-A| x4) at half resolution, for judging motion ghosting by eye."""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageChops

D = Path(__file__).resolve().parent / 'parity'
style, view = sys.argv[1], sys.argv[2]
a_run, b_run, n_run = (sys.argv[3:6] + ['off1', 'on', 'off2'][len(sys.argv[3:6]):])[:3]
load = lambda run, t: D / f'{style}-{view}-{run}-t{t}.png'
rows = []
for t in range(110, 172, 2):
    if not all(load(r, t).exists() for r in (a_run, b_run, n_run)):
        continue
    A, B, N = (np.asarray(Image.open(load(r, t)).convert('RGB')).astype(np.int16) for r in (a_run, b_run, n_run))
    d, n = np.abs(B - A), np.abs(N - A)
    print(f't{t}: {b_run} mad={d.mean():.3f} >8={(d.max(axis=2) > 8).mean()*100:.2f}% | {n_run} mad={n.mean():.3f} >8={(n.max(axis=2) > 8).mean()*100:.2f}%')
    if t % 8 == 4:
        rows.append(t)
tiles = []
for t in rows:
    A, B = (Image.open(load(r, t)).convert('RGB') for r in (a_run, b_run))
    diff = ImageChops.difference(A, B).point(lambda v: min(255, v * 4))
    tiles.append([im.resize((im.width // 2, im.height // 2)) for im in (A, B, diff)])
if tiles:
    w, h = tiles[0][0].size
    sheet = Image.new('RGB', (3 * w + 20, len(tiles) * (h + 10)), (24, 24, 24))
    for i, row in enumerate(tiles):
        for j, im in enumerate(row):
            sheet.paste(im, (j * (w + 10), i * (h + 10)))
    (D / 'cmp').mkdir(exist_ok=True)
    sheet.save(D / 'cmp' / f'{style}-{view}-walk.png')
