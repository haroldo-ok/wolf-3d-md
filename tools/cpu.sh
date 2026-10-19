#!/bin/sh
# usage: cpu.sh FRAMES SCRIPT  -> prints PC/SP at every shot
RUN=$(nm /root/work/gpgx/genesis_plus_gx_libretro.so | awk '$3=="retro_run"{print $1}')
REG=$(nm /root/work/gpgx/genesis_plus_gx_libretro.so | awk '$3=="m68k_get_reg"{print $1}')
WOLF_CPU=1 WOLF_RUNSYM=$RUN WOLF_REGSYM=$REG ./tools/harness /root/work/gpgx/genesis_plus_gx_libretro.so ${3:-out/rom.bin} $1 "$2" /tmp/shots/c 2>&1 | grep -E "frame [0-9]+:"
