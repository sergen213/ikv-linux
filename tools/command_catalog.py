#!/usr/bin/env python3
"""Export address-free command facts from a local Ghidra dispatcher decompilation.

The source remains ignored; output includes constants, line anchors and call targets,
never handler semantics or proprietary source. Decompiler text changes may require
updating this intentionally narrow extractor.
"""
import argparse
import json
from pathlib import Path
import re


def extract(text):
    matches = list(re.finditer(r'local_b8 == (0x[0-9a-f]+)', text))
    result = []
    for index, match in enumerate(matches):
        value = int(match[1], 16)
        end = matches[index + 1].start() if index + 1 < len(matches) else text.find('\n      else {', match.end())
        block = text[match.end():end if end >= 0 else len(text)]
        wire = value.to_bytes(4, 'little')
        result.append(dict(id=f'0x{value:08x}', wire_hex=wire.hex(' '), wire_ascii=wire.decode('ascii', 'replace'),
                           dispatcher_va='0x0045b4b0', decompiler_line=text[:match.start()].count('\n') + 1,
                           call_targets=sorted(set(re.findall(r'(?:FUN_|LAB_)([0-9a-f]{8})', block))),
                           confidence='confirmed-static ID; argument schema/semantics not established'))
    if not result:
        raise ValueError('No expected dispatcher comparisons; not a compatible evidence file')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('decompiler_file', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(extract(args.decompiler_file.read_text()), indent=2) + '\n')
