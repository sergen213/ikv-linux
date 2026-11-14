#!/usr/bin/env python3
"""masked.py OFF1 OFF2 ON [regionset]: per region, |ON-OFF1| statistics on pixels where OFF1 and OFF2 agree within 4/255
(masks run-to-run animated particles/fire): mean, p99, share > 4 and > 8 (max channel), signed luma bias."""
import os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
os.environ.setdefault('VIEW', sys.argv[4] if len(sys.argv) > 4 else 'ring')
from compare import REGIONS
a, b, c = (np.asarray(Image.open(p).convert('RGB')).astype(np.int16) for p in sys.argv[1:4])
stable = np.abs(b - a).max(axis=2) <= 4
for name, (x0, y0, x1, y1) in {'frame': (0, 0, a.shape[1], a.shape[0]), **REGIONS}.items():
    m = stable[y0:y1, x0:x1]
    d = (c - a)[y0:y1, x0:x1][m]
    ad = np.abs(d).max(axis=1)
    luma = (d * [0.299, 0.587, 0.114]).sum(axis=1)
    print(f'{name:14s} stable={m.mean()*100:5.1f}% mean={np.abs(d).mean():.3f} p99={np.percentile(ad,99):.0f} >4={(ad>4).mean()*100:.2f}% >8={(ad>8).mean()*100:.3f}% bias={luma.mean():+.3f}')
