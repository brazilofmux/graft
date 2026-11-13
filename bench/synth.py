#!/usr/bin/env python3
"""Generate synthetic old/new pairs for the benchmark harness.

    python3 -I bench/synth.py OUTDIR [--seed N] [--size BYTES] [--exe PATH]

Each pair is OUTDIR/<name>/{old,new}. The pairs isolate one behavior each:

  edits     random data with scattered small in-place edits
  insert    a block inserted near the start (everything after it shifts)
  shuffle   blocks of the file reordered (moved code or data)
  text      line-oriented text with lines inserted, deleted and changed
  reloc     a real executable with every aligned 32-bit word in a range
            nudged by a constant (crude model of code moving: the case
            bsdiff's approximate matching and our exe filters target)
  identical old == new (overhead floor)

These are no substitute for real version pairs. Put those under
bench/corpus/private/<name>/{old,new} (gitignored) and pass that directory.
"""
import argparse
import os
import random
import struct
import sys


def write_pair(root, name, old, new):
    d = os.path.join(root, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "old"), "wb") as f:
        f.write(old)
    with open(os.path.join(d, "new"), "wb") as f:
        f.write(new)
    print(f"{name:10s} old={len(old):>10,d}  new={len(new):>10,d}")


def edits(rng, size):
    old = rng.randbytes(size)
    new = bytearray(old)
    for _ in range(max(1, size // 4096)):
        pos = rng.randrange(size - 16)
        new[pos:pos + rng.randrange(1, 16)] = rng.randbytes(rng.randrange(1, 16))
    return old, bytes(new)


def insert(rng, size):
    old = rng.randbytes(size)
    pos = size // 16
    return old, old[:pos] + rng.randbytes(4096) + old[pos:]


def shuffle(rng, size):
    old = rng.randbytes(size)
    block = 16384
    blocks = [old[i:i + block] for i in range(0, size, block)]
    rng.shuffle(blocks)
    return old, b"".join(blocks)


def text(rng, size):
    words = ["patch", "delta", "graft", "source", "target", "window", "match",
             "copy", "add", "run", "hash", "block", "version", "archive", "mask"]
    lines, total = [], 0
    while total < size:
        line = " ".join(rng.choice(words) for _ in range(rng.randrange(3, 12))) + "\n"
        lines.append(line)
        total += len(line)
    new = list(lines)
    for _ in range(len(lines) // 50):
        i = rng.randrange(len(new))
        op = rng.randrange(3)
        if op == 0:
            new.insert(i, "inserted " + rng.choice(words) + "\n")
        elif op == 1 and len(new) > 1:
            del new[i]
        else:
            new[i] = new[i].upper()
    return "".join(lines).encode(), "".join(new).encode()


def reloc(rng, exe_path):
    with open(exe_path, "rb") as f:
        old = f.read()
    new = bytearray(old)
    lo, hi = len(old) // 8, len(old) // 2  # skip headers; touch a middle span
    delta = 0x40
    for off in range(lo & ~3, hi - 4, 4):
        (v,) = struct.unpack_from("<I", new, off)
        # Only words that look like nearby offsets: small magnitude, so
        # mostly displacement- and pointer-like values, not opcodes.
        if 0 < v < len(old):
            struct.pack_into("<I", new, off, (v + delta) & 0xFFFFFFFF)
    # And a real insertion, so the code also moves.
    ins = len(old) // 4
    new[ins:ins] = rng.randbytes(256)
    return old, bytes(new)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--size", type=int, default=4 << 20)
    ap.add_argument("--exe", default=sys.executable, help="executable to base the 'reloc' pair on")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    write_pair(a.outdir, "edits", *edits(rng, a.size))
    write_pair(a.outdir, "insert", *insert(rng, a.size))
    write_pair(a.outdir, "shuffle", *shuffle(rng, a.size))
    write_pair(a.outdir, "text", *text(rng, a.size))
    exe = os.path.realpath(a.exe)
    write_pair(a.outdir, "reloc", *reloc(rng, exe))
    same = rng.randbytes(a.size // 4)
    write_pair(a.outdir, "identical", same, same)


if __name__ == "__main__":
    main()
