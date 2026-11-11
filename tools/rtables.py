#!/usr/bin/env python3
"""Lookup tables of the renderer that depend only on the view geometry (no game data needed).

usage: rtables.py OUTDIR      (also run by convert.py)   -> OUTDIR/src/gen/rtables.c

spr_rowtab[H][y] = ceil(y * H / 64): the first screen row (relative to the top of a sprite
scaled to height H) of texel row y, for y = 0..64.  The sprite column code looks the two
ends of every post up here instead of multiplying.

rc_heighttab[d] = proj_heightnumerator / d: projected wall height (4 * pixels) for the
perpendicular distance d (8.8 tiles, at least MINDIST >> 8), replacing a 140 cycle divu."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cgen import c_words, write
import config as C

MAXH = 512

def gen_rtables(outdir):
    rows = []
    for H in range(MAXH + 1):
        rows += [(y * H + 63) >> 6 for y in range(65)]
    t = '#include "rtables.h"\n'
    t += c_words("spr_rowtab", rows, "u16", per_line=13, align=2)
    # the projection of convert.py: gen_tables
    scale = int((C.VIEWW // 2) * (C.FOCALLENGTH + C.MINDIST) / (C.VIEWGLOBAL / 2))
    heightnumerator = (65536 * scale) >> 6
    mind = C.MINDIST >> 8
    hts = [heightnumerator // max(d, mind) for d in range(32768)]
    assert max(hts) < 65536
    t += c_words("rc_heighttab", hts, "u16", per_line=16, align=2)
    t += "const s32 rt_heightnumerator = %d;\n" % heightnumerator
    write(os.path.join(outdir, "src/gen/rtables.c"), t)
    write(os.path.join(outdir, "src/gen/rtables.h"),
          "#ifndef RTABLES_H\n#define RTABLES_H\n#include \"wl_types.h\"\n"
          "extern const u16 spr_rowtab[%d];     /* [H][65], H = 0..%d */\n"
          "extern const u16 rc_heighttab[32768];\nextern const s32 rt_heightnumerator;\n#endif\n" % (len(rows), MAXH))
    return len(rows) * 2 + len(hts) * 2

if __name__ == "__main__":
    print("renderer tables bytes:", gen_rtables(sys.argv[1]))
