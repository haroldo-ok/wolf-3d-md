#!/bin/sh
# usage: run.sh FRAMES SCRIPT [ROM]   -> /tmp/shots/f<N>.png
ROM=${3:-out/rom.bin}
rm -f /tmp/shots/*
timeout 600 ./tools/harness /root/work/gpgx/genesis_plus_gx_libretro.so $ROM $1 "$2" /tmp/shots/f 2>&1 | tail -5
for f in /tmp/shots/f*.ppm; do python3 -c "
import sys
from PIL import Image
im=Image.open('$f'); im.save('${f%.ppm}.png')" ; done
ls /tmp/shots/*.png
