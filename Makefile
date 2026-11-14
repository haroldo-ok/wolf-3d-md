# Wolfenstein 3D for the Sega Genesis / Mega Drive - SGDK project
#
#   make data   convert the original shareware files (tools/convert.py)
#   make        build out/rom.bin
#
GDK ?= /root/work/sgdk
WOLF_DATA ?= /root/work/data
WOLF_SRC ?= /root/work/wolf3d
export WOLF_PCM_BASE ?= 0x320000
EXTRA_FLAGS += -Isrc

include $(GDK)/makefile.gen

# 68000-safe libgcc from SGDK, and no build-id note (it collides with the
# vector table when linking with a Linux-target binutils)
LIBGCC := $(GDK)/lib/libgcc.a -Wl,--build-id=none

.PHONY: data
data:
	python3 -I tools/convert.py $(WOLF_DATA) $(WOLF_SRC) .

# The full game's sound data is kept out of the linked image: convert.py then writes src/gen/pcm.bin, which is
# appended to the ROM at WOLF_PCM_BASE (the code reaches it through the SSF mapper window at 0x380000 or linearly).
.PHONY: packROM
release: packROM
packROM: padROM
	@if [ -f src/gen/pcm.bin ]; then python3 -I tools/packrom.py out/release/rom.bin src/gen/pcm.bin out/rom.bin; fi
