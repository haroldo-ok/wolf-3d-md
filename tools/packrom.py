#!/usr/bin/env python3
"""Append the sound data to the ROM: packrom.py ROM PCM_BIN OUT

ROM is the padded image the SGDK build produced (code and all other data), PCM_BIN the file written by
convert.py (see audio.py).  The sound data goes at WOLF_PCM_BASE (default 0x320000, the same value convert.py
laid the samples out for), so the image has to be no bigger than that.  The ROM is then padded to a multiple of
128 KB and the header's end address and checksum are updated for the new size."""
import os, struct, sys

MAXROM = 0x800000
BASE = int(os.environ.get("WOLF_PCM_BASE", "0x320000"), 0)


def checksum(rom):
    """SGDK's own format (sizebnd -checksum): xor of all 32 bit words of the file, the header field zeroed, folded to 16 bits"""
    rom[0x18E] = rom[0x18F] = 0
    c = 0
    for (w,) in struct.iter_unpack(">I", bytes(rom[:len(rom) & ~3])):
        c ^= w
    return (c ^ (c >> 16)) & 0xFFFF


def main():
    rom_path, pcm_path, out_path = sys.argv[1:4]
    rom = bytearray(open(rom_path, "rb").read())
    pcm = open(pcm_path, "rb").read()
    if len(rom) > BASE:
        raise SystemExit("code and data take %d bytes; the sound data starts at %d (WOLF_PCM_BASE, also for "
                         "'make data')" % (len(rom), BASE))
    rom.extend(bytes(BASE - len(rom)))
    rom.extend(pcm)
    rom.extend(bytes(-len(rom) % 0x20000))
    if len(rom) > MAXROM:
        raise SystemExit("ROM would be %d bytes, more than the 8 MB the SSF mapper can address" % len(rom))
    struct.pack_into(">I", rom, 0x1A4, len(rom) - 1)
    struct.pack_into(">H", rom, 0x18E, checksum(rom))
    open(out_path, "wb").write(rom)
    print("ROM %d bytes: code and data up to 0x%X, %d KB of sound behind them" % (len(rom), BASE, len(pcm) // 1024))


if __name__ == "__main__":
    main()
