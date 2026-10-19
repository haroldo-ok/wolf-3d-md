import re
#!/usr/bin/env python3
"""Wolfenstein 3D (shareware v1.4) -> Sega Genesis asset converter.

usage: convert.py DATADIR WOLFSRCDIR OUTDIR
Writes C sources / headers into OUTDIR/src/gen and sound/graphics resources
into OUTDIR/res.
"""
import sys, os, json, math, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from wolfdata import Data, deplane
from quant import *
from cgen import *
from sprnames import sprite_names
import pics as PICS
import texts as TEXTS
import audio as AUDIO
import config as C

FLOOR_COLOR = 0x19
DEFAULT_CEILING = 0x1d


def vdp(c3):
    r, g, b = [int(v) for v in c3]
    return (b << 9) | (g << 5) | (r << 1)

# ----------------------------------------------------------------------------
# palette of the 3-D view (walls + world sprites)
# ----------------------------------------------------------------------------

def build_view_palette(d, outdir):
    cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), "viewpal.json")
    if os.path.exists(cache):
        return [tuple(c) for c in json.load(open(cache))]
    allpal = d.pal8
    cnt = collections.Counter()
    for p in range(d.num_walls()):
        if d.page(p) is None:
            continue
        for col in d.wall(p):
            for v in col:
                cnt[v] += 1.0
    for s in range(d.num_sprites()):
        sp = d.sprite(s)
        if sp is None:
            continue
        for col in sp:
            for v in col:
                if v is not None:
                    cnt[v] += 0.6
    cols = np.array([allpal[i] for i in cnt])
    w = np.array([cnt[i] for i in cnt])
    fixed = [snap(allpal[FLOOR_COLOR]), snap(allpal[DEFAULT_CEILING])]
    pal, err = best_kmeans(cols, w, 16, fixed, tries=12)
    pal = [tuple(int(v) for v in c) for c in pal]
    json.dump(pal, open(cache, "w"))
    return pal


def map256(d, pal):
    lab = rgb_to_lab(np.array(d.pal8, dtype=np.float64))
    idx, _ = nearest_index(lab, pal)
    return [int(i) for i in idx]

# ----------------------------------------------------------------------------
# tables
# ----------------------------------------------------------------------------

def gen_tables(d, outdir):
    FINE = C.FINEANGLES
    ANG = C.ANGLES
    radtoint = np.float32(FINE) / np.float32(2) / np.float32(3.141592657)
    finetan = [0] * (FINE // 4)
    for i in range(FINE // 8):
        tang = math.tan((float(i) + 0.5) / float(radtoint))
        finetan[i] = int(tang * 65536)
        finetan[FINE // 4 - 1 - i] = int(1 / tang * 65536)
    sin = [0] * (ANG + ANG // 4)
    angle = np.float32(0)
    step = np.float32(3.141592657) / np.float32(2) / np.float32(ANG // 4)
    for i in range(ANG // 4 + 1):
        v = int(65536 * math.sin(float(angle)))
        v = min(v, 65535)
        sin[i] = v
        if i + ANG < len(sin):
            sin[i + ANG] = v
        sin[ANG // 2 - i] = v
        sin[ANG - i] = -v
        sin[ANG // 2 + i] = -v
        angle = np.float32(angle + step)
    # projection
    facedist = C.FOCALLENGTH + C.MINDIST
    halfview = C.VIEWW // 2
    scale = int(halfview * facedist / (C.VIEWGLOBAL / 2))
    heightnumerator = (65536 * scale) >> 6
    rays = C.VIEWW // 2
    pixelangle = [0] * rays
    for c in range(rays):
        p = (2 * c + 1) - C.VIEWW / 2.0          # pixel offset (+ = right), pair centre
        tang = p * (C.VIEWGLOBAL / C.VIEWW) / facedist
        a = math.atan(tang) * float(radtoint)
        # left of centre = larger angle
        pixelangle[c] = -int(round(a))
    rnd = [0, 8, 109, 220, 222, 241, 149, 107, 75, 248, 254, 140, 16, 66,
           74, 21, 211, 47, 80, 242, 154, 27, 205, 128, 161, 89, 77, 36,
           95, 110, 85, 48, 212, 140, 211, 249, 22, 79, 200, 50, 28, 188,
           52, 140, 202, 120, 68, 145, 62, 70, 184, 190, 91, 197, 152, 224,
           149, 104, 25, 178, 252, 182, 202, 182, 141, 197, 4, 81, 181, 242,
           145, 42, 39, 227, 156, 198, 225, 193, 219, 93, 122, 175, 249, 0,
           175, 143, 70, 239, 46, 246, 163, 53, 163, 109, 168, 135, 2, 235,
           25, 92, 20, 145, 138, 77, 69, 166, 78, 176, 173, 212, 166, 113,
           94, 161, 41, 50, 239, 49, 111, 164, 70, 60, 2, 37, 171, 75,
           136, 156, 11, 56, 42, 146, 138, 229, 73, 146, 77, 61, 98, 196,
           135, 106, 63, 197, 195, 86, 96, 203, 113, 101, 170, 247, 181, 113,
           80, 250, 108, 7, 255, 237, 129, 226, 79, 107, 112, 166, 103, 241,
           24, 223, 239, 120, 198, 58, 60, 82, 128, 3, 184, 66, 143, 224,
           145, 224, 81, 206, 163, 45, 63, 90, 168, 114, 59, 33, 159, 95,
           28, 139, 123, 98, 125, 196, 15, 70, 194, 253, 54, 14, 109, 226,
           71, 17, 161, 93, 186, 87, 244, 138, 20, 52, 123, 251, 26, 36,
           17, 46, 52, 231, 232, 76, 31, 221, 84, 37, 216, 165, 212, 106,
           197, 242, 98, 43, 39, 175, 254, 145, 190, 84, 118, 222, 187, 136,
           120, 163, 236, 249]
    assert len(rnd) == 256, len(rnd)
    # reciprocal table: 65536*64/h as 16.16 step for small heights
    recip = [0] + [min(0xFFFFFF, (64 << 16) // h) for h in range(1, 513)]
    t = "#include \"gen.h\"\n"
    t += c_words("sintable", sin, "s32")
    t += c_words("finetangent", finetan, "s32")
    t += c_words("pixelangle", pixelangle, "s16")
    t += c_bytes("rndtable", rnd)
    t += c_words("recip_step", recip, "u32")
    t += "const s32 proj_scale = %d;\nconst s32 proj_heightnumerator = %d;\n" % (scale, heightnumerator)
    write(os.path.join(outdir, "src/gen/tables.c"), t)
    return scale, heightnumerator

# ----------------------------------------------------------------------------
# walls and sprites
# ----------------------------------------------------------------------------

def gen_walls(d, m256, outdir):
    blob = bytearray()
    ofs = []
    for p in range(d.num_walls()):
        if d.page(p) is None:
            ofs.append(0xFFFFFFFF)
            continue
        ofs.append(len(blob))
        cols = d.wall(p)
        for x in range(64):
            for y in range(64):
                i = m256[cols[x][y]]
                blob.append(i * 17)
    t = "#include \"gen.h\"\n"
    t += c_words("wall_ofs", ofs, "u32")
    t += c_bytes("wall_data", blob, align=2)
    write(os.path.join(outdir, "src/gen/walls.c"), t)
    return len(blob)


def encode_sprite(cols, m256):
    """-> (left, right, colofs list (relative), data bytes)"""
    nonempty = [x for x in range(64) if any(v is not None for v in cols[x])]
    if not nonempty:
        return None
    left, right = nonempty[0], nonempty[-1]
    data = bytearray()
    colofs = []
    for x in range(left, right + 1):
        colofs.append(len(data))
        col = cols[x]
        y = 0
        while y < 64:
            if col[y] is None:
                y += 1
                continue
            y0 = y
            while y < 64 and col[y] is not None:
                y += 1
            data.append(y0)
            data.append(y)
            for yy in range(y0, y):
                data.append(m256[col[yy]] * 17)
        data.append(0xFF)
    if len(data) & 1:
        data.append(0xFF)
    return left, right, colofs, data


def gen_sprites(d, m256, outdir):
    n = d.num_sprites()
    hdrs = []
    colofs_all = []
    blob = bytearray()
    for s in range(n):
        sp = d.sprite(s)
        enc = encode_sprite(sp, m256) if sp is not None else None
        if enc is None:
            hdrs.append((0xFFFFFFFF, 0, 0, 0))
            continue
        left, right, colofs, data = enc
        hdrs.append((len(blob), len(colofs_all), left, right))
        colofs_all.extend(colofs)
        blob.extend(data)
    t = "#include \"gen.h\"\n"
    t += "const sprhdr_t spr_hdr[%d] = {\n" % n
    for h in hdrs:
        t += "{%d,%d,%d,%d},\n" % (h[0], h[1], h[2], h[3])
    t += "};\n"
    t += c_words("spr_colofs", colofs_all, "u16")
    t += c_bytes("spr_data", blob, align=2)
    write(os.path.join(outdir, "src/gen/sprites.c"), t)
    return len(blob), len(colofs_all) * 2


# ----------------------------------------------------------------------------
# maps
# ----------------------------------------------------------------------------
CEILING_E1 = [0x1d] * 9 + [0xbf]
PAR_E1 = [(1.5, "01:30"), (2, "02:00"), (2, "02:00"), (3.5, "03:30"), (3, "03:00"),
          (3, "03:00"), (2.5, "02:30"), (2.5, "02:30"), (0, "??:??"), (0, "??:??")]


def gen_maps(d, pal3, outdir):
    t = "#include \"gen.h\"\n"
    nm = len(d.maps)
    for i, m in enumerate(d.maps):
        p0 = m["planes"][0]
        p1 = m["planes"][1]
        assert max(p0) < 256, max(p0)
        t += c_bytes("map%d_p0" % i, p0)
        t += c_words("map%d_p1" % i, p1, "u16", per_line=16)
    t += "const mapinfo_t mapinfo[%d] = {\n" % nm
    allpal = d.pal8
    for i, m in enumerate(d.maps):
        cc = CEILING_E1[i]
        c3 = snap(allpal[cc])
        t += "{map%d_p0,map%d_p1,\"%s\",0x%04X,%d},\n" % (i, i, m["name"], vdp(c3), int(PAR_E1[i][0] * 60 * 70))
    t += "};\n"
    write(os.path.join(outdir, "src/gen/maps.c"), t)


def gen_header(d, pal3, outdir):
    h = "#ifndef GEN_H\n#define GEN_H\n#include \"wl_types.h\"\n"
    h += "#define VIEWW %d\n#define VIEWH %d\n" % (C.VIEWW, C.VIEWH)
    h += "#define NUMWALLPAGES %d\n#define NUMSPRITES %d\n#define NUMMAPS %d\n" % (d.num_walls(), d.num_sprites(), len(d.maps))
    h += "#define FLOOR_PALIDX 0\n#define CEIL_PALIDX 1\n"
    h += "typedef struct { u32 data_ofs; u16 colidx; u8 left, right; } sprhdr_t;\n"
    h += "typedef struct { const u8 *p0; const u16 *p1; const char *name; u16 ceilcolor; s32 partics; } mapinfo_t;\n"
    h += "extern const s32 sintable[%d];\nextern const s32 finetangent[%d];\n" % (C.ANGLES + C.ANGLES // 4, C.FINEANGLES // 4)
    h += "extern const s16 pixelangle[%d];\nextern const u8 rndtable[256];\nextern const u32 recip_step[%d];\n" % (C.VIEWW // 2, 513)
    h += "extern const s32 proj_scale, proj_heightnumerator;\n"
    h += "extern const u32 wall_ofs[NUMWALLPAGES];\nextern const u8 wall_data[];\n"
    h += "extern const sprhdr_t spr_hdr[NUMSPRITES];\nextern const u16 spr_colofs[];\nextern const u8 spr_data[];\n"
    h += "extern const mapinfo_t mapinfo[NUMMAPS];\n"
    h += "extern const u16 view_palette[16];\n"
    h += "#endif\n"
    write(os.path.join(outdir, "src/gen/gen.h"), h)
    t = "#include \"gen.h\"\nconst u16 view_palette[16] = {" + ",".join("0x%04X" % vdp(c) for c in pal3) + "};\n"
    write(os.path.join(outdir, "src/gen/palette.c"), t)


def gen_sprnames(srcdir, outdir):
    names = sprite_names(srcdir)
    t = "#ifndef SPRNAMES_H\n#define SPRNAMES_H\n"
    for i, n in enumerate(names):
        t += "#define %s %d\n" % (n, i)
    t += "#define SPR_MACHINEGUNATK3 MACHINEGUNATK3\n"
    t += "#endif\n"
    write(os.path.join(outdir, "src/gen/sprnames.h"), t)


def gen_namehdrs(srcdir, outdir):
    # audio: v1.4 data matches AUDIOWL6.H (87 sounds, 288 chunks)
    t = open(os.path.join(srcdir, "WOLFSRC", "AUDIOWL6.H"), encoding="latin-1").read()
    write(os.path.join(outdir, "src/gen", "audionames.h"), "#ifndef WL_AUDIONAMES_H\n#define WL_AUDIONAMES_H\n%s\n#endif\n" % t)
    # graphics: v1.4 shareware pictable = repo WL1 header + 8 extra pics
    h = open(os.path.join(srcdir, "WOLFSRC", "GFXV_WL1.H"), encoding="latin-1").read()
    e = h[h.index("typedef enum"):h.index("}", h.index("typedef enum"))]
    names = ["H_BJPIC"] + re.findall(r"^\s*([A-Z0-9_]+),", e, re.M)
    names = [n for n in names if n not in ("ORDERSCREEN", "ERRORSCREEN")]
    out = []
    for n in names:
        if n == "C_OPTIONSPIC":
            out.append("H_APOGEEPIC")
        out.append(n)
        if n == "C_CODEPIC":
            out += ["C_CODE2PIC", "C_LEVELPIC", "C_NAMEPIC", "C_SCOREPIC", "C_JOY1PIC", "C_JOY2PIC"]
        if n == "L_EXPOINTPIC":
            out.append("L_APOSTROPHEPIC")
    assert len(out) == 144, len(out)
    g = "#ifndef WL_GFXNAMES_H\n#define WL_GFXNAMES_H\n"
    for i, n in enumerate(out):
        g += "#define %s %d\n" % (n, 3 + i)
    g += "#define STARTPICS 3\n#define NUMPICS 144\n#define STARTTILE8 147\n#define NUMGFXPICS 144\n#endif\n"
    write(os.path.join(outdir, "src/gen", "gfxnames.h"), g)
    print("gfx pics:", len(out), "STATUSBAR", out.index("STATUSBARPIC") + 3, "N_0", out.index("N_0PIC") + 3, "PAUSED", out.index("PAUSEDPIC") + 3)
    return [(n, 3 + i) for i, n in enumerate(out)]


def gen_soundtab(srcdir, outdir):
    t = open(os.path.join(srcdir, "WOLFSRC", "WL_GAME.C"), encoding="latin-1").read()
    out = "#include \"gen.h\"\n"
    for name in ("righttable", "lefttable"):
        i = t.index("byte %s[" % name)
        j = t.index("};", i)
        nums = [int(x) for x in re.findall(r"\d+", t[t.index("{", i):j])]
        assert len(nums) == 15 * 30, len(nums)
        out += c_bytes(name, nums)
    write(os.path.join(outdir, "src/gen/soundtab.c"), out)


def main():
    datadir, srcdir, outdir = sys.argv[1:4]
    d = Data(datadir, srcdir)
    pal3 = build_view_palette(d, outdir)
    m256 = map256(d, pal3)
    print("view palette:", pal3)
    gen_header(d, pal3, outdir)
    print("tables:", gen_tables(d, outdir))
    print("walls bytes:", gen_walls(d, m256, outdir))
    print("sprites bytes (data, colofs):", gen_sprites(d, m256, outdir))
    gen_maps(d, pal3, outdir)
    gen_sprnames(srcdir, outdir)
    nm = gen_namehdrs(srcdir, outdir)
    gen_soundtab(srcdir, outdir)
    print('pics bytes:', PICS.gen_pics(d, nm, pal3, outdir))
    TEXTS.gen_texts(d, outdir)
    print('audio (pcm bytes, music bytes, digi sounds):', AUDIO.gen_audio(d, srcdir, outdir))
    fh = PICS.gen_fonts(d, outdir)
    open(os.path.join(outdir, 'src/gen/pics.h'), 'a').write('')


if __name__ == "__main__":
    main()
