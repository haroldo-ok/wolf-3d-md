#!/usr/bin/env python3
"""Wall textures with every texel stored twice, for the low detail mode.

usage: walls2.py OUTDIR      (also run by convert.py, after the walls)  -> OUTDIR/src/gen/walls2.c

In low detail a ray is 4 pixels = 2 bytes wide, and the wall scalers (tools/scalers.py, the
word set) move a whole word per screen row.  That only costs the same as a byte if the word
can be loaded as it is, hence this copy of wall_data in which texel i of a column is the
word at offset 2i.  Reads the converted walls back from OUTDIR/src/gen/walls.c."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cgen import c_words, write


def gen_walls2(outdir):
    s = open(os.path.join(outdir, "src/gen/walls.c")).read()
    m = re.search(r"wall_data\[\d+\][^=]*= \{(.*?)\};", s, re.S)
    data = [int(x) for x in m.group(1).split(",") if x.strip()]
    t = '#include "gen.h"\n' + c_words("wall_data2", [b * 257 for b in data], "u16", per_line=16, align=2)
    write(os.path.join(outdir, "src/gen/walls2.c"), t)
    return len(data) * 2

if __name__ == "__main__":
    print("doubled wall bytes:", gen_walls2(sys.argv[1]))
