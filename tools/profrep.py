#!/usr/bin/env python3
"""profrep.py - summarise /tmp/prof.bin (cycles per word address) per function and (with -g) per source line"""
import struct, subprocess, sys, collections, bisect
h = struct.unpack('<%dI' % 0x200000, open('/tmp/prof.bin','rb').read())
nm = subprocess.run(['m68k-elf-nm','-n','out/release/rom.out'],capture_output=True,text=True).stdout
syms=[(int(l.split()[0],16),l.split()[2]) for l in nm.splitlines() if len(l.split())==3 and l.split()[1] in 'tTwW']
addrs=[a for a,_ in syms]
tot = sum(h)
byf = collections.Counter()
for i,c in enumerate(h):
    if c:
        a = i*2
        j = bisect.bisect_right(addrs, a)-1
        byf[syms[j][1] if j>=0 else '?'] += c
print('total cycles (x7):', tot)
for n,c in byf.most_common(int(sys.argv[1]) if len(sys.argv)>1 else 25):
    print('%6.2f%% %s' % (100.0*c/tot, n))
if '-l' in sys.argv:
    lines = collections.Counter()
    hot = sorted(((c,i*2) for i,c in enumerate(h) if c), reverse=True)[:4000]
    for c,a in hot:
        r = subprocess.run(['m68k-elf-addr2line','-e','out/release/rom.out','-f','-i','0x%x'%a],capture_output=True,text=True).stdout.split('\n')
        # innermost frame first
        loc = r[1] if len(r)>1 else '?'
        lines[loc] += c
    for l,c in lines.most_common(30):
        print('%6.2f%% %s' % (100.0*c/tot, l))
