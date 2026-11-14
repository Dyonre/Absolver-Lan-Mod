"""pakls.py <pak> [substring] - lists the files in a UE4.18 .pak (unencrypted legacy index), optionally filtered. Read-only."""
import struct, sys


def entries(path):
    f = open(path, "rb")
    f.seek(-45, 2)
    t = f.read(45)
    off, sz = struct.unpack_from("<QQ", t, 9)
    f.seek(off)
    d = f.read(sz)
    o = 0
    n, = struct.unpack_from("<I", d, o); o += 4
    mount = d[o:o + n - 1].decode(); o += n
    c, = struct.unpack_from("<I", d, o); o += 4
    out = []
    for _ in range(c):
        n, = struct.unpack_from("<I", d, o); o += 4
        name = d[o:o + n - 1].decode(); o += n
        o += 24
        comp, = struct.unpack_from("<I", d, o); o += 4
        o += 20
        if comp:
            blocks, = struct.unpack_from("<I", d, o); o += 4 + blocks * 16
        o += 5
        out.append(mount + name)
    return out


if __name__ == "__main__":
    for e in entries(sys.argv[1]):
        if len(sys.argv) < 3 or sys.argv[2].lower() in e.lower():
            print(e)
