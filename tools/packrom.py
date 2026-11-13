#!/usr/bin/env python3
"""Append the sound pages to the ROM: packrom.py ROM PCM_BIN OUT

ROM is the padded image the SGDK build produced (code and all other data), PCM_BIN the file written by
convert.py (a multiple of 512 KB, see audio.py).  The image has to stay below 0x380000 because the sound
pages are swapped into the mapper window at 0x380000; the pages start at 0x400000.  The ROM header's end
address and checksum are updated for the new size."""
import struct, sys

PAGE = 0x80000
LIMIT = 0x380000
MAXROM = 0x800000


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
    assert len(pcm) % PAGE == 0, "pcm.bin must be whole 512 KB pages"
    if len(rom) > LIMIT:
        raise SystemExit("code and data take %d bytes; at most %d fit below the sound window" % (len(rom), LIMIT))
    rom.extend(bytes(0x400000 - len(rom)))
    rom.extend(pcm)
    if len(rom) > MAXROM:
        raise SystemExit("ROM would be %d bytes, more than the 8 MB the SSF mapper can address" % len(rom))
    struct.pack_into(">I", rom, 0x1A4, len(rom) - 1)
    struct.pack_into(">H", rom, 0x18E, checksum(rom))
    open(out_path, "wb").write(rom)
    print("ROM %d bytes: code and data in the first 4 MB, %d KB of sound in %d pages" % (len(rom), len(pcm) // 1024, len(pcm) // PAGE))


if __name__ == "__main__":
    main()
