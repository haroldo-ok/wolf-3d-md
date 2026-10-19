"""2-D picture / font conversion: pics -> 4bpp tile-column blocks with per-group 16 colour palettes."""
import os, struct
import numpy as np
from quant import *
from cgen import *
from wolfdata import deplane

# VGA palette indices used by the original menus / text
TEXT_COLORS = dict(BORD=0x29, BORD2=0x23, DEACTIVE=0x2b, BKGD=0x2d, STRIPE=0x2c,
                   READ=0x4a, READH=0x47, VIEW=0x7f, TEXT=0x17, HIGHLIGHT=0x13,
                   BLACK=0x00, WHITE=0x0f)


def vdp(c3):
    r, g, b = [int(v) for v in c3]
    return (b << 9) | (g << 5) | (r << 1)


def group_of(name):
    if name in ("STATUSBARPIC", "KNIFEPIC", "GUNPIC", "MACHINEGUNPIC", "GATLINGGUNPIC", "NOKEYPIC", "GOLDKEYPIC",
                "SILVERKEYPIC", "N_BLANKPIC", "GOTGATLINGPIC", "MUTANTBJPIC") or name.startswith("N_") or \
            (name.startswith("FACE") and name.endswith("PIC")):
        return 0
    if name in ("C_NAMEPIC", "C_LEVELPIC", "C_SCOREPIC"):
        return 6
    if name.startswith("C_"):
        return 1
    if name.startswith("L_"):
        return 2
    if name == "TITLEPIC": return 3
    if name == "PG13PIC": return 4
    if name == "CREDITSPIC": return 5
    if name == "HIGHSCORESPIC": return 6
    if name in ("PAUSEDPIC", "GETPSYCHEDPIC"): return 8
    if name.startswith("H_"): return 7
    raise Exception(name)

GROUP_NAMES = ["HUD", "MENU", "INTER", "TITLE", "PG13", "CREDITS", "HSCORE", "HELP", "VIEW"]
GROUP_TEXTFIX = {1, 2, 6}
# colours used by the help text (^Cxx codes, background, page counter); kept as fixed palette slots
HELP_FIXED = [0x00, 0x11, 0x4f, 0x28, 0x6e, 0x7e]
HELP_CODES = [0x00, 0x11, 0x4f, 0x28, 0x29, 0x2c, 0x6d, 0x6e, 0x6f, 0x7c, 0x7e]


def gen_pics(d, names, view_pal, outdir):
    """names: list of (name, chunk).  Returns nothing; writes src/gen/pics.c, pics.h"""
    pt = d.vga_chunk(0)
    infos = {}
    for n, c in names:
        w, h = struct.unpack_from("<HH", pt, (c - 3) * 4)
        raw = d.vga_chunk(c)
        assert len(raw) == w * h, (n, w, h, len(raw))
        infos[n] = (c, w, h, np.array(deplane(raw, w, h), dtype=np.uint8))
    pal8 = np.array(d.pal8, dtype=np.float64)
    labs = rgb_to_lab(pal8)
    gpal = []
    gmap = []
    for g, gn in enumerate(GROUP_NAMES):
        members = [n for n, c in names if group_of(n) == g]
        if g == 8:
            pal = [tuple(c) for c in view_pal]
        else:
            cnt = np.zeros(256)
            for n in members:
                cnt += np.bincount(infos[n][3].ravel(), minlength=256)
            used = np.nonzero(cnt)[0]
            fixed = []
            if g == 7:
                for k in HELP_FIXED:
                    t = snap(d.pal8[k])
                    if t not in fixed:
                        fixed.append(t)
            elif g in GROUP_TEXTFIX:
                for k in TEXT_COLORS.values():
                    t = snap(d.pal8[k])
                    if t not in fixed:
                        fixed.append(t)
            else:
                fixed = [snap(d.pal8[0])] if cnt[0] else []
            cols = np.array([d.pal8[i] for i in used], dtype=np.float64)
            w = cnt[used]
            k = 16
            # distinct colours small enough? use them directly
            snapped = []
            for i in used:
                t = snap(d.pal8[i])
                if t not in snapped:
                    snapped.append(t)
            allc = list(fixed)
            for t in snapped:
                if t not in allc:
                    allc.append(t)
            if len(allc) <= 16:
                pal = allc + [(0, 0, 0)] * (16 - len(allc))
            else:
                pal, err = best_kmeans(cols, w, 16, fixed, tries=8)
            pal = [tuple(int(v) for v in c) for c in pal]
        gpal.append(pal)
        plab = palette_lab(pal)
        m, _ = nearest_index(labs, pal)
        gmap.append(m.astype(np.uint8))
    out = ["#include \"pics.h\"\n"]
    blob = bytearray()
    entries = {}
    for n, c in names:
        _, w, h, px = infos[n]
        g = group_of(n)
        m = gmap[g]
        idx = m[px]
        ofs = len(blob)
        for cx in range(w // 8):
            for y in range(h):
                for b in range(4):
                    a = idx[y, cx * 8 + b * 2]
                    bb = idx[y, cx * 8 + b * 2 + 1]
                    blob.append((int(a) << 4) | int(bb))
        entries[c] = (w // 8, h, g, ofs)
    out.append(c_bytes("pic_data", bytes(blob), "u8", True, 2))
    tab = []
    for c in range(3, 147):
        e = entries.get(c, (0, 0, 0, 0))
        tab.append("{%d,%d,%d,%d}" % (e[0], e[1], e[2], e[3]))
    out.append("const pic_t pic_tab[%d] = {%s};\n" % (144, ",".join(tab)))
    out.append("const u16 pic_group_pal[%d][16] = {\n%s};\n" % (len(GROUP_NAMES), ",\n".join(
        "{" + ",".join("0x%04X" % vdp(c) for c in p) + "}" for p in gpal)))
    lab7 = palette_lab(gpal[7])
    hm = []
    for k in HELP_CODES:
        i, _ = nearest_index(rgb_to_lab(np.array([d.pal8[k]], dtype=np.float64)), gpal[7])
        hm.append("{0x%02X,%d}" % (k, int(i[0])))
    out.append("const u8 help_cmap[%d][2] = {%s};\n" % (len(HELP_CODES), ",".join(hm)))
    out.append("const u16 border_color = 0x%04X;\n" % vdp(snap(d.pal8[0x7f])))
    write(os.path.join(outdir, "src/gen/pics.c"), "".join(out))
    # palette index of the text colours inside the menu-like groups
    h = "#ifndef PICS_H\n#define PICS_H\n#include \"../wl_types.h\"\ntypedef struct { u16 w8; u16 h; u16 group; u32 ofs; } pic_t;\n"
    h += "extern const u8 pic_data[];\nextern const pic_t pic_tab[144];\nextern const u16 border_color;\nextern const u16 pic_group_pal[%d][16];\n" % len(GROUP_NAMES)
    for i, gn in enumerate(GROUP_NAMES):
        h += "#define PG_%s %d\n" % (gn, i)
    for g in sorted(GROUP_TEXTFIX):
        pl = palette_lab(gpal[g])
        for k, v in TEXT_COLORS.items():
            lab = rgb_to_lab(np.array([d.pal8[v]], dtype=np.float64))
            i, _ = nearest_index(lab, gpal[g])
            h += "#define PC_%s_%s %d\n" % (GROUP_NAMES[g], k, int(i[0]))
    h += "#define HELP_NCOLORS %d\nextern const u8 help_cmap[HELP_NCOLORS][2];\n" % len(HELP_CODES)
    h += "#define PIC_FIRST 3\ntypedef struct { u16 height; u16 loc[256]; u8 width[256]; const u16 *data; } font_t;\nextern const font_t fonts[2];\n#endif\n"
    write(os.path.join(outdir, "src/gen/pics.h"), h)
    return len(blob)


def gen_fonts(d, outdir):
    out = ["#include \"pics.h\"\n"]
    hdr = "typedef struct { u16 height; u16 loc[256]; u8 width[256]; const u16 *data; } font_t;\nextern const font_t fonts[2];\n"
    ents = []
    for fi, chunk in enumerate((1, 2)):
        f = d.vga_chunk(chunk)
        height = struct.unpack_from("<H", f, 0)[0]
        loc = struct.unpack_from("<256H", f, 2)
        wid = list(f[2 + 512:2 + 512 + 256])
        words = []
        newloc = []
        for ch in range(256):
            newloc.append(len(words))
            w = wid[ch]
            assert w <= 16, w
            for y in range(height):
                v = 0
                for x in range(w):
                    if f[loc[ch] + y * w + x]:
                        v |= 0x8000 >> x
                words.append(v)
        out.append(c_words("font%d_data" % fi, words, "u16", 16, True, 2))
        ents.append("{%d,{%s},{%s},font%d_data}" % (height, ",".join(map(str, newloc)), ",".join(map(str, wid)), fi))
    out.append("const font_t fonts[2] = {%s};\n" % ",".join(ents))
    write(os.path.join(outdir, "src/gen/fonts.c"), "".join(out))
    return hdr
