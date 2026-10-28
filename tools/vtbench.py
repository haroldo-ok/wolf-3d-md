#!/usr/bin/env python3
"""vtbench.py - deterministic benchmark and frame-by-frame comparison (see README, "Performance").

  tools/vtbench.py tour ROM ELF                compare-free benchmark: ROM built with -DVT_TOUR
  tools/vtbench.py cmp ROM_A ELF_A ROM_B ELF_B  two ROMs built with -DVT_SEQ: compare the VRAM
                                               checksum of every one of the 240 sequence frames

ELF is out/release/rom.out of the same build.  Runs the ROM headless with tools/harness and the
Genesis Plus GX libretro core ($GPGX_CORE, default /root/work/gpgx/genesis_plus_gx_libretro.so).
"""
import os, struct, subprocess, sys, tempfile

CORE = os.environ.get('GPGX_CORE', '/root/work/gpgx/genesis_plus_gx_libretro.so')
HARNESS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'harness')
INTRO = '1000-1003:start,1300-1303:c,1700-1703:c'
N = 240                     # VT_FRAMES in src/render.c
VT_SUM_OFS = 27136          # vt_sum lives at screenbuf + 27136 (src/render.c)


def run(rom, elf, end):
    """run to vblank END, return (symbol table, RAM reader, view-area screenshot bytes)"""
    d = tempfile.mkdtemp(prefix='vtbench')
    subprocess.run([HARNESS, CORE, rom, str(end + 2), '%s,shot@%d' % (INTRO, end), d + '/f'],
                   capture_output=True)
    nm = subprocess.run(['m68k-elf-nm', elf], capture_output=True, text=True).stdout
    sy = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines()) if len(p) == 3}
    ram = open('%s/f%d.ram' % (d, end), 'rb').read()

    def rd(a):              # the dump holds byte-swapped words
        a &= 0xFFFF
        b = ram[a:a + 4]
        return struct.unpack('>I', bytes([b[1], b[0], b[3], b[2]]))[0]
    ppm = open('%s/f%d.ppm' % (d, end), 'rb').read()
    return sy, rd, ppm[-320 * 224 * 3:][:320 * 184 * 3]      # status bar (animated face) left out


def tour(rom, elf):
    sy, rd, _ = run(rom, elf, 4800)
    t8, t80, t240 = rd(sy['vt_t8']), rd(sy['vt_t80']), rd(sy['vt_t240'])
    if not (t8 < t80 < t240):
        sys.exit('sequence did not complete (was the ROM built with -DVT_TOUR?)')
    print('room (frames 8..80):   %.2f fps' % (72 * 60.0 / (t80 - t8)))
    print('tour (frames 80..240): %.2f fps' % (160 * 60.0 / (t240 - t80)))


def cmp(rom_a, elf_a, rom_b, elf_b):
    res = []
    for rom, elf in ((rom_a, elf_a), (rom_b, elf_b)):
        sy, rd, view = run(rom, elf, 5600)
        sums = [rd(sy['screenbuf'] + VT_SUM_OFS + 4 * i) for i in range(N)]
        res.append((sums, rd(sy['dbg_frames']), view))
    (sa, fa, va), (sb, fb, vb) = res
    n = min(fa, fb, N)
    bad = [i for i in range(n) if sa[i] != sb[i]]
    print('frames rendered: %d / %d, compared %d (%d distinct views)' % (fa, fb, n, len(set(sa[:n]))))
    print('mismatching frames: %s' % (bad[:20] or 'none'))
    print('final view:', 'identical' if va == vb else 'DIFFERENT')
    sys.exit(1 if bad or n < N or va != vb else 0)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'tour':
        tour(*sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == 'cmp':
        cmp(*sys.argv[2:])
    else:
        sys.exit(__doc__)
