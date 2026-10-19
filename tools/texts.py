"""Help / end-of-episode text (T_HELPART, T_ENDART1) -> src/gen/texts.c, with the keyboard-specific pages rewritten for the Genesis pad."""
import re, os
from cgen import write

GENESIS_PAGES = {
 1: """^PAGE 1
^c7eQUICK COMMAND REFERENCE
^c00
D-pad moves and turns. B shoots. A runs.

Start: menu
C: use (open doors, push walls)
C + Left/Right: strafe (slide sideways)
A + C: next weapon
X, Y, Z: knife, pistol, machine gun
In menus: Up/Down moves, A or C selects, B backs out

Read on for more information.
""",
 6: """^PAGE 6
^G16,16,6
^c7eCONTROLS
^c00
In Wolfenstein 3-D for the Genesis, use the control pad.

To move around the menus, press ^c6eUp ^c00and ^c6eDown ^c00to move from item to item, then press ^c6eA ^c00or ^c6eC ^c00to select that item.  ^c6eB ^c00always backs out.

The ^c6eStart ^c00button opens the menu at any time during the game.
""",
 7: """^PAGE 7
^G20,16,5
Once you are in the game, use the D-pad to move. Hold down ^c6eA ^c00to run fast.

Press ^c6eB ^c00to fire your weapon.

Hold ^c6eC ^c00and press the left and right D-pad to slide side to side instead of turning left or right.  This is called "Strafe Mode."

Press ^c6eC ^c00to Open Doors and activate elevators to go to the next floor of the keep.
""",
 8: """^PAGE 8
^G16,16,6
Press ^c6eA + C ^c00together to step through the weapons you are carrying.



The ^c6eX^c00, ^c6eY ^c00and ^c6eZ ^c00buttons of a six button pad choose the knife, pistol and machine gun directly.

Your best weapon is selected automatically when you pick one up.
""",
 9: """^PAGE 9
^G50,110,12
Once you have more than one weapon, ^c6eA + C ^c00or the ^c6eX Y Z ^c00buttons choose the weapons:








Holding down the fire button is fun, but short bursts save bullets.
""",
}


def patch_help(t):
    parts = re.split(r'(?=^\^PAGE \d+\n)', t, flags=re.M)
    out = []
    for p in parts:
        m = re.match(r'\^PAGE (\d+)\n', p)
        if m and int(m.group(1)) in GENESIS_PAGES:
            out.append(GENESIS_PAGES[int(m.group(1))])
        else:
            out.append(p)
    t = "".join(out)
    t = t.replace("Hold down Space to push the wall in front of you. Just think of Space as the Use key.",
                  "Press C to push the wall in front of you. Just think of C as the Use key.")
    return t


def cstr(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\n": out.append("\\n")
        elif ch == "\\": out.append("\\\\")
        elif ch == '"': out.append('\\"')
        elif ch == "\t": out.append("\\t")
        elif 32 <= o < 127: out.append(ch)
        elif ch == "?": out.append("\\?")
        else: out.append("\\%03o" % o)
    return "".join(out)


def gen_texts(d, outdir):
    help_t = d.vga_chunk(150).decode("latin1").replace("\r", "")
    end_t = d.vga_chunk(155).decode("latin1").replace("\r", "")
    help_t = patch_help(help_t)
    # page 1 does not start with a numbered page marker in the original layout engine's eyes: fine, "^PAGE n" starts with ^P
    def lit(name, t):
        lines = ['const char %s[] =' % name]
        for ln in t.split("\n"):
            lines.append('"%s\\n"' % cstr(ln))
        lines[-1] += ";"
        return "\n".join(lines) + "\n"
    write(os.path.join(outdir, "src/gen", "texts.c"), lit("help_text", help_t) + lit("end_text", end_t))
