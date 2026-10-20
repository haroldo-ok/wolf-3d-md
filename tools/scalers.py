"""Compiled wall scalers: for every on-screen wall height a tiny 68000 routine
   (move.b N(a0),4*r(a1) ... rts) that copies the right texel for each screen row.
   a0 = texture column (64 bytes), a1 = first destination row.  Runs ~2x faster
   than the stepping loop (16 vs 30 cycles per row)."""
from cgen import *
import config as C

MOVEB = 0x1368   # move.b d16(a0),d16(a1)
RTS = 0x4E75

def variant(H):
    """code words for wall height H (rows drawn = min(H, VIEWH))"""
    step = min(0xFFFFFF, (64 << 16) // H)
    if H >= C.VIEWH:
        n = C.VIEWH
        pos = ((H - C.VIEWH) >> 1) * step + (step >> 1)
    else:
        n = H
        pos = step >> 1
    w = []
    for r in range(n):
        t = min(63, pos >> 16)
        w += [MOVEB, t, 4 * r]
        pos += step
    w.append(RTS)
    return w

def heights():
    # index i: i<=VIEWH -> H=i ; i>VIEWH -> H = VIEWH + 2*(i-VIEWH)
    hs = [0] + list(range(1, C.VIEWH + 1))
    h = C.VIEWH + 2
    while h <= 512:
        hs.append(h)
        h += 2
    return hs

def gen_scalers(outdir):
    code = []
    ofs = []
    for H in heights():
        ofs.append(len(code) * 2)
        code += variant(max(H, 1))
    t = '#include "gen.h"\n'
    t += c_words("scaler_ofs", ofs, "u32")
    t += c_words("scaler_code", code, "u16", per_line=16, align=2)
    write(os.path.join(outdir, "src/gen/scalers.c"), t)
    return len(code) * 2, len(ofs)
