#!/usr/bin/env python3
"""ramsym.py RAMDUMP SYMBOL[:size][ ...]  - peek 68k RAM dump (u16/u32 big endian, auto byte-swap detect)"""
import sys, subprocess, struct
dump = open(sys.argv[1], 'rb').read()
nm = subprocess.run(['m68k-elf-nm', '-n', 'out/release/rom.out'], capture_output=True, text=True).stdout
syms = {}
for l in nm.splitlines():
    p = l.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
def swap(b):
    out = bytearray(len(b))
    out[0::2] = b[1::2]; out[1::2] = b[0::2]
    return bytes(out)
if 'swap' in sys.argv: dump = swap(dump)
for a in sys.argv[2:]:
    if a == 'swap': continue
    n, _, sz = a.partition(':')
    off = syms[n] & 0xFFFF
    sz = int(sz) if sz else 4
    print(n, hex(syms[n]), dump[off:off+sz].hex(), struct.unpack('>I', dump[off:off+4])[0] if sz >= 4 else '')
