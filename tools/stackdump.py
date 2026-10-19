#!/usr/bin/env python3
"""stackdump.py RAMDUMP : find plausible return addresses in the (byte-swapped) 68k RAM dump top area"""
import sys, struct, subprocess, bisect
d = bytearray(open(sys.argv[1],'rb').read())
d[0::2], d[1::2] = d[1::2], d[0::2]
nm = subprocess.run(['m68k-elf-nm','-n','out/release/rom.out'],capture_output=True,text=True).stdout
syms=[(int(l.split()[0],16),l.split()[2]) for l in nm.splitlines() if len(l.split())==3 and l.split()[1] in 'tTwW']
addrs=[a for a,_ in syms]
lo = int(sys.argv[2],16) if len(sys.argv)>2 else 0xF000
for off in range(lo, 0x10000-3, 2):
    v = struct.unpack('>I', d[off:off+4])[0]
    if 0x200 < v < 0x100000:
        i = bisect.bisect_right(addrs, v)-1
        print(hex(off), hex(v), syms[i][1], '+', hex(v-syms[i][0]))
