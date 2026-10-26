#!/usr/bin/env python3
"""Compare the original T3DNativeLightData SPIR-V member offsets with vec4 columns.

Usage: python verify_native_light_spirv_abi.py baseline_color.spv patched_color.spv
Both files should be glslang-generated native-lighting fragment variants.
"""

import argparse
import struct
from pathlib import Path


def spirv_text(words):
    return struct.pack('<' + 'I' * len(words), *words).split(b'\x00', 1)[0].decode('utf-8', 'replace')


def native_light_layout(path):
    blob = Path(path).read_bytes()
    if len(blob) < 20 or len(blob) % 4 != 0:
        raise ValueError(f'malformed SPIR-V: {path}')
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    if words[0] != 0x07230203:
        raise ValueError(f'wrong SPIR-V magic: {path}')
    names, fields, offsets, structures = {}, {}, {}, set()
    i = 5
    while i < len(words):
        size = words[i] >> 16
        opcode = words[i] & 65535
        if size == 0 or i + size > len(words):
            raise ValueError('invalid SPIR-V instruction')
        args = words[i + 1:i + size]
        if opcode == 5:
            names[args[0]] = spirv_text(args[1:])
        elif opcode == 6:
            fields[(args[0], args[1])] = spirv_text(args[2:])
        elif opcode == 72 and len(args) > 3 and args[2] == 35:
            offsets[(args[0], args[1])] = args[3]
        elif opcode == 30:
            structures.add(args[0])
        i += size
    results = []
    for identifier, name in names.items():
        if identifier not in structures or 'T3DNativeLightData' not in name:
            continue
        result = {field: offset for (ident, index), field in fields.items() if ident == identifier
                  for offset in [offsets.get((ident, index))]}
        if result:
            results.append(result)
    return results


def verify(before, after):
    old_candidates = native_light_layout(before)
    new_candidates = native_light_layout(after)
    old = next((item for item in old_candidates if item.get('shadow_matrix') is not None), None)
    new = next((item for item in new_candidates if item.get('shadow_column0') is not None), None)
    if old is None or new is None:
        raise AssertionError('native-light struct with member offsets missing in SPIR-V')
    origin = old['shadow_matrix']
    for i in range(4):
        assert new[f'shadow_column{i}'] == origin + 16 * i
    for field, offset in old.items():
        if field != 'shadow_matrix':
            assert new.get(field) == offset, f'{field}: {offset} -> {new.get(field)}'
    extra = set(new) - set(old) - {f'shadow_column{i}' for i in range(4)}
    assert not extra, f'unexpected native light fields: {extra}'
    print(f'T3DNativeLightData SPIR-V ABI PASS: shadow columns {origin}..{origin+63}; '
          f'{len(old)-1} other field offsets unchanged')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline_spv', type=Path)
    parser.add_argument('patched_spv', type=Path)
    args = parser.parse_args()
    verify(args.baseline_spv, args.patched_spv)


if __name__ == '__main__':
    main()
