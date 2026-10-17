#!/usr/bin/env python3
"""Minimal Xbox 360 GDFX (XDVDFS) extractor: gdfx_extract.py <iso> <outdir> [top-level names...]"""
import os, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# plain XDVDFS, XGD3, XGD2, XGD1 game-partition offsets
PARTITIONS = [0, 0x2080000, 0xFD90000, 0x18300000]


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            root_sector, root_size = struct.unpack("<II", f.read(8))
            return base, root_sector, root_size
    sys.exit("No GDFX volume descriptor found")


def read_dir(f, base, sector, size):
    f.seek(base + sector * SECTOR)
    buf = f.read(size)
    entries, stack, seen = [], [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(buf):
            continue
        seen.add(off)
        left, right, sec, sz, attr, nlen = struct.unpack_from("<HHIIBB", buf, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = buf[off + 14: off + 14 + nlen].decode("latin-1")
        entries.append((name, sec, sz, bool(attr & 0x10)))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return entries


def extract(f, base, entries, out, wanted=None):
    for name, sec, sz, is_dir in entries:
        if wanted and name.lower() not in wanted:
            continue
        path = os.path.join(out, name)
        if is_dir:
            os.makedirs(path, exist_ok=True)
            if sz:
                extract(f, base, read_dir(f, base, sec, sz), path)
        else:
            f.seek(base + sec * SECTOR)
            with open(path, "wb") as o:
                left = sz
                while left:
                    chunk = f.read(min(left, 1 << 22))
                    o.write(chunk)
                    left -= len(chunk)
            print(path, sz)


if __name__ == "__main__":
    iso, out = sys.argv[1], sys.argv[2]
    wanted = {n.lower() for n in sys.argv[3:]} or None
    with open(iso, "rb") as f:
        base, rs, rz = find_partition(f)
        os.makedirs(out, exist_ok=True)
        extract(f, base, read_dir(f, base, rs, rz), out, wanted)
