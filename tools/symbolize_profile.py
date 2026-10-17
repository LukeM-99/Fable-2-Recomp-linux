#!/usr/bin/env python3
"""Symbolize profile.txt written by FABLE2_PROFILE=1 (src/diagnostics/linux_sampler.h).

usage: symbolize_profile.py <profile.txt> <dir with fable_2 + .so files> [top N]

Prints, per thread, the hottest functions (self samples) with percentages.
Module offsets are resolved with `nm -n --defined-only -C` on the binaries.
"""
import bisect
import collections
import os
import subprocess
import sys


def load_symbols(path):
    out = subprocess.run(["nm", "-n", "--defined-only", "-C", path],
                         capture_output=True, text=True).stdout
    if not out.strip():  # stripped library: fall back to the dynamic symbols
        out = subprocess.run(["nm", "-n", "-D", "--defined-only", "-C", path],
                             capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3 or parts[1] not in "tTwWiI":
            continue
        try:
            addrs.append(int(parts[0], 16))
        except ValueError:
            continue
        names.append(parts[2])
    return addrs, names


def main():
    prof, bindir = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    tables = {}
    per_thread = collections.defaultdict(collections.Counter)
    for line in open(prof):
        if line.startswith("#"):
            continue
        n, thread, module, off = line.split()
        n, off = int(n), int(off, 16)
        if module not in tables:
            p = os.path.join(bindir, module)
            if not os.path.exists(p):  # system library
                for d in ("/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"):
                    if os.path.exists(os.path.join(d, module)):
                        p = os.path.join(d, module)
                        break
            tables[module] = load_symbols(p) if os.path.exists(p) else ([], [])
        addrs, names = tables[module]
        i = bisect.bisect_right(addrs, off) - 1
        name = names[i] if i >= 0 else f"{module}+0x{off:x}"
        if len(name) > 110:
            name = name[:107] + "..."
        per_thread[thread][name if addrs else f"[{module}]"] += n
    totals = {t: sum(c.values()) for t, c in per_thread.items()}
    for thread, _ in sorted(totals.items(), key=lambda kv: -kv[1])[:8]:
        c = per_thread[thread]
        print(f"== {thread}: {totals[thread]} samples")
        for name, n in c.most_common(top):
            print(f"  {100.0 * n / totals[thread]:5.1f}%  {name}")


if __name__ == "__main__":
    main()
