"""Per-level tables of the original game (non-Spear-of-Destiny branch), read from the id source:
songs (WL_PLAY.C), ceiling colours (WL_DRAW.C) and par times (WL_INTER.C).  60 entries each:
six episodes of ten maps (eight floors, the boss level and the secret level)."""
import os, re


def _first_branch(txt):
    i = txt.index("#ifndef SPEAR")
    j = txt.index("#else", i)
    return txt[i:j]


def _read(srcdir, name):
    with open(os.path.join(srcdir, "WOLFSRC", name), encoding="latin-1") as f:
        return f.read()


def songs(srcdir):
    t = _read(srcdir, "WL_PLAY.C")
    s = _first_branch(t[t.index("int songs[]="):])
    names = re.findall(r"^\s*([A-Z0-9_]+_MUS)", re.sub(r"//.*", "", s), re.M)
    assert len(names) == 60, len(names)
    return names


def ceilings(srcdir):
    t = _read(srcdir, "WL_DRAW.C")
    s = _first_branch(t[t.index("unsigned vgaCeiling[]"):])
    v = [int(x, 16) & 0xFF for x in re.findall(r"0x([0-9a-fA-F]{4})", s)]
    assert len(v) == 60, len(v)
    return v


def par_times(srcdir):
    """-> list of (minutes as float, 'mm:ss')"""
    t = _read(srcdir, "WL_INTER.C")
    s = _first_branch(t[t.index("times parTimes[]"):])
    p = [(float(a), b) for a, b in re.findall(r'\{\s*([0-9.]+)\s*,\s*"([?0-9:]+)"\s*\}', s)]
    assert len(p) == 60, len(p)
    return p
