"""Compare two builds of a DLL, ignoring only the build timestamp.

Usage: python compare_dll.py <dll A> <dll B>
MSVC stores the link timestamp in the PE header and again in the debug directory. This script blanks every copy of each
file's own timestamp, then compares what is left byte for byte.
"""
import struct
import sys


def load(path):
    data = bytearray(open(path, 'rb').read())
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    stamp = bytes(data[pe + 8:pe + 12])
    copies = 0
    at = data.find(stamp)
    while at >= 0:
        data[at:at + 4] = b'\0\0\0\0'
        copies += 1
        at = data.find(stamp, at + 4)
    return bytes(data), stamp[::-1].hex().upper(), copies


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, stamp_a, copies_a = load(sys.argv[1])
    b, stamp_b, copies_b = load(sys.argv[2])
    print('A: timestamp %s, %d copies' % (stamp_a, copies_a))
    print('B: timestamp %s, %d copies' % (stamp_b, copies_b))
    if a == b:
        print('MATCH: the files are identical apart from the build timestamp.')
        return
    if len(a) != len(b):
        print('DIFFERENT: sizes are %d and %d bytes.' % (len(a), len(b)))
    else:
        print('DIFFERENT: %d bytes differ.' % sum(x != y for x, y in zip(a, b)))
    sys.exit(1)


if __name__ == '__main__':
    main()
