#!/usr/bin/env python3
"""NONCANONICAL wave 29: build the enhanced-appearance PBR texture pack (assets/pbr).

The pack is never derived from original game content: every image comes from a CC0
ambientCG set listed in assets/pbr/sources.json. The original installation is only read
(never written) by the `inventory` and `sheets` commands, which list the material keys the
renderer looks up and render contact sheets used to classify them by eye.

  pbr_pack.py inventory ORIGINAL_BASE OUT.json      keys + maps + colour statistics
  pbr_pack.py sheets ORIGINAL_BASE OUT_DIR [--manifest M]  contact sheets (per family)
  pbr_pack.py build [--pack assets/pbr] [--cache DIR]      download, convert, write SOURCES.md
  pbr_pack.py check [--pack assets/pbr]                    validate manifest/files/size

Key rule (renderer contract, include/ikv/renderer.hpp): Material::diffuse as scene.cpp
normalises it: "textures/" + authored name, lowercase, '/' separators.
Albedo contract: albedo.png is detail-normalised: neutral mean, mean linear luminance 0.18;
the renderer multiplies it by the blurred original diffuse / 0.18.
"""
import argparse
import hashlib
import io
import json
import pathlib
import struct
import sys
import urllib.request
import zipfile

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from asset_adf import archive, chunks  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
PACK = REPO / 'assets' / 'pbr'
CACHE = pathlib.Path.home() / '.cache' / 'ikv-pbr'
ACG_ZIP = 'https://ambientcg.com/get?file={id}_1K-PNG.zip'
ACG_PAGE = 'https://ambientcg.com/view?id={id}'
MEAN_LUMINANCE = 0.18
LUMA = np.array([0.2126, 0.7152, 0.0722])


# ---------------------------------------------------------------- original (read-only)
def texture_index(base):
    """lowercase 'textures/x.dds' -> (archive path, offset, size); first archive wins."""
    found = {}
    for n in ('1', '2', '4'):
        path = base / f'textures_{n}.DAT'
        for name, offset, size in archive(path):
            found.setdefault(name.replace('\\', '/').lower(), (path, offset, size))
    return found


def material_keys(base):
    """Diffuse keys of every zone map and location prop (objects/maps, objects/loc)."""
    keys = {}
    path = base / 'objects.dat'
    with path.open('rb') as f:
        for name, offset, size in archive(path):
            low = name.lower()
            if not low.startswith(('objects/maps/', 'objects/loc/')) or 'particles' in low:
                continue
            f.seek(offset)
            for tag, _, b in chunks(f.read(size)):
                if tag != 'mate':
                    continue
                for p in range(0, len(b), 468):
                    if not struct.unpack_from('<I', b, p + 4)[0] & 1:
                        continue
                    text = b[p + 20:p + 84].split(b'\0')[0].decode('latin1')
                    key = ('textures/' + text).replace('\\', '/').lower()
                    keys.setdefault(key, set()).add(low.rsplit('/', 1)[1].split('.')[0])
    return keys


def decode_original(entry):
    path, offset, size = entry
    with open(path, 'rb') as f:
        f.seek(offset)
        image = Image.open(io.BytesIO(f.read(size)))
        image.load()
    return image.convert('RGBA')


def cmd_inventory(args):
    base = pathlib.Path(args.base).resolve()
    out = pathlib.Path(args.out).resolve()
    if out.is_relative_to(base):
        raise SystemExit('output must be outside the original installation')
    keys, textures = material_keys(base), texture_index(base)
    report = {}
    for key, maps in sorted(keys.items()):
        item = {'maps': sorted(maps), 'present': key in textures}
        if key in textures:
            rgba = np.asarray(decode_original(textures[key])).astype(np.float32) / 255
            item.update(width=rgba.shape[1], height=rgba.shape[0],
                        mean_srgb=[round(float(v), 4) for v in rgba[..., :3].reshape(-1, 3).mean(0)],
                        alpha_mean=round(float(rgba[..., 3].mean()), 3))
        report[key] = item
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=1) + '\n')
    print(f'{out}: {len(report)} keys, {sum(v["present"] for v in report.values())} present')


def cmd_sheets(args):
    base = pathlib.Path(args.base).resolve()
    out = pathlib.Path(args.out).resolve()
    if out.is_relative_to(base):
        raise SystemExit('output must be outside the original installation')
    manifest = json.loads(pathlib.Path(args.manifest).read_text())
    textures = texture_index(base)
    keys = sorted(material_keys(base))
    groups = {}
    for key in keys:
        groups.setdefault(manifest['textures'].get(key, '_unmapped'), []).append(key)
    out.mkdir(parents=True, exist_ok=True)
    for family, members in sorted(groups.items()):
        members = [k for k in members if k in textures]
        cols = 8
        for page in range(0, len(members), 48):
            chunk = members[page:page + 48]
            sheet = Image.new('RGB', (cols * 136, ((len(chunk) + cols - 1) // cols) * 150), (40, 40, 40))
            draw = ImageDraw.Draw(sheet)
            for j, key in enumerate(chunk):
                x, y = (j % cols) * 136 + 4, (j // cols) * 150 + 4
                sheet.paste(decode_original(textures[key]).convert('RGB').resize((128, 128)), (x, y))
                draw.text((x, y + 129), key[9:-4][:21], fill=(255, 255, 0))
            sheet.save(out / f'{family}_{page // 48}.png')
    print(f'{out}: {len(groups)} families')


# ---------------------------------------------------------------- pack build
def fetch(asset, cache):
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / f'{asset}_1K-PNG.zip'
    if not path.exists():
        request = urllib.request.Request(ACG_ZIP.format(id=asset), headers={'User-Agent': 'ikv-pbr-pack'})
        data = urllib.request.urlopen(request, timeout=300).read()
        zipfile.ZipFile(io.BytesIO(data)).testzip()
        path.write_bytes(data)
    return zipfile.ZipFile(path)


def member(zf, suffix):
    names = [n for n in zf.namelist() if n.endswith(f'_{suffix}.png')]
    if not names:
        return None
    image = Image.open(io.BytesIO(zf.read(names[0])))
    image.load()
    return image


def square(image, size):
    """Square tile: a seamless 1:n source is repeated n times along its short side (keeps aspect)."""
    w, h = image.size
    if w != h:
        if max(w, h) % min(w, h):
            raise ValueError(f'source aspect not 1:n {image.size}')
        n = max(w, h) // min(w, h)
        reps = (1, n) if w < h else (n, 1)
        a = np.asarray(image)
        image = Image.fromarray(np.tile(a, reps + (1,) * (a.ndim - 2)))
    return image if image.width == size else image.resize((size, size), Image.LANCZOS)


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def detail_albedo(image, size, ao=None, contrast=1.0):
    """Neutral mean, mean linear luminance MEAN_LUMINANCE, local hue/value variation kept. The source AO is
    multiplied in (the original textures carry baked joint/crevice darkening) and the luminance deviation from
    the mean is raised to `contrast` so joints read through the renderer's blurred-original tint."""
    rgb = srgb_to_linear(np.asarray(square(image.convert('RGB'), size)).astype(np.float64) / 255)
    if ao is not None:
        rgb = rgb * np.asarray(Image.fromarray(np.round(ao * 255).astype(np.uint8)).resize((size, size), Image.BICUBIC))[..., None] / 255
    if contrast != 1.0:
        lum = np.maximum(rgb @ LUMA, 1e-5)
        rgb = rgb * ((lum / lum.mean()) ** (contrast - 1))[..., None]
    for _ in range(3):  # clip-aware renormalisation to the neutral 0.18 mean
        rgb = np.clip(rgb / rgb.reshape(-1, 3).mean(0) * MEAN_LUMINANCE, 0, 1)
    return Image.fromarray(np.round(linear_to_srgb(rgb) * 255).astype(np.uint8))


def gray(image, size, default):
    if image is None:
        return np.full((size, size), default, np.float64)
    a = np.asarray(square(image, size)).astype(np.float64)
    if a.ndim == 3:
        a = a[..., 0]
    return a / (65535 if a.max() > 255 else 255)


def normal_map(image, size, strength):
    n = np.asarray(square(image.convert('RGB'), size)).astype(np.float64) / 255 * 2 - 1
    n[..., :2] *= strength
    n[..., 2] = np.maximum(n[..., 2], 1e-3)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return Image.fromarray(np.round((n * 0.5 + 0.5) * 255).astype(np.uint8))


def build_material(spec, params, zf, out):
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob('*.png'):
        stale.unlink()
    size, aux = spec.get('size', 1024), spec.get('aux_size', 512)
    repeat = spec.get('repeat', 1)  # n x n copies per tile: matches the original's feature density

    def load(suffix):
        image = member(zf, suffix)
        if image is None or repeat == 1:
            return image
        w = max(image.size)
        a = np.asarray(square(image, w).resize((w // repeat, w // repeat), Image.LANCZOS))
        return Image.fromarray(np.tile(a, (repeat, repeat) + (1,) * (a.ndim - 2)))

    ao = gray(load('AmbientOcclusion'), aux, 1.0)
    if not spec.get('micro'):
        detail_albedo(load('Color'), size, ao, spec.get('contrast', 1.0)).save(out / 'albedo.png', optimize=True)
    normal_map(load('NormalGL'), size, spec.get('normal_strength', 1.0)).save(out / 'normal.png', optimize=True)
    rough = np.clip(gray(load('Roughness'), aux, 0.65) + spec.get('roughness_bias', 0.0), 0.04, 1)
    metal = gray(load('Metalness'), aux, 0.0) * spec.get('metal_scale', 1.0)
    orm = np.stack([ao, rough, metal], -1)
    Image.fromarray(np.round(orm * 255).astype(np.uint8)).save(out / 'orm.png', optimize=True)
    if params.get('tessellate') or params.get('parallax'):
        h = gray(load('Displacement'), aux, 0.5)
        h = (h - h.min()) / max(h.max() - h.min(), 1e-6)
        Image.fromarray(np.round(h * 65535).astype(np.uint16)).save(out / 'height.png', optimize=True)


def cmd_build(args):
    pack, cache = pathlib.Path(args.pack), pathlib.Path(args.cache)
    sources = json.loads((pack / 'sources.json').read_text())
    manifest = json.loads((pack / 'manifest.json').read_text())
    only = set(args.only or [])
    for material_id, spec in sorted(sources['materials'].items()):
        if only and material_id not in only:
            continue
        if material_id not in manifest['materials']:
            raise SystemExit(f'{material_id}: in sources.json but not manifest.json')
        zf = fetch(spec['ambientcg'], cache)
        build_material(spec, manifest['materials'][material_id], zf, pack / material_id)
        print(f'{material_id}: {spec["ambientcg"]}')
    write_sources(pack, sources, manifest)
    cmd_check(args)


def write_sources(pack, sources, manifest):
    users = {}
    for key, material_id in manifest['textures'].items():
        users.setdefault(material_id, []).append(key)
    lines = ['# PBR pack sources (NONCANONICAL wave 29)', '',
             'Every image in this pack is converted by `tools/pbr_pack.py build` from the ambientCG set',
             'listed below (1K-PNG download). All ambientCG assets are released under',
             '[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/) (https://ambientcg.com/license).',
             'No original game content is included or derived; original textures only select the family.', '',
             '| material | source | license | maps | original keys |', '|---|---|---|---|---|']
    for material_id, spec in sorted(sources['materials'].items()):
        asset = spec['ambientcg']
        files = ', '.join(sorted(p.stem for p in (pack / material_id).glob('*.png')))
        lines.append(f'| {material_id} | [ambientCG {asset}]({ACG_PAGE.format(id=asset)}) | CC0 1.0 | {files} | '
                     f'{len(users.get(material_id, []))} |')
    (pack / 'SOURCES.md').write_text('\n'.join(lines) + '\n')


def cmd_check(args):
    pack = pathlib.Path(args.pack)
    manifest = json.loads((pack / 'manifest.json').read_text())
    if manifest.get('version') != 1:
        raise SystemExit('manifest version must be 1')
    problems, total = [], 0
    for material_id, params in manifest['materials'].items():
        folder = pack / material_id
        files = sorted(folder.glob('*.png'))
        if not files:
            problems.append(f'{material_id}: no images')
        for path in files:
            total += path.stat().st_size
            with Image.open(path) as image:
                if path.stem not in ('albedo', 'normal', 'orm', 'height') or max(image.size) > 1024:
                    problems.append(f'{path}: unexpected name or size {image.size}')
        if params.get('tessellate') and not (folder / 'height.png').exists():
            problems.append(f'{material_id}: tessellate without height.png')
    for key, material_id in manifest['textures'].items():
        if material_id not in manifest['materials']:
            problems.append(f'{key}: unknown material {material_id}')
        if key != key.lower() or '\\' in key or not key.startswith('textures/'):
            problems.append(f'{key}: key not normalised')
        if any(word in key for word in ('ocean', 'water', 'sky', 'golet')):
            problems.append(f'{key}: water/sky must never be mapped')
    digest = hashlib.sha256((pack / 'manifest.json').read_bytes()).hexdigest()[:12]
    print(f'pack {pack}: {len(manifest["materials"])} materials, {len(manifest["textures"])} keys, '
          f'{total / 1e6:.1f} MB, manifest sha256 {digest}')
    if problems:
        raise SystemExit('\n'.join(problems))


def cmd_preview(args):
    """Per material: original keys (top) vs the renderer's albedo formula on the pack (bottom), lit by the
    pack normal from the upper left. Approximates blur(orig) by the original's mean colour."""
    base, out, pack = pathlib.Path(args.base).resolve(), pathlib.Path(args.out).resolve(), pathlib.Path(args.pack)
    if out.is_relative_to(base):
        raise SystemExit('output must be outside the original installation')
    manifest = json.loads((pack / 'manifest.json').read_text())
    textures = texture_index(base)
    out.mkdir(parents=True, exist_ok=True)
    users = {}
    for key, material_id in manifest['textures'].items():
        users.setdefault(material_id, []).append(key)
    for material_id, keys in sorted(users.items()):
        folder, params = pack / material_id, manifest['materials'][material_id]
        keys = [k for k in keys if k in textures][:8]
        sheet = Image.new('RGB', (len(keys) * 196 + 4, 420), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        n = np.asarray(Image.open(folder / 'normal.png').convert('RGB').resize((192, 192))).astype(np.float64) / 127.5 - 1
        light = np.clip(n @ np.array([-0.5, 0.5, 0.7071]), 0, 1)[..., None] * 0.8 + 0.25
        detail = (srgb_to_linear(np.asarray(Image.open(folder / 'albedo.png').convert('RGB').resize((192, 192)))
                                 .astype(np.float64) / 255) if (folder / 'albedo.png').exists() else None)
        t = params['tint_keep_original']
        for j, key in enumerate(keys):
            orig = decode_original(textures[key]).convert('RGB').resize((192, 192))
            o = srgb_to_linear(np.asarray(orig).astype(np.float64) / 255)
            mixed = o if detail is None else detail * (o.reshape(-1, 3).mean(0) / MEAN_LUMINANCE) * (1 - t) + o * t
            lit = Image.fromarray(np.round(linear_to_srgb(mixed * light) * 255).astype(np.uint8))
            sheet.paste(orig, (4 + j * 196, 4))
            sheet.paste(lit, (4 + j * 196, 212))
            draw.text((6 + j * 196, 198), key[9:-4][:30], fill=(255, 255, 0))
        draw.text((6, 406), f'{material_id}: top original, bottom pack (t={t})', fill=(0, 255, 255))
        sheet.save(out / f'{material_id}.png')
    print(f'{out}: {len(users)} materials')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('inventory')
    p.add_argument('base')
    p.add_argument('out')
    p.set_defaults(run=cmd_inventory)
    p = sub.add_parser('sheets')
    p.add_argument('base')
    p.add_argument('out')
    p.add_argument('--manifest', default=str(PACK / 'manifest.json'))
    p.set_defaults(run=cmd_sheets)
    for name, run in (('build', cmd_build), ('check', cmd_check)):
        p = sub.add_parser(name)
        p.add_argument('--pack', default=str(PACK))
        p.add_argument('--cache', default=str(CACHE))
        if name == 'build':
            p.add_argument('--only', nargs='*')
        p.set_defaults(run=run)
    p = sub.add_parser('preview')
    p.add_argument('base')
    p.add_argument('out')
    p.add_argument('--pack', default=str(PACK))
    p.set_defaults(run=cmd_preview)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
