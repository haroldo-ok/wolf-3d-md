#!/bin/sh
# usage: fps.sh [ROM] -> renders in 1200 vblanks standing still in the start view (and turning slowly)
ROM=${1:-out/rom.bin}
rm -f /tmp/shots/*
./tools/harness /root/work/gpgx/genesis_plus_gx_libretro.so $ROM 3700 "1000-1003:start,1300-1303:c,1700-1703:c,2400-2500:left,shot@2500,shot@3690" /tmp/shots/f >/dev/null 2>&1
python3 - <<'P'
import subprocess
def v(f):
    o=subprocess.run(['python3','tools/ramsym.py',f,'dbg_frames:4'],capture_output=True,text=True).stdout.split()
    x=int(o[2],16); return ((x&0xff00)>>8)|(((x>>24)&0xff)<<8)
a=v('/tmp/shots/f2500.ram'); b=v('/tmp/shots/f3690.ram')
print("renders in 1200 vblanks:", (b-a)&0xffff, " => %.2f fps"%(((b-a)&0xffff)/19.8))
P
