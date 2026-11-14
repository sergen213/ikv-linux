#!/usr/bin/env python3
"""tm.py [PREFIX...]: GPU medians (ms) of metrics/metrics.json runs whose label starts with a PREFIX."""
import json, sys
from pathlib import Path
m = json.loads((Path(__file__).parent / 'metrics/metrics.json').read_text())
keys = ['totalMs', 'scenePassMs', 'opaqueMs', 'alphaMs', 'denoiseMs', 'fsrMs', 'presentCopyMs', 'flowMs', 'interpolateMs', 'hudMs']
print('label'.ljust(34), ' '.join(k[:-2].rjust(8) for k in keys))
for label, r in m.items():
    if len(sys.argv) > 1 and not any(label.startswith(p) for p in sys.argv[1:]): continue
    g = r.get('gpu') or {}
    print(label.replace('/vk-user','/').ljust(34), ' '.join(f"{g.get(k, float('nan')):8.3f}" for k in keys))
