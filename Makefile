# Wolfenstein 3D for the Sega Genesis / Mega Drive - SGDK project
#
#   make data   convert the original shareware files (tools/convert.py)
#   make        build out/rom.bin
#
GDK ?= /root/work/sgdk
WOLF_DATA ?= /root/work/data
WOLF_SRC ?= /root/work/wolf3d
EXTRA_FLAGS += -Isrc

include $(GDK)/makefile.gen

# 68000-safe libgcc from SGDK, and no build-id note (it collides with the
# vector table when linking with a Linux-target binutils)
LIBGCC := $(GDK)/lib/libgcc.a -Wl,--build-id=none

.PHONY: data
data:
	python3 -I tools/convert.py $(WOLF_DATA) $(WOLF_SRC) .
