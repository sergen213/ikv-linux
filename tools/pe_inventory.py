#!/usr/bin/env python3
"""Read-only PE metadata/import inventory; no execution, probing, or dependencies."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import struct


def pe_metadata(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('not a DOS/PE image')
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('invalid PE signature')
    machine, count, timestamp, _, _, opt_size, flags = struct.unpack_from('<HHIIIHH', data, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    if magic not in (0x10b, 0x20b):
        raise ValueError('unsupported optional header')
    wide = magic == 0x20b
    image_base = struct.unpack_from('<Q' if wide else '<I', data, opt + (24 if wide else 28))[0]
    entry = struct.unpack_from('<I', data, opt + 16)[0]
    subsystem = struct.unpack_from('<H', data, opt + 68)[0]
    section_table = opt + opt_size
    sections = []
    for i in range(count):
        start = section_table + i * 40
        name = data[start:start + 8].rstrip(b'\0').decode('ascii', 'replace')
        virtual_size, rva, size, offset = struct.unpack_from('<IIII', data, start + 8)
        sections.append(dict(name=name, rva=rva, virtual_size=virtual_size, raw_size=size,
                             raw_offset=offset, flags=struct.unpack_from('<I', data, start + 36)[0]))

    def file_offset(rva):
        for section in sections:
            delta = rva - section['rva']
            if 0 <= delta < section['raw_size']:
                return section['raw_offset'] + delta
        if 0 <= rva < struct.unpack_from('<I', data, opt + 60)[0]:
            return rva
        raise ValueError(f'unmapped RVA {rva:x}')

    def string(rva):
        offset = file_offset(rva)
        end = data.index(b'\0', offset)
        return data[offset:end].decode('ascii', 'replace')

    imports = []
    directories = opt + (112 if wide else 96)
    import_rva, _ = struct.unpack_from('<II', data, directories + 8)
    if import_rva:
        descriptor = file_offset(import_rva)
        while True:
            original, _, _, name, first = struct.unpack_from('<IIIII', data, descriptor)
            if not any((original, name, first)):
                break
            library = string(name)
            thunk = file_offset(original or first)
            width = 8 if wide else 4
            index = 0
            while True:
                value = struct.unpack_from('<Q' if wide else '<I', data, thunk + index * width)[0]
                if value == 0:
                    break
                ordinal_mask = 1 << (width * 8 - 1)
                symbol = f'ordinal:{value & 0xffff}' if value & ordinal_mask else string(value + 2)
                imports.append(dict(library=library, name=symbol, iat_rva=first + index * width,
                                    iat_va=image_base + first + index * width))
                index += 1
            descriptor += 20
    return dict(machine=hex(machine), architecture='x86-64' if wide else 'x86',
                linker_version=f'{data[opt + 2]}.{data[opt + 3]}',
                timestamp_utc=datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).isoformat(),
                image_base=hex(image_base), entry_rva=hex(entry), entry_va=hex(image_base + entry),
                subsystem=subsystem, characteristics=hex(flags), sections=sections, imports=imports)


def inventory(root):
    result = []
    endpoint = re.compile(rb'(?:https?://|ftp://)[!-~]+|(?:[a-zA-Z0-9-]+\.)+(?:com|net|org|com\.tr)(?:/[!-~]*)?')
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.suffix.lower() not in ('.exe', '.dll'):
            continue
        data = path.read_bytes()
        item = dict(path=str(path), size=len(data), sha256=hashlib.sha256(data).hexdigest())
        try:
            item.update(pe_metadata(data))
            item['endpoint_candidates'] = [dict(file_offset=hex(m.start()), value=m.group().decode('ascii'))
                                            for m in endpoint.finditer(data)]
        except (ValueError, struct.error, IndexError) as error:
            item['error'] = str(error)
        result.append(item)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(inventory(args.root), indent=2) + '\n')
