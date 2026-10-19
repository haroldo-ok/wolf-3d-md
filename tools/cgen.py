"""Tiny helpers that emit C source for binary blobs."""
import os

def ensure(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)

def c_bytes(name, data, ctype="u8", const=True, align=None, section=None):
    out = []
    attr = ""
    if align:
        attr += " __attribute__((aligned(%d)))" % align
    out.append("%s%s %s[%d]%s = {" % ("const " if const else "", ctype, name, len(data), attr))
    line = []
    for i, b in enumerate(data):
        line.append("%d" % b)
        if len(line) == 32:
            out.append(",".join(line) + ",")
            line = []
    if line:
        out.append(",".join(line))
    out.append("};")
    return "\n".join(out) + "\n"

def c_words(name, vals, ctype, per_line=12, const=True, align=None):
    out = []
    attr = " __attribute__((aligned(%d)))" % align if align else ""
    out.append("%s%s %s[%d]%s = {" % ("const " if const else "", ctype, name, len(vals), attr))
    line = []
    for v in vals:
        line.append("%d" % v)
        if len(line) == per_line:
            out.append(",".join(line) + ",")
            line = []
    if line:
        out.append(",".join(line))
    out.append("};")
    return "\n".join(out) + "\n"

def write(path, text):
    ensure(path)
    with open(path, "w") as f:
        f.write(text)
