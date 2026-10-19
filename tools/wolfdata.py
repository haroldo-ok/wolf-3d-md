"""Readers for the Wolfenstein 3D (shareware v1.4, .WL1) data files.

VSWAP   : wall pages, sprite pages (post-compressed shapes), digitised sounds
MAPHEAD/GAMEMAPS : RLEW + Carmack compressed levels
VGAHEAD/VGADICT/VGAGRAPH : Huffman compressed pictures, fonts
AUDIOHED/AUDIOT : PC speaker / AdLib sounds and IMF music
GAMEPAL.OBJ (from the GPL source) : the 256 colour VGA palette
"""
import struct, os

class Data:
    def __init__(self, datadir, srcdir, ext="WL1"):
        self.d = datadir
        self.ext = ext
        self.src = srcdir
        self.load_palette()
        self.load_vswap()
        self.load_maps()
        self.load_vga()
        self.load_audio()

    def rd(self, name):
        with open(os.path.join(self.d, "%s.%s" % (name, self.ext)), "rb") as f:
            return f.read()

    # ------------------------------------------------------------ palette
    def load_palette(self):
        with open(os.path.join(self.src, "WOLFSRC", "OBJ", "GAMEPAL.OBJ"), "rb") as f:
            o = f.read()
        pos = 0
        pal = None
        while pos < len(o):
            t = o[pos]
            ln = struct.unpack_from("<H", o, pos + 1)[0]
            rec = o[pos + 3: pos + 3 + ln - 1]
            if t == 0xA0 and len(rec) >= 3 + 768:
                pal = rec[3:3 + 768]
                break
            pos += 3 + ln
        assert pal is not None and len(pal) == 768
        self.pal6 = [tuple(pal[i * 3:i * 3 + 3]) for i in range(256)]
        assert max(max(c) for c in self.pal6) <= 63
        # 6 bit VGA DAC -> 8 bit
        self.pal8 = [tuple((v * 255 + 31) // 63 for v in c) for c in self.pal6]

    # -------------------------------------------------------------- vswap
    def load_vswap(self):
        v = self.rd("VSWAP")
        self.vswap = v
        n, sprstart, sndstart = struct.unpack_from("<HHH", v, 0)
        self.nchunks, self.spritestart, self.soundstart = n, sprstart, sndstart
        offs = struct.unpack_from("<%dI" % n, v, 6)
        lens = struct.unpack_from("<%dH" % n, v, 6 + 4 * n)
        self.voffs, self.vlens = offs, lens

    def page(self, i):
        if self.voffs[i] == 0:
            return None
        return self.vswap[self.voffs[i]:self.voffs[i] + self.vlens[i]]

    def num_walls(self):
        return self.spritestart

    def num_sprites(self):
        return self.soundstart - self.spritestart

    def wall(self, i):
        """64x64, returned as list of 64 columns (each 64 palette indices)"""
        p = self.page(i)
        assert len(p) == 4096
        return [list(p[x * 64:(x + 1) * 64]) for x in range(64)]

    def sprite(self, i):
        """returns 64 columns each a list of 64 values (None = transparent)"""
        p = self.page(self.spritestart + i)
        if p is None:
            return None
        left, right = struct.unpack_from("<HH", p, 0)
        cols = [[None] * 64 for _ in range(64)]
        ofs = struct.unpack_from("<%dH" % (right - left + 1), p, 4)
        for ci, x in enumerate(range(left, right + 1)):
            q = ofs[ci]
            while True:
                end2 = struct.unpack_from("<H", p, q)[0]
                if end2 == 0:
                    break
                corr = struct.unpack_from("<h", p, q + 2)[0]
                start2 = struct.unpack_from("<H", p, q + 4)[0]
                q += 6
                # pixels for rows start2/2 .. end2/2 ; data pointer arithmetic from the original:
                # pixel(row) = p[corr + row]
                for row in range(start2 // 2, end2 // 2):
                    cols[x][row] = p[corr + row]
        return cols

    def digi_info(self):
        """list of (startpage, length) for the digitised sounds"""
        last = self.page(self.nchunks - 1)
        n = len(last) // 4
        return [struct.unpack_from("<HH", last, i * 4) for i in range(n)]

    def digi_sound(self, i):
        start, length = self.digi_info()[i]
        out = b""
        page = self.soundstart + start
        remaining = length
        while remaining > 0:
            p = self.page(page)
            take = min(len(p), remaining)
            out += p[:take]
            remaining -= take
            page += 1
        return out

    # --------------------------------------------------------------- maps
    @staticmethod
    def carmack_expand(src, outlen):
        NEAR, FAR = 0xA7, 0xA8
        out = bytearray()
        i = 0
        words = outlen // 2
        n = 0
        while n < words:
            ch = src[i] | (src[i + 1] << 8)
            hi = ch >> 8
            if hi == NEAR or hi == FAR:
                cnt = ch & 0xFF
                if cnt == 0:
                    # escaped literal
                    out += bytes([src[i + 2], hi])
                    i += 3
                    n += 1
                elif hi == NEAR:
                    off = src[i + 2]
                    i += 3
                    for _ in range(cnt):
                        s = len(out) - off * 2
                        out += out[s:s + 2]
                        n += 1
                else:
                    off = src[i + 2] | (src[i + 3] << 8)
                    i += 4
                    for k in range(cnt):
                        s = (off + k) * 2
                        out += out[s:s + 2]
                        n += 1
            else:
                out += src[i:i + 2]
                i += 2
                n += 1
        return bytes(out)

    @staticmethod
    def rlew_expand(src, outlen, tag):
        out = bytearray()
        i = 0
        while len(out) < outlen:
            w = src[i] | (src[i + 1] << 8)
            i += 2
            if w == tag:
                cnt = src[i] | (src[i + 1] << 8)
                val = src[i + 2:i + 4]
                i += 4
                out += val * cnt
            else:
                out += src[i - 2:i]
        return bytes(out[:outlen])

    def load_maps(self):
        mh = self.rd("MAPHEAD")
        gm = self.rd("GAMEMAPS")
        self.rlewtag = struct.unpack_from("<H", mh, 0)[0]
        offs = struct.unpack_from("<100I", mh, 2)
        self.maps = []
        for mi, o in enumerate(offs):
            if o == 0 or o == 0xFFFFFFFF:
                continue
            h = struct.unpack_from("<3I3HHH16s", gm, o)
            planestart = h[0:3]
            planelen = h[3:6]
            w, hh = h[6], h[7]
            name = h[8].split(b"\0")[0].decode("ascii", "replace")
            planes = []
            for pi in range(3):
                comp = gm[planestart[pi]:planestart[pi] + planelen[pi]]
                expl = struct.unpack_from("<H", comp, 0)[0]
                car = self.carmack_expand(comp[2:], expl)
                rl_len = struct.unpack_from("<H", car, 0)[0]
                data = self.rlew_expand(car[2:], rl_len, self.rlewtag)
                planes.append(list(struct.unpack("<%dH" % (len(data) // 2), data)))
            self.maps.append(dict(index=mi, name=name, width=w, height=hh, planes=planes))

    # ---------------------------------------------------------------- vga
    def load_vga(self):
        head = self.rd("VGAHEAD")
        self.vgaoffs = []
        for i in range(len(head) // 3):
            o = head[i * 3] | (head[i * 3 + 1] << 8) | (head[i * 3 + 2] << 16)
            self.vgaoffs.append(o)
        dic = self.rd("VGADICT")
        self.huff = [struct.unpack_from("<HH", dic, i * 4) for i in range(255)]
        self.vgagraph = self.rd("VGAGRAPH")

    def huff_expand(self, src, outlen):
        out = bytearray()
        node = 254
        i = 0
        bit = 0
        while len(out) < outlen:
            byte = src[i]
            for b in range(8):
                nxt = self.huff[node][(byte >> b) & 1]
                if nxt < 256:
                    out.append(nxt)
                    node = 254
                    if len(out) >= outlen:
                        break
                else:
                    node = nxt - 256
            i += 1
        return bytes(out)

    def vga_chunk(self, i, explen=None):
        o = self.vgaoffs[i]
        nxt = o
        j = i + 1
        while j < len(self.vgaoffs) and (self.vgaoffs[j] == 0xFFFFFF or self.vgaoffs[j] == o):
            j += 1
        nxt = self.vgaoffs[j] if j < len(self.vgaoffs) else len(self.vgagraph)
        raw = self.vgagraph[o:nxt]
        if explen is None:
            explen = struct.unpack_from("<I", raw, 0)[0]
            raw = raw[4:]
        return self.huff_expand(raw, explen)

    def load_audio(self):
        h = self.rd("AUDIOHED")
        self.audoffs = struct.unpack("<%dI" % (len(h) // 4), h)
        self.audiot = self.rd("AUDIOT")

    def audio_chunk(self, i):
        a, b = self.audoffs[i], self.audoffs[i + 1]
        return self.audiot[a:b]


def deplane(raw, w, h):
    """VGA mode-X style planar picture -> list of rows of palette indices"""
    qw = w // 4
    rows = []
    for y in range(h):
        row = [0] * w
        for x in range(w):
            row[x] = raw[(x & 3) * (qw * h) + y * qw + (x >> 2)]
        rows.append(row)
    return rows
