"""Compiled wall scalers: for every on-screen wall height a tiny 68000 routine
   that copies the right texel for each screen row.
   a0 = texture column (64 bytes), a1 = first destination row, d0 is scratch.

   Heights >= 64 never skip a texel (the texel index moves by 0 or 1 per row), so
   the code can walk the texture column with a0 instead of addressing every texel
   with its own displacement:

       move.b d16(a0),d16(a1)    20 cycles  (the original encoding, H < 64)
       move.b (a0),d16(a1)       16 cycles  texel repeated, a0 stays
       move.b (a0)+,d16(a1)      16 cycles  last row of a texel, a0 moves on
       move.b (a0)+,d0           8 cycles   texel repeated 3+ times: load once...
       move.b d0,d16(a1)         12 cycles  ...and store it once per row

   Runs of 3 or more rows use the d0 form (8 + 12 per row, cheaper than 16 per
   row from three rows up); a wall taller than the 128 row view starts part way
   down the column, which costs one lea d16(a0),a0."""
from cgen import *
import config as C

MOVEB = 0x1368      # move.b d16(a0),d16(a1)  20 cycles, 6 bytes
MOVEB_A = 0x1350    # move.b (a0),d16(a1)     16 cycles, 4 bytes
MOVEB_AP = 0x1358   # move.b (a0)+,d16(a1)    16 cycles, 4 bytes
MOVEB_D0 = 0x1340   # move.b d0,d16(a1)       12 cycles, 4 bytes
LOAD_D0 = 0x1018    # move.b (a0)+,d0          8 cycles, 2 bytes
LEA_A0 = 0x41E8     # lea d16(a0),a0           8 cycles, 4 bytes
RTS = 0x4E75
WORD = 0x2000       # the same moves with word operands: low detail draws rays 4 pixels wide,
                    # from textures that hold every texel twice (tools/walls2.py)

def texel_rows(H):
    """texture column index drawn on each screen row for wall height H"""
    step = min(0xFFFFFF, (64 << 16) // H)
    if H >= C.VIEWH:
        n = C.VIEWH
        pos = ((H - C.VIEWH) >> 1) * step + (step >> 1)
    else:
        n = H
        pos = step >> 1
    rows = []
    for r in range(n):
        rows.append(min(63, pos >> 16))
        pos += step
    return rows

def variant_plain(H, wide):
    """the original encoding: one move.b d16(a0),d16(a1) per row"""
    w = []
    for r, t in enumerate(texel_rows(H)):
        w += [MOVEB | WORD, 2 * t, 4 * r] if wide else [MOVEB, t, 4 * r]
    w.append(RTS)
    return w

def variant(H, wide=False):
    """code words for wall height H (rows drawn = min(H, VIEWH))"""
    rows = texel_rows(H)
    n = len(rows)
    steps = [rows[i + 1] - rows[i] for i in range(n - 1)]
    if any(s not in (0, 1) for s in steps):
        return variant_plain(H, wide)     # skips texels: needs the displacement form
    size = WORD if wide else 0
    w = []
    if rows[0]:
        w += [LEA_A0, rows[0] * (2 if wide else 1)]
    i = 0
    while i < n:
        j = i
        while j < n and rows[j] == rows[i]:
            j += 1
        last = j == n
        if j - i >= 3:
            w.append(LOAD_D0 | size)
            for r in range(i, j):
                w += [MOVEB_D0 | size, 4 * r]
        else:
            for r in range(i, j):
                w += [(MOVEB_AP if (r == j - 1 and not last) else MOVEB_A) | size, 4 * r]
        i = j
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

def scaler_set(name, wide):
    code = []
    ofs = []
    for H in heights():
        ofs.append(len(code) * 2)
        code += variant(max(H, 1), wide)
    t = c_words("scaler_code" + name, code, "u16", per_line=16, align=2)
    # scaler_tab[H], H = 0..512 pixels: the routine for that wall height and the offset of
    # its first row in the screen column (walls taller than the view start on row 0)
    t += "const scaler_ent_t scaler_tab%s[513] = {\n" % name
    for H in range(513):
        if H <= C.VIEWH:
            idx, dst = H, ((C.VIEWH - H) >> 1) << 2
        else:
            idx, dst = C.VIEWH + ((H - (C.VIEWH - 1)) >> 1), 0
        t += "{scaler_code%s+%d,%d,0},\n" % (name, ofs[idx] // 2, dst)
    t += "};\n"
    return t, len(code) * 2, len(ofs)

def gen_scalers(outdir):
    t, size, n = scaler_set("", False)
    write(os.path.join(outdir, "src/gen/scalers.c"), '#include "gen.h"\n' + t)
    # low detail: the same routines moving words
    t2, size2, _ = scaler_set("2", True)
    write(os.path.join(outdir, "src/gen/scalers2.c"), '#include "gen.h"\n' + t2)
    return size + size2, n
