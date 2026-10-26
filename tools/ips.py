#!/usr/bin/env python3
"""Apply IPS patches to a ROM: ips.py [--pad=MEGABYTES] in.gba out.gba [patch.ips ...]"""
import sys


def apply(data, patch):
    assert patch[:5] == b'PATCH', 'not an IPS file'
    p = 5
    while patch[p:p + 3] != b'EOF' or p + 3 < len(patch) - 3:
        off = int.from_bytes(patch[p:p + 3], 'big')
        size = int.from_bytes(patch[p + 3:p + 5], 'big')
        p += 5
        if size:
            chunk = patch[p:p + size]
            p += size
        else:   # run-length record
            size = int.from_bytes(patch[p:p + 2], 'big')
            chunk = patch[p + 2:p + 3] * size
            p += 3
        if off + size > len(data):
            data.extend(b'\0' * (off + size - len(data)))
        data[off:off + size] = chunk
    return data


def main():
    args = sys.argv[1:]
    pad = 0
    if args[0].startswith('--pad='):    # grow the ROM to this many megabytes (room for added data)
        pad = int(args.pop(0)[6:]) << 20
    sys.argv[1:] = args
    data = bytearray(open(sys.argv[1], 'rb').read())
    for path in sys.argv[3:]:
        apply(data, open(path, 'rb').read())
        print('ips: applied %s' % path)
    if len(data) < pad:
        data.extend(b'\xff' * (pad - len(data)))
    open(sys.argv[2], 'wb').write(data)


if __name__ == '__main__':
    main()
