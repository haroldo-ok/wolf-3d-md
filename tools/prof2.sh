#!/bin/sh
# profile 400 frames of walking gameplay; prints renders in window and busy cycles/render
SO=/root/work/gpgx_prof/genesis_plus_gx_libretro.so
RUN=$(nm $SO | awk '$3=="retro_run"{print $1}'); PROF=$(nm $SO | awk '$3=="wolf_prof"{print $1}')
WOLF_RUNSYM=$RUN WOLF_PROFSYM=$PROF WOLF_PROFSTART=3000 ./tools/harness $SO ${1:-out/rom.bin} 3400 "1000-1003:start,1300-1303:c,1700-1703:c,2500-3500:up,shot@3000,shot@3399" /tmp/shots/p >/dev/null 2>&1
python3 tools/ramsym.py /tmp/shots/p3000.ram dbg_frames:4; python3 tools/ramsym.py /tmp/shots/p3399.ram dbg_frames:4
python3 tools/profrep.py 10
