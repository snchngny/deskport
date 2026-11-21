#!/usr/bin/env python3
"""Check every Mach-O slice in a bundle without needing Apple's tools."""
import argparse
import json
from pathlib import Path
import struct

CPUS = {'x86_64': 0x01000007, 'arm64': 0x0100000c}


def version(value):
    return value >> 16, (value >> 8) & 255, value & 255


def slices(data):
    if data[:4] == b'\xca\xfe\xba\xbe':
        if len(data) < 8:
            raise ValueError('truncated universal header')
        count, = struct.unpack_from('>I', data, 4)
        if count > 64 or len(data) < 8 + count * 20:
            raise ValueError('invalid universal architecture table')
        for i in range(count):
            cpu, _, offset, size, _ = struct.unpack_from('>IIIII', data, 8 + i * 20)
            if offset < 8 + count * 20 or size < 32 or offset + size > len(data):
                raise ValueError('invalid universal slice bounds')
            yield cpu, data[offset:offset + size]
    elif data[:4] == b'\xcf\xfa\xed\xfe':
        if len(data) < 32:
            raise ValueError('truncated Mach-O header')
        yield struct.unpack_from('<I', data, 4)[0], data
    elif data[:4] in (b'\xfe\xed\xfa\xcf', b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbf'):
        raise ValueError('unsupported Mach-O format')


def deployment(data, cpu):
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe':
        raise ValueError('expected little-endian 64-bit Mach-O')
    actual, _, _, count, size = struct.unpack_from('<IIIII', data, 4)
    if actual != cpu or 32 + size > len(data) or count > size // 8:
        raise ValueError('invalid Mach-O load commands')
    offset, found = 32, []
    for _ in range(count):
        if offset + 8 > 32 + size:
            raise ValueError('truncated load command')
        command, length = struct.unpack_from('<II', data, offset)
        if length < 8 or offset + length > 32 + size:
            raise ValueError('invalid load command size')
        if command == 0x24:
            if length < 16:
                raise ValueError('truncated deployment command')
            found.append(version(struct.unpack_from('<I', data, offset + 8)[0]))
        elif command == 0x32:
            if length < 24 or struct.unpack_from('<I', data, offset + 8)[0] != 1:
                raise ValueError('invalid macOS build-version command')
            found.append(version(struct.unpack_from('<I', data, offset + 12)[0]))
        offset += length
    if not found:
        raise ValueError('missing minimum macOS version')
    return max(found)


def check(path, arch, maximum):
    entries = list(slices(path.read_bytes()))
    if not entries:
        return None
    matches = [data for cpu, data in entries if cpu == CPUS[arch]]
    if len(matches) != 1:
        raise ValueError('missing or duplicate ' + arch + ' slice')
    if matches[0].startswith(b'!<arch>\n'):
        # Static libraries are linked into a checked executable, not loaded by dyld.
        return None
    minimum = deployment(matches[0], CPUS[arch])
    if minimum > maximum:
        raise ValueError('requires macOS ' + '.'.join(map(str, minimum)))
    return '.'.join(map(str, minimum))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--arch', choices=CPUS, default='x86_64')
    parser.add_argument('--max-version', default='11.7.11')
    args = parser.parse_args()
    maximum = tuple(map(int, args.max_version.split('.')))
    maximum = (maximum + (0, 0, 0))[:3]
    errors, checked = [], []
    files = [args.root] if args.root.is_file() else sorted(args.root.rglob('*'))
    for path in files:
        if path.is_file() and not path.is_symlink():
            try:
                minimum = check(path, args.arch, maximum)
                if minimum is not None:
                    checked.append({'file': str(path), 'minimum': minimum})
            except (ValueError, struct.error) as error:
                errors.append({'file': str(path), 'error': str(error)})
    if not checked:
        errors.append({'error': 'no compatible Mach-O files found'})
    print(json.dumps({'checked': checked, 'errors': errors}, indent=2))
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
