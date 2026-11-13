#!/usr/bin/env python3
"""The player's weapon as hardware sprites.

usage: weapon.py OUTDIR      (also run by convert.py, after the sprites)  -> OUTDIR/src/gen/weapon.c/.h

The weapon is always drawn at exactly 2x on top of everything, so instead of scaling it into
the view every frame (and uploading it again) each of its frames is rendered here, once, into
VDP tiles that the game shows with hardware sprites.  The picture is the one the software
scaler drew: texel (t, y) of the 64x64 shape covers view pixels (64 + 2t .. 65 + 2t, 2y .. 2y + 1).

Sprite colour 0 is transparent, but the weapons do use view colour 0.  The sprites therefore
use palette line 3, a copy of the view palette whose entry WP_ZERO_IDX holds view colour 0
(no weapon uses that entry), and colour 0 pixels are stored as WP_ZERO_IDX.

Reads the converted sprites back from OUTDIR/src/gen/sprites.c, so it needs no game data."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cgen import c_bytes, c_words, write
import config as C

NFRAMES = 20                    # SPR_KNIFEREADY .. SPR_CHAINATK4
ZERO_IDX = 2                    # default; gen_weapon picks an entry no weapon uses (depends on the view palette)
MAXTILES = 152                  # per frame; two banks of this many fit below the planes
VIEW_CX0 = (40 - C.VIEWW // 8) // 2
VIEW_CY0 = (23 - C.VIEWH // 8) // 2
TILE_BASE = 1 + 2 * (C.VIEWW // 8) * (C.VIEWH // 8) + 40 * 5 + 1      # vid.h: TILE_BORDER + 1


def c_array(text, name):
    m = re.search(name + r"\[\d+\][^=]*= \{(.*?)\};", text, re.S)
    return m.group(1)


def load(outdir):
    s = open(os.path.join(outdir, "src/gen/sprites.c")).read()
    hdr = [tuple(int(x) for x in t.split(",")) for t in re.findall(r"\{(\d+,\d+,\d+,\d+)\}", c_array(s, "spr_hdr"))]
    colofs = [int(x) for x in c_array(s, "spr_colofs").split(",") if x.strip()]
    data = [int(x) for x in c_array(s, "spr_data").split(",") if x.strip()]
    names = open(os.path.join(outdir, "src/gen/sprnames.h")).read()
    first = int(re.search(r"#define SPR_KNIFEREADY (\d+)", names).group(1))
    return hdr, colofs, data, first


def frame_pixels(hdr, colofs, data, shape):
    """{(x, y): colour} in view pixels"""
    ofs, ci, left, right = hdr[shape]
    px = {}
    for t in range(left, right + 1):
        p = ofs + colofs[ci + t - left]
        while data[p] != 255:
            y0, y1 = data[p], data[p + 1]
            for y in range(y0, y1):
                b = data[p + 2 + y - y0]
                for i in (0, 1):
                    px[(64 + 2 * t, 2 * y + i)] = b >> 4
                    px[(65 + 2 * t, 2 * y + i)] = b & 15
            p += 2 + y1 - y0
    return px


def gen_weapon(outdir):
    global ZERO_IDX
    hdr, colofs, data, first = load(outdir)
    used = set()
    for f in range(NFRAMES):
        used |= set(frame_pixels(hdr, colofs, data, first + f).values())
    free = [i for i in range(2, 16) if i not in used]
    assert free, "every view palette entry is used by a weapon: no entry left for transparent colour 0"
    ZERO_IDX = 2 if 2 in free else free[0]
    tiles = bytearray()
    sat = []
    frames = []
    for f in range(NFRAMES):
        px = frame_pixels(hdr, colofs, data, first + f)
        assert px and ZERO_IDX not in px.values(), "weapon frame %d uses colour %d" % (f, ZERO_IDX)
        tx0 = min(x for x, _ in px) // 8
        tx1 = max(x for x, _ in px) // 8
        ty0 = min(y for _, y in px) // 8
        ty1 = max(y for _, y in px) // 8
        assert tx1 < C.VIEWW // 8 and ty1 < C.VIEWH // 8
        tile_ofs = len(tiles)
        entries = []            # (x, y, w, h, first tile)
        ntiles = 0
        for sy in range(ty0, ty1 + 1, 4):
            for sx in range(tx0, tx1 + 1, 4):
                w = min(4, tx1 + 1 - sx)
                h = min(4, ty1 + 1 - sy)
                entries.append((sx, sy, w, h, ntiles))
                for cx in range(sx, sx + w):        # sprite tiles are numbered column by column
                    for cy in range(sy, sy + h):
                        for row in range(8):
                            v = 0
                            for col in range(8):
                                c = px.get((cx * 8 + col, cy * 8 + row))
                                v = (v << 4) | (0 if c is None else (ZERO_IDX if c == 0 else c))
                            tiles += v.to_bytes(4, "big")
                        ntiles += 1
        assert ntiles <= MAXTILES, (f, ntiles)
        assert len(entries) <= 16
        sat_ofs = len(sat)
        for bank in (0, 1):
            for i, (sx, sy, w, h, t) in enumerate(entries):
                link = i + 1 if i + 1 < len(entries) else 0
                sat += [128 + (VIEW_CY0 + sy) * 8,
                        (((w - 1) << 2 | (h - 1)) << 8) | link,
                        0x6000 | (TILE_BASE + bank * MAXTILES + t),      # palette line 3, low priority
                        128 + (VIEW_CX0 + sx) * 8]
        frames.append((tile_ofs, ntiles * 32, sat_ofs, len(entries) * 8))
    t = '#include "weapon.h"\n'
    t += c_bytes("wp_tiles", tiles, align=2)
    t += c_words("wp_sat", sat, "u16", per_line=16, align=2)
    t += "const wpframe_t wp_frames[%d] = {\n" % NFRAMES
    for fr in frames:
        t += "{wp_tiles+%d,%d,{wp_sat+%d,wp_sat+%d},%d},\n" % (fr[0], fr[1], fr[2], fr[2] + fr[3] // 2, fr[3])
    t += "};\n"
    write(os.path.join(outdir, "src/gen/weapon.c"), t)
    h = "#ifndef WEAPON_H\n#define WEAPON_H\n#include \"wl_types.h\"\n"
    h += "#define WP_FRAMES %d\n#define WP_FIRST_SPR %d\n#define WP_ZERO_IDX %d\n" % (NFRAMES, first, ZERO_IDX)
    h += "#define WP_MAXTILES %d\n#define WP_TILE_BASE %d\n" % (MAXTILES, TILE_BASE)
    h += "/* tiles: VDP tile data; sat[bank]: sprite table entries using tile bank 0 / 1 */\n"
    h += "typedef struct { const u8 *tiles; u16 tile_bytes; const u16 *sat[2]; u16 sat_bytes; } wpframe_t;\n"
    h += "extern const wpframe_t wp_frames[WP_FRAMES];\n#endif\n"
    write(os.path.join(outdir, "src/gen/weapon.h"), h)
    return len(tiles), max(fr[1] // 32 for fr in frames), max(fr[3] // 8 for fr in frames)

if __name__ == "__main__":
    print("weapon sprites (tile bytes, most tiles, most sprites):", gen_weapon(sys.argv[1]))
