#!/bin/sh
# usage: prof.sh FRAMES START SCRIPT  -> /tmp/prof.bin cycle histogram + report
SO=/root/work/gpgx_prof/genesis_plus_gx_libretro.so
RUN=$(nm $SO | awk '$3=="retro_run"{print $1}')
PROF=$(nm $SO | awk '$3=="wolf_prof"{print $1}')
WOLF_RUNSYM=$RUN WOLF_PROFSYM=$PROF WOLF_PROFSTART=$2 ./tools/harness $SO ${4:-out/rom.bin} $1 "$3" /tmp/shots/p 2>&1 | grep -i "error\|fail"
python3 tools/profrep.py
