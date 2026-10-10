#!/usr/bin/env python3
"""Small synthetic check for local analysis readers; contains no original bytes."""
import struct

from command_catalog import extract
from pe_inventory import pe_metadata


def main():
    image = bytearray(1024)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 60, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', image, 0x84, 0x14c, 1, 0, 0, 0, 0xe0, 0x103)
    opt = 0x98
    struct.pack_into('<H', image, opt, 0x10b)
    struct.pack_into('<I', image, opt + 16, 0x1100)
    struct.pack_into('<I', image, opt + 28, 0x400000)
    struct.pack_into('<I', image, opt + 60, 0x200)
    struct.pack_into('<H', image, opt + 68, 2)
    struct.pack_into('<II', image, opt + 104, 0x1000, 40)
    section = opt + 0xe0
    image[section:section + 8] = b'.rdata\0\0'
    struct.pack_into('<IIII', image, section + 8, 512, 0x1000, 512, 0x200)
    struct.pack_into('<IIIII', image, 0x200, 0x1040, 0, 0, 0x1080, 0x1050)
    struct.pack_into('<III', image, 0x240, 0x1090, 0x80000013, 0)
    image[0x280:0x28b] = b'WS2_32.dll\0'
    image[0x290:0x297] = b'\0\0send\0'
    metadata = pe_metadata(image)
    assert metadata['architecture'] == 'x86'
    assert metadata['entry_va'] == '0x401100'
    assert [(i['library'], i['name'], i['iat_va']) for i in metadata['imports']] == [
        ('WS2_32.dll', 'send', 0x401050), ('WS2_32.dll', 'ordinal:19', 0x401054)]
    try:
        pe_metadata(b'not an image')
    except ValueError:
        pass
    else:
        raise AssertionError('invalid PE accepted')
    commands = extract('if (local_b8 == 0x70766564) { FUN_00400000(); }\n      else { }')
    assert len(commands) == 1
    assert commands[0]['wire_hex'] == '64 65 76 70'
    assert commands[0]['call_targets'] == ['00400000']
    print('analysis reader synthetic checks passed')


if __name__ == '__main__':
    main()
