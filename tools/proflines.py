#!/usr/bin/env python3
"""proflines.py FUNC [N]: per source line cycle share of everything inlined/called under FUNC (frame chain contains FUNC)"""
import struct,subprocess,collections,sys
h=struct.unpack('<%dI'%0x200000,open('/tmp/prof.bin','rb').read())
tot=sum(h); want=sys.argv[1]; N=int(sys.argv[2]) if len(sys.argv)>2 else 25
hot=sorted(((c,i*2) for i,c in enumerate(h) if c),reverse=True)[:8000]
agg=collections.Counter(); sub=0
for c,a in hot:
    r=subprocess.run(['m68k-elf-addr2line','-e','out/release/rom.out','-f','-i','0x%x'%a],capture_output=True,text=True).stdout.split('\n')
    pairs=[(r[i],r[i+1]) for i in range(0,len(r)-1,2)]
    for k,(f,l) in enumerate(pairs):
        if f==want:
            agg[l.split('/')[-1]]+=c; sub+=c; break
print('%s total %.2f%%'%(want,100*sub/tot))
for k,c in agg.most_common(N): print('%5.2f%% %s'%(100*c/tot,k))
