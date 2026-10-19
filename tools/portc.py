#!/usr/bin/env python3
"""Mechanical first pass: 16-bit DOS C -> 68000 C (types, keywords)."""
import re, sys

def port(t):
    t = t.replace("\r", "")
    t = re.sub(r'#include\s+"WL_DEF.H"', '#include "wl_def.h"', t, flags=re.I)
    t = re.sub(r'#pragma hdrstop\n', '', t)
    t = re.sub(r'\b(far|near|huge|_seg|cdecl|pascal)\b\s*', '', t)
    t = re.sub(r'\bunsigned\s+long\b', 'u32', t)
    t = re.sub(r'\bunsigned\s+int\b', 'u16', t)
    t = re.sub(r'\bunsigned\s+char\b', 'u8', t)
    t = re.sub(r'\bunsigned\b', 'u16', t)
    t = re.sub(r'\blong\b', 's32', t)
    t = re.sub(r'\bint\b', 's16', t)
    t = re.sub(r'\bnew\b', 'newobj', t)
    t = re.sub(r'\bstatetype\s*\*', 'const statetype *', t)
    t = re.sub(r'\bboolean\b', 'boolean', t)
    t = re.sub(r'\bfalse\b', 'FALSE', t)
    t = re.sub(r'\btrue\b', 'TRUE', t)
    t = t.replace("US_RndT", "US_RndT")
    return t

if __name__ == "__main__":
    src, dst = sys.argv[1:3]
    open(dst, "w").write(port(open(src, encoding="latin-1").read()))
