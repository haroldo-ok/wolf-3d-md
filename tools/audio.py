"""Audio conversion for the Genesis port.

* digitised sounds (7 kHz unsigned 8 bit)   -> 16 kHz signed 8 bit for the Z80 4-channel PCM driver
* AdLib sound effects (OPL2 register data)  -> rendered by a small software OPL2 model to 16 kHz PCM
* IMF music (OPL2 register writes @ 700 Hz) -> YM2612 + SN76489 register streams played by the 68000

Everything is generated from the original AUDIOT / VSWAP data.
"""
import math, os, re, struct
import numpy as np
from cgen import write, c_bytes

PCM_RATE = 16000
NUMSOUNDS = 87
STARTADLIB = 87
STARTMUSIC = 261
NUMSONGS = 27


# ---------------------------------------------------------------- digitised sounds

NDIGI = 46


def digimap(srcdir):
    t = open(os.path.join(srcdir, "WOLFSRC", "WL_MAIN.C"), encoding="latin-1").read()
    i = t.index("static  int     wolfdigimap[]")
    j = t.index("LASTSOUND", i)
    body = t[i:j]
    # honour the same #ifdef structure: not SPEAR, not UPLOAD (full list)
    body = re.sub(r"#ifdef SPEAR.*?#else", "", body, flags=re.S) if False else body
    names = re.findall(r"^\s*([A-Z0-9_]+SND)\s*,\s*(\d+)\s*,", body, flags=re.M)
    audionames = open(os.path.join(srcdir, "WOLFSRC", "AUDIOWL6.H"), encoding="latin-1").read()
    ids = {}
    for m in re.finditer(r"\b([A-Z0-9_]+SND)\s*,\s*//\s*(\d+)", audionames):
        ids[m.group(1)] = int(m.group(2))
    dm = {}
    for n, v in names:
        if n in ids and ids[n] < NUMSOUNDS and int(v) < NDIGI:
            dm.setdefault(ids[n], int(v))
    return dm


def resample_digi(raw):
    x = np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0
    n_out = int(round(len(x) * PCM_RATE / 7000.0))
    t = np.arange(n_out) * (7000.0 / PCM_RATE)
    y = np.interp(t, np.arange(len(x)), x)
    y = np.clip(np.round(y), -128, 127).astype(np.int8)
    return y


# ------------------------------------------------------------------ OPL2 model

KSLROM = [0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64]


def attack_ms(r):
    return 0.0 if r >= 15 else (1e12 if r == 0 else 2826.0 / (2 ** (r - 1)))


def decay_db_per_ms(r):
    if r == 0:
        return 0.0
    return 96.0 / (39280.0 / (2 ** (r - 1)))


def wave(w, p):
    s = math.sin(2 * math.pi * p)
    if w == 0:
        return s
    if w == 1:
        return s if s > 0 else 0.0
    if w == 2:
        return abs(s)
    q = p % 0.5
    return abs(s) if q < 0.25 else 0.0


class Op:
    def __init__(self, chr_, scale, attack, sus, wav):
        self.mult = chr_ & 15
        self.mult = 0.5 if self.mult == 0 else (self.mult if self.mult < 11 else {11: 10, 12: 12, 13: 12, 14: 15, 15: 15}[self.mult])
        self.egt = bool(chr_ & 0x20)
        self.ksl = scale >> 6
        self.tl = (scale & 63) * 0.75
        self.ar = attack >> 4
        self.dr = attack & 15
        self.sl = (sus >> 4) * 3.0
        self.rr = sus & 15
        self.wav = wav & 3
        self.phase = 0.0
        self.att = 96.0          # attenuation in dB (96 = silent)
        self.stage = 'off'
        self.keyon = False
        self.o1 = 0.0
        self.o2 = 0.0

    def key(self, on):
        if on and not self.keyon:
            self.stage = 'attack'
            self.att = 96.0 if self.att > 90 else self.att
            if self.ar >= 15:
                self.att = 0.0
        elif not on and self.keyon:
            self.stage = 'release'
        self.keyon = on

    def step_env(self, ms):
        if self.stage == 'attack':
            if self.ar >= 15:
                self.att = 0.0
                self.stage = 'decay'
            elif self.ar == 0:
                pass
            else:
                self.att -= 96.0 * ms / attack_ms(self.ar)
                if self.att <= 0.0:
                    self.att = 0.0
                    self.stage = 'decay'
        elif self.stage == 'decay':
            self.att += decay_db_per_ms(self.dr * 1) * 0 + (96.0 / (39280.0 / (2 ** (self.dr - 1))) * ms if self.dr else 0.0)
            if self.att >= self.sl:
                self.att = self.sl
                self.stage = 'sustain'
        elif self.stage == 'sustain':
            if not self.egt and self.rr:
                self.att = min(96.0, self.att + 96.0 / (39280.0 / (2 ** (self.rr - 1))) * ms)
        elif self.stage == 'release':
            if self.rr:
                self.att = min(96.0, self.att + 96.0 / (39280.0 / (2 ** (self.rr - 1))) * ms)
            else:
                self.att = min(96.0, self.att + 96.0 / 39280.0 * 0.5 * ms)


def render_adlib(chunk):
    length, prio = struct.unpack_from("<IH", chunk, 0)
    inst = chunk[6:22]
    block = chunk[22]
    data = chunk[23:23 + length]
    mC, cC, mS, cS, mA, cA, mSu, cSu, mW, cW, nConn = inst[:11]
    m = Op(mC, mS, mA, mSu, mW)
    c = Op(cC, cS, cA, cSu, cW)
    fb = (nConn >> 1) & 7
    conn = nConn & 1
    fbtab = [0, math.pi / 16, math.pi / 8, math.pi / 4, math.pi / 2, math.pi, 2 * math.pi, 4 * math.pi]
    fbscale = fbtab[fb] / (2 * math.pi)   # in cycles
    spb = PCM_RATE / 140.0
    out = []
    ms_per_sample = 1000.0 / PCM_RATE
    blk = block & 7
    fnum = 0
    key = False
    n_samples = int(len(data) * spb)
    tail = int(PCM_RATE * 0.6)
    ksl_att = [0.0, 0.0]
    for i in range(n_samples + tail):
        if i < n_samples:
            bi = int(i / spb)
            if i == int(bi * spb) or i == 0 or True:
                s = data[bi]
                if s == 0:
                    if key:
                        key = False
                        m.key(False); c.key(False)
                else:
                    fnum = s
                    if not key:
                        key = True
                        m.key(True); c.key(True)
        else:
            if key:
                key = False
                m.key(False); c.key(False)
        base = fnum * 49716.0 / (1 << (20 - blk))
        # key scale level
        for k, op in enumerate((m, c)):
            if op.ksl:
                a = KSLROM[(fnum << 0) >> 6 if fnum >> 6 < 16 else 15] * 0.1875 - 3.0 * (7 - blk)
                a = max(0.0, a) * {1: 1.0, 2: 0.5, 3: 2.0}[op.ksl]
            else:
                a = 0.0
            ksl_att[k] = a
        m.step_env(ms_per_sample)
        c.step_env(ms_per_sample)
        fm = base * m.mult / PCM_RATE
        fc = base * c.mult / PCM_RATE
        m.phase = (m.phase + fm) % 1.0
        c.phase = (c.phase + fc) % 1.0
        ma = 10 ** (-(min(96.0, m.att + m.tl + ksl_att[0])) / 20.0) if m.att < 96 else 0.0
        ca = 10 ** (-(min(96.0, c.att + c.tl + ksl_att[1])) / 20.0) if c.att < 96 else 0.0
        mo = wave(m.wav, (m.phase + fbscale * (m.o1 + m.o2) * 0.5) % 1.0) * ma
        m.o2 = m.o1
        m.o1 = mo
        if conn == 0:
            co = wave(c.wav, (c.phase + mo * 2.0) % 1.0) * ca
        else:
            co = wave(c.wav, c.phase) * ca + mo
        out.append(co)
        if i >= n_samples and c.att > 80 and m.att > 80:
            break
    return np.array(out), prio


def dc_block(y):
    out = np.zeros(len(y))
    px = 0.0
    py = 0.0
    for i in range(len(y)):
        py = y[i] - px + 0.995 * py
        px = y[i]
        out[i] = py
    return out


def render_all_adlib(d):
    res = []
    for s in range(NUMSOUNDS):
        chunk = d.audio_chunk(STARTADLIB + s)
        y, prio = render_adlib(chunk)
        if len(y):
            y = dc_block(y)
            nz = np.nonzero(np.abs(y) > 0.004)[0]
            y = y[:(nz[-1] + 1 if len(nz) else 1)]
            k = min(len(y), 64)
            y[-k:] *= np.linspace(1.0, 0.0, k)
        res.append((y, prio))
    return res


# ----------------------------------------------------------------- music: IMF -> YM2612 + PSG

OPL_MOD = [0, 1, 2, 8, 9, 0xA, 0x10, 0x11, 0x12]
YM_OFF = {1: 0x00, 2: 0x08, 3: 0x04, 4: 0x0C}      # operator number -> register offset


class Stream:
    def __init__(self):
        self.rec = []          # (delay, type, a, b)
        self.pending = 0       # ticks since last record
        self.cache = {}

    def wait(self, n):
        self.pending += n

    def emit(self, typ, a, b):
        d = self.pending
        while d > 255:
            self.rec.append((255, 0, 0, 0))
            d -= 255
        self.rec.append((d, typ, a, b))
        self.pending = 0

    def ym(self, part, reg, val, force=False):
        k = (part, reg)
        if not force and self.cache.get(k) == val:
            return
        self.cache[k] = val
        self.emit(1 + part, reg, val)

    def psg(self, byte):
        self.emit(3, byte, 0)


def ym_tl(v):
    return min(127, v)


def convert_song(chunk, psg_adj=4):
    n = struct.unpack_from("<H", chunk, 0)[0]
    events = [struct.unpack_from("<BBH", chunk, 2 + 4 * k) for k in range(n // 4)]
    if not events:
        return b""
    # ---- decide voices from key-on counts
    keyons = [0] * 9
    st = [0] * 9
    for r, v, dl in events:
        if 0xB0 <= r <= 0xB8:
            c = r - 0xB0
            on = 1 if v & 0x20 else 0
            if on and not st[c]:
                keyons[c] += 1
            st[c] = on
    order = sorted(range(9), key=lambda c: -keyons[c])
    voice = {}
    fm_slots = [(0, 0), (0, 1), (0, 2), (1, 0), (1, 1)]    # (part, channel) for YM channels 1-5
    for i, c in enumerate(order):
        if keyons[c] == 0:
            continue
        if i < 5:
            voice[c] = ('fm', fm_slots[i])
        elif i < 8:
            voice[c] = ('psg', i - 5)
    s = Stream()
    opl = [0] * 256
    # init: FM channels silent ops 3/4, pan centre, algorithm set on first sync
    for part, ch in fm_slots:
        for op in (3, 4):
            off = YM_OFF[op] + ch
            s.ym(part, 0x40 + off, 127)
            s.ym(part, 0x50 + off, 31)
            s.ym(part, 0x60 + off, 0)
            s.ym(part, 0x70 + off, 0)
            s.ym(part, 0x80 + off, 0x0F)
        s.ym(part, 0xB4 + ch, 0xC0)
    for p in range(3):
        s.psg(0x80 | (p << 5) | 0x00)
        s.psg(0x00)
        s.psg(0x90 | (p << 5) | 0x0F)
    # PSG envelope state
    penv = [None] * 3

    def opl_params(c):
        mo = OPL_MOD[c]
        co = mo + 3
        return mo, co

    def sync_fm(c, part, ch):
        mo, co = opl_params(c)
        for opn, o in ((1, mo), (2, co)):
            off = YM_OFF[opn] + ch
            r20 = opl[0x20 + o]
            mult = r20 & 15
            tl = opl[0x40 + o] & 63
            ar = opl[0x60 + o] >> 4
            dr = opl[0x60 + o] & 15
            sl = opl[0x80 + o] >> 4
            rr = opl[0x80 + o] & 15
            egt = bool(r20 & 0x20)
            ar5 = 31 if ar >= 15 else ar * 2
            d1r = min(31, dr * 2)
            d2r = 0 if egt else min(31, rr * 2)
            s.ym(part, 0x30 + off, mult)
            s.ym(part, 0x40 + off, ym_tl(tl))
            s.ym(part, 0x50 + off, ar5)
            s.ym(part, 0x60 + off, d1r)
            s.ym(part, 0x70 + off, d2r)
            s.ym(part, 0x80 + off, (sl << 4) | rr)
        conn = opl[0xC0 + c] & 1
        fb = (opl[0xC0 + c] >> 1) & 7
        s.ym(part, 0xB0 + ch, (fb << 3) | (4 if conn == 0 else 7))

    def fm_key(part, ch, on):
        chn = ch + (4 if part else 0)
        s.emit(1, 0x28, (0x30 if on else 0x00) | chn)    # operators 1+2 (bits 4,5)
        s.cache.pop((0, 0x28), None)

    def fm_freq(c, part, ch):
        f = ((opl[0xB0 + c] & 3) << 8) | opl[0xA0 + c]
        blk = (opl[0xB0 + c] >> 2) & 7
        fy = min(2047, int(round(f * 1.8667)))
        s.ym(part, 0xA4 + ch, (blk << 3) | (fy >> 8))
        s.ym(part, 0xA0 + ch, fy & 0xFF)

    keystate = [0] * 9

    def psg_tone(c, p):
        mo, co = opl_params(c)
        f = ((opl[0xB0 + c] & 3) << 8) | opl[0xA0 + c]
        blk = (opl[0xB0 + c] >> 2) & 7
        mult = opl[0x20 + co] & 15
        mult = 0.5 if mult == 0 else mult
        hz = f * 49716.0 / (1 << (20 - blk)) * mult
        if hz < 20:
            hz = 20
        period = int(round(3579545 / (32.0 * hz)))
        while period > 1023:
            period //= 2          # octave up
        period = max(1, period)
        s.psg(0x80 | (p << 5) | (period & 15))
        s.psg((period >> 4) & 0x3F)

    # carrier envelope simulation for PSG voices
    def psg_env_start(c, p):
        mo, co = opl_params(c)
        r20 = opl[0x20 + co]
        tl = (opl[0x40 + co] & 63) * 0.75
        ar = opl[0x60 + co] >> 4
        dr = opl[0x60 + co] & 15
        sl = (opl[0x80 + co] >> 4) * 3.0
        rr = opl[0x80 + co] & 15
        penv[p] = dict(c=c, stage='attack' if ar < 15 else 'decay', att=96.0 if ar < 15 else 0.0, tl=tl, ar=ar, dr=dr,
                       sl=sl, rr=rr, egt=bool(r20 & 0x20), last=None)

    def psg_env_step(p):
        e = penv[p]
        if e is None:
            return
        ms = 1000.0 / 700.0
        if e['stage'] == 'attack':
            e['att'] -= 96.0 * ms / attack_ms(e['ar'])
            if e['att'] <= 0:
                e['att'] = 0.0
                e['stage'] = 'decay'
        elif e['stage'] == 'decay':
            if e['dr']:
                e['att'] += 96.0 / (39280.0 / (2 ** (e['dr'] - 1))) * ms
            if e['att'] >= e['sl']:
                e['att'] = e['sl']
                e['stage'] = 'sustain'
        elif e['stage'] == 'sustain':
            if not e['egt'] and e['rr']:
                e['att'] = min(96.0, e['att'] + 96.0 / (39280.0 / (2 ** (e['rr'] - 1))) * ms)
        else:
            rr = e['rr']
            e['att'] = min(96.0, e['att'] + (96.0 / (39280.0 / (2 ** (rr - 1))) * ms if rr else 0.05))
        total = e['att'] + e['tl']
        a4 = int(round(total / 2.0)) + psg_adj
        a4 = 15 if (total >= 60 or a4 > 15) else max(0, a4)
        if a4 != e['last']:
            e['last'] = a4
            s.psg(0x90 | (p << 5) | a4)
        if a4 == 15 and e['stage'] == 'release':
            penv[p] = None

    for r, v, dl in events:
        opl[r] = v
        if r in range(0x20, 0x96) or 0xC0 <= r <= 0xC8 or 0xE0 <= r <= 0xF5:
            # operator register: find channel
            o = r & 0x1F if r < 0xC0 else None
            if r >= 0xC0 and r <= 0xC8:
                c = r - 0xC0
                if c in voice and voice[c][0] == 'fm':
                    sync_fm(c, *voice[c][1])
            elif o is not None:
                for c in range(9):
                    if c in voice and voice[c][0] == 'fm' and o in (OPL_MOD[c], OPL_MOD[c] + 3):
                        sync_fm(c, *voice[c][1])
        elif 0xA0 <= r <= 0xA8:
            c = r - 0xA0
            if c in voice:
                if voice[c][0] == 'fm':
                    fm_freq(c, *voice[c][1])
                elif keystate[c]:
                    psg_tone(c, voice[c][1])
        elif 0xB0 <= r <= 0xB8:
            c = r - 0xB0
            on = 1 if v & 0x20 else 0
            if c in voice:
                kind, slot = voice[c]
                if kind == 'fm':
                    part, ch = slot
                    if on and not keystate[c]:
                        sync_fm(c, part, ch)
                        fm_freq(c, part, ch)
                        fm_key(part, ch, False)
                        fm_key(part, ch, True)
                    elif not on and keystate[c]:
                        fm_key(part, ch, False)
                    elif on:
                        fm_freq(c, part, ch)
                else:
                    p = slot
                    if on and not keystate[c]:
                        psg_tone(c, p)
                        psg_env_start(c, p)
                    elif not on and keystate[c]:
                        if penv[p]:
                            penv[p]['stage'] = 'release'
                    elif on:
                        psg_tone(c, p)
            keystate[c] = on
        # advance time
        for _ in range(dl):
            s.wait(1)
            for p in range(3):
                psg_env_step(p)
    # loop point: silence at end then restart
    s.emit(4, 0, 0)
    out = bytearray()
    for d, t, a, b in s.rec:
        out += bytes((d, t, a, b))
    return bytes(out)


# ------------------------------------------------------------------ output

def pad256(b):
    return b + bytes((-len(b)) % 256)


def gen_audio(d, srcdir, outdir):
    dm = digimap(srcdir)
    avail = set()
    for i in range(len(d.digi_info())):
        try:
            d.digi_sound(i)
            avail.add(i)
        except TypeError:
            pass
    dm = {k: v for k, v in dm.items() if v in avail}
    blob = bytearray()
    entries = []
    adl = render_all_adlib(d)
    peak = max(float(np.max(np.abs(y))) for y, _ in adl if len(y))
    for s in range(NUMSOUNDS):
        y, prio = adl[s]
        pcm = np.clip(np.round(y / peak * 100.0), -128, 127).astype(np.int8).tobytes() if len(y) else b""
        pcm = pad256(pcm if pcm else b"\0")
        aofs = len(blob)
        blob += pcm
        alen = len(pcm)
        dofs = dlen = 0
        if s in dm:
            raw = d.digi_sound(dm[s])
            dpcm = pad256(resample_digi(raw).tobytes())
            dofs = len(blob)
            blob += dpcm
            dlen = len(dpcm)
        entries.append((aofs, alen, dofs, dlen, prio))
    # music
    mus = bytearray()
    mtab = []
    for i in range(NUMSONGS):
        chunk = d.audio_chunk(STARTMUSIC + i)
        st = convert_song(chunk)
        mtab.append((len(mus), len(st)))
        mus += st
    # C source
    out = ['#include "audio.h"\n']
    out.append(c_bytes("pcm_data", bytes(blob), "u8", True, 256))
    out.append("const snd_t snd_tab[%d] = {\n" % NUMSOUNDS)
    out.append(",\n".join("{%dul,%dul,%dul,%dul,%d}" % e for e in entries))
    out.append("};\n")
    out.append(c_bytes("music_data", bytes(mus) if mus else b"\0", "u8", True))
    out.append("const mus_t music_tab[%d] = {\n" % NUMSONGS)
    out.append(",".join("{%dul,%dul}" % m for m in mtab))
    out.append("};\n")
    write(os.path.join(outdir, "src/gen", "audio.c"), "".join(out))
    h = ("#ifndef AUDIO_H\n#define AUDIO_H\n#include \"../wl_types.h\"\n"
         "typedef struct { u32 adlib_ofs, adlib_len, digi_ofs, digi_len; u16 priority; } snd_t;\n"
         "typedef struct { u32 ofs, len; } mus_t;\n"
         "extern const u8 pcm_data[];\nextern const snd_t snd_tab[%d];\nextern const u8 music_data[];\nextern const mus_t music_tab[%d];\n"
         "#define PCM_RATE_HZ 16000\n#define NUMSOUNDS_G %d\n#define NUMSONGS_G %d\n#endif\n") % (NUMSOUNDS, NUMSONGS, NUMSOUNDS, NUMSONGS)
    write(os.path.join(outdir, "src/gen", "audio.h"), h)
    return len(blob), len(mus), sum(1 for e in entries if e[3])
