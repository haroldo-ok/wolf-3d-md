"""Extract the SPR_* enum (shareware, non-SPEAR) from WL_DEF.H -> list of names."""
import re

def sprite_names(wolfsrc):
    t = open(wolfsrc + "/WOLFSRC/WL_DEF.H", encoding="latin-1").read()
    a = t.index("SPR_DEMO")
    a = t.rindex("enum", 0, a)
    b = t.index("};", a)
    body = t[a:b]
    # conditional compilation: SPEAR undefined
    out = []
    skip = [False]
    stack = []
    for line in body.splitlines():
        s = line.strip()
        if s.startswith("#ifdef"):
            stack.append(skip[0]); skip[0] = skip[0] or ("SPEAR" in s)
            skip.append(skip[0]) if False else None
            continue
        if s.startswith("#ifndef"):
            stack.append(skip[0]); skip[0] = skip[0] or False
            continue
        if s.startswith("#else"):
            # toggle relative to the matching #ifdef/#ifndef on SPEAR
            skip[0] = not skip[0] if stack and stack[-1] is False else skip[0]
            continue
        if s.startswith("#endif"):
            skip[0] = stack.pop()
            continue
        if skip[0]:
            continue
        s = re.sub(r"//.*", "", s)
        for n in re.findall(r"[A-Za-z_][A-Za-z_0-9]*", s):
            if n != "enum":
                out.append(n)
    return out

if __name__ == "__main__":
    import sys
    n = sprite_names(sys.argv[1])
    print(len(n), n[:3], n[-6:])
