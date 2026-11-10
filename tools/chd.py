"""Pure-Python reader for MAME CHD v5 CD-ROM images (codecs cdlz, cdzl, cdfl).

Converts a .chd disc image to the raw .bin/.cue form the rest of the tool chain
uses:

    python tools/chd.py rom/game.chd            -> rom/game.bin + rom/game.cue

Only the standard library is needed (lzma, zlib); FLAC is decoded here.
"""
import lzma
import os
import struct
import sys
import zlib

CD_FRAME = 2352
CD_SUB = 96
CD_UNIT = CD_FRAME + CD_SUB
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"

# v5 map compression types
C_TYPE0, C_TYPE1, C_TYPE2, C_TYPE3, C_NONE, C_SELF, C_PARENT = range(7)
C_RLE_SMALL, C_RLE_LARGE, C_SELF_0, C_SELF_1, C_PARENT_SELF, C_PARENT_0, C_PARENT_1 = range(7, 14)


class BitReader:
    """MSB-first bit reader (MAME bitstream_in)."""

    def __init__(self, data, pos=0):
        self.data = data
        self.bit = pos * 8

    def read(self, n):
        if n == 0:
            return 0
        v = 0
        d = self.data
        for _ in range(n):
            byte = self.bit >> 3
            b = (d[byte] >> (7 - (self.bit & 7))) & 1 if byte < len(d) else 0
            v = (v << 1) | b
            self.bit += 1
        return v


class Huffman:
    """MAME huffman_decoder<numcodes, maxbits> with RLE tree import."""

    def __init__(self, numcodes, maxbits):
        self.numcodes = numcodes
        self.maxbits = maxbits

    def import_tree_rle(self, br):
        numbits = 5 if self.maxbits >= 16 else (4 if self.maxbits >= 8 else 3)
        lens = []
        while len(lens) < self.numcodes:
            nb = br.read(numbits)
            if nb != 1:
                lens.append(nb)
            else:
                nb = br.read(numbits)
                if nb == 1:
                    lens.append(nb)
                else:
                    rep = br.read(numbits) + 3
                    lens.extend([nb] * rep)
        if len(lens) != self.numcodes:
            raise ValueError("huffman tree overflow")
        self._assign(lens)

    def _assign(self, lens):
        histo = [0] * 33
        for l in lens:
            histo[l] += 1
        cur = 0
        for codelen in range(32, 0, -1):
            nxt = (cur + histo[codelen]) >> 1
            if codelen != 1 and nxt * 2 != cur + histo[codelen]:
                raise ValueError("bad huffman tree")
            histo[codelen] = cur
            cur = nxt
        self.table = {}
        for sym, l in enumerate(lens):
            if l:
                self.table[(l, histo[l])] = sym
                histo[l] += 1

    def decode(self, br):
        code = 0
        for l in range(1, self.maxbits + 1):
            code = (code << 1) | br.read(1)
            s = self.table.get((l, code))
            if s is not None:
                return s
        raise ValueError("bad huffman code")


# ---------------------------------------------------------------- CD ECC
_F = [((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF for i in range(256)]
_B = [0] * 256
for _i in range(256):
    _B[_i ^ _F[_i]] = _i
_POFF = [[b + 86 * i for i in range(24)] for b in range(86)]
_QOFF = [[2 * (((b >> 1) * 43 + i * 44) % 1118) + (b & 1) for i in range(43)] for b in range(52)]


def _ecc_bytes(sec, row, mode2):
    v1 = v2 = 0
    for off in row:
        s = 0 if (mode2 and off < 4) else sec[12 + off]
        v1 ^= s
        v2 ^= s
        v1 = _F[v1]
    v1 = _B[_F[v1] ^ v2]
    return v1, v1 ^ v2


def ecc_generate(sec):
    mode2 = sec[15] == 2
    for b in range(86):
        sec[2076 + b], sec[2076 + 86 + b] = _ecc_bytes(sec, _POFF[b], mode2)
    for b in range(52):
        sec[2248 + b], sec[2248 + 52 + b] = _ecc_bytes(sec, _QOFF[b], mode2)


# ---------------------------------------------------------------- FLAC
class FlacReader(BitReader):
    def read_signed(self, n):
        v = self.read(n)
        if n and v & (1 << (n - 1)):
            v -= 1 << n
        return v

    def read_unary(self):
        n = 0
        while self.read(1) == 0:
            n += 1
        return n

    def read_rice(self, k):
        q = self.read_unary()
        v = (q << k) | self.read(k)
        return (v >> 1) ^ -(v & 1)

    def align(self):
        self.bit = (self.bit + 7) & ~7


def _utf8_skip(br):
    b = br.read(8)
    n = 0
    while b & 0x80:
        b = (b << 1) & 0xFF
        n += 1
    for _ in range(max(0, n - 1)):
        br.read(8)


def _flac_subframe(br, bs, bps):
    br.read(1)
    t = br.read(6)
    wasted = 0
    if br.read(1):
        wasted = 1 + br.read_unary()
        bps -= wasted
    if t == 0:
        s = [br.read_signed(bps)] * bs
    elif t == 1:
        s = [br.read_signed(bps) for _ in range(bs)]
    elif 8 <= t <= 12 or t >= 32:
        if t >= 32:
            order = (t & 31) + 1
            warm = [br.read_signed(bps) for _ in range(order)]
            prec = br.read(4) + 1
            shift = br.read_signed(5)
            coefs = [br.read_signed(prec) for _ in range(order)]
        else:
            order = t - 8
            warm = [br.read_signed(bps) for _ in range(order)]
            coefs = [[], [1], [2, -1], [3, -3, 1], [4, -6, 4, -1]][order]
            shift = 0
        res = _flac_residual(br, bs, order)
        s = warm
        if order == 0:
            s = res
        else:
            s = list(warm)
            for r in res:
                acc = 0
                for j, c in enumerate(coefs):
                    acc += c * s[-1 - j]
                s.append(r + (acc >> shift))
    else:
        raise ValueError("bad FLAC subframe type %d" % t)
    if wasted:
        s = [x << wasted for x in s]
    return s


def _flac_residual(br, bs, order):
    method = br.read(2)
    pbits, esc = (4, 15) if method == 0 else (5, 31)
    porder = br.read(4)
    parts = 1 << porder
    out = []
    for p in range(parts):
        n = (bs >> porder) - (order if p == 0 else 0)
        k = br.read(pbits)
        if k == esc:
            nb = br.read(5)
            out.extend(br.read_signed(nb) for _ in range(n))
        else:
            rr = br.read_rice
            out.extend(rr(k) for _ in range(n))
    return out


def flac_decode(data, nsamples):
    """Decode headerless 16-bit stereo FLAC frames; returns (bytes BE, consumed)."""
    br = FlacReader(data)
    out = bytearray()
    got = 0
    bs_codes = {1: 192, 2: 576, 3: 1152, 4: 2304, 5: 4608}
    while got < nsamples:
        sync = br.read(14)
        if sync != 0x3FFE:
            raise ValueError("FLAC sync lost")
        br.read(1)
        br.read(1)
        bsc = br.read(4)
        src = br.read(4)
        chan = br.read(4)
        br.read(3)
        br.read(1)
        _utf8_skip(br)
        if bsc == 6:
            bs = br.read(8) + 1
        elif bsc == 7:
            bs = br.read(16) + 1
        elif bsc in bs_codes:
            bs = bs_codes[bsc]
        else:
            bs = 256 << (bsc - 8)
        if src == 12:
            br.read(8)
        elif src in (13, 14):
            br.read(16)
        br.read(8)  # crc8
        if chan < 8:
            l = _flac_subframe(br, bs, 16)
            r = _flac_subframe(br, bs, 16)
        elif chan == 8:  # left/side
            l = _flac_subframe(br, bs, 16)
            sd = _flac_subframe(br, bs, 17)
            r = [a - b for a, b in zip(l, sd)]
        elif chan == 9:  # side/right
            sd = _flac_subframe(br, bs, 17)
            r = _flac_subframe(br, bs, 16)
            l = [a + b for a, b in zip(sd, r)]
        else:  # mid/side
            m = _flac_subframe(br, bs, 16)
            sd = _flac_subframe(br, bs, 17)
            l, r = [], []
            for a, b in zip(m, sd):
                a = (a << 1) | (b & 1)
                l.append((a + b) >> 1)
                r.append((a - b) >> 1)
        br.align()
        br.read(16)  # crc16
        for a, b in zip(l, r):
            out += struct.pack(">hh", a, b)
        got += bs
    return bytes(out), br.bit >> 3


# ---------------------------------------------------------------- CHD
class CHD:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(124)
        if h[:8] != b"MComprHD" or struct.unpack(">I", h[12:16])[0] != 5:
            raise ValueError("not a CHD v5 file")
        self.codecs = [h[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logical, self.mapoff, self.metaoff = struct.unpack(">QQQ", h[32:56])
        self.hunkbytes, self.unitbytes = struct.unpack(">II", h[56:64])
        self.hunks = (self.logical + self.hunkbytes - 1) // self.hunkbytes
        self._read_map()
        self.lzma_props = self._lzma_dict()

    def _lzma_dict(self):
        d = 1 << 26  # level 8
        rs = self.hunkbytes
        for i in range(11, 31):
            if rs <= (2 << i):
                d = 2 << i
                break
            if rs <= (3 << i):
                d = 3 << i
                break
        return [{"id": lzma.FILTER_LZMA1, "dict_size": d, "lc": 3, "lp": 0, "pb": 2}]

    def _read_map(self):
        f = self.f
        f.seek(self.mapoff)
        hdr = f.read(16)
        mapbytes = struct.unpack(">I", hdr[0:4])[0]
        firstoffs = int.from_bytes(hdr[4:10], "big")
        lengthbits, selfbits, parentbits = hdr[12], hdr[13], hdr[14]
        br = BitReader(f.read(mapbytes))
        hf = Huffman(16, 8)
        hf.import_tree_rle(br)
        comp = []
        last = 0
        rep = 0
        for _ in range(self.hunks):
            if rep:
                comp.append(last)
                rep -= 1
                continue
            v = hf.decode(br)
            if v == C_RLE_SMALL:
                comp.append(last)
                rep = 2 + hf.decode(br)
            elif v == C_RLE_LARGE:
                comp.append(last)
                rep = 2 + 16 + (hf.decode(br) << 4)
                rep += hf.decode(br)
            else:
                comp.append(v)
                last = v
        self.map = []
        cur = firstoffs
        last_self = 0
        for hn in range(self.hunks):
            c = comp[hn]
            off = cur
            length = 0
            if c <= C_TYPE3:
                length = br.read(lengthbits)
                cur += length
                br.read(16)
            elif c == C_NONE:
                length = self.hunkbytes
                cur += length
                br.read(16)
            elif c == C_SELF:
                last_self = off = br.read(selfbits)
            elif c == C_PARENT:
                raise ValueError("parent CHDs are not supported")
            elif c in (C_SELF_0, C_SELF_1):
                if c == C_SELF_1:
                    last_self += 1
                c = C_SELF
                off = last_self
            else:
                raise ValueError("parent CHDs are not supported")
            self.map.append((c, off, length))
        self.cache = {}

    def metadata(self):
        out = []
        off = self.metaoff
        while off:
            self.f.seek(off)
            m = self.f.read(16)
            ln = int.from_bytes(m[5:8], "big")
            out.append((m[:4], self.f.read(ln)))
            off = struct.unpack(">Q", m[8:16])[0]
        return out

    def stats(self):
        n = {}
        for c, _, _ in self.map:
            name = self.codecs[c].decode() if c <= C_TYPE3 else {C_NONE: "none", C_SELF: "self"}[c]
            n[name] = n.get(name, 0) + 1
        return n

    def hunk(self, hn):
        c, off, length = self.map[hn]
        if c == C_SELF:
            return self.hunk(off)
        if hn in self.cache:
            return self.cache[hn]
        self.f.seek(off)
        src = self.f.read(length)
        if c == C_NONE:
            data = src
        else:
            data = self._decompress_cd(self.codecs[c], src)
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[hn] = data
        return data

    def _decompress_cd(self, codec, src):
        frames = self.hunkbytes // CD_UNIT
        cbytes = 2 if self.hunkbytes < 65536 else 3
        eccb = (frames + 7) // 8
        hb = eccb + cbytes
        if codec == b"cdfl":
            # cdfl has no ECC/length header: FLAC frames, then deflated subcode
            base, used = flac_decode(src, frames * CD_FRAME // 4)
            sub = zlib.decompressobj(-15).decompress(src[used:], frames * CD_SUB)
            ecc = b"\0" * eccb
        else:
            clen = int.from_bytes(src[eccb:hb], "big")
            ecc = src[:eccb]
            body = src[hb:hb + clen]
            if codec == b"cdlz":
                base = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=self.lzma_props).decompress(body, frames * CD_FRAME)
            elif codec == b"cdzl":
                base = zlib.decompressobj(-15).decompress(body, frames * CD_FRAME)
            else:
                raise ValueError("unsupported codec %r" % codec)
            sub = zlib.decompressobj(-15).decompress(src[hb + clen:], frames * CD_SUB)
        if len(base) != frames * CD_FRAME:
            raise ValueError("short hunk")
        out = bytearray()
        for fr in range(frames):
            sec = bytearray(base[fr * CD_FRAME:(fr + 1) * CD_FRAME])
            if ecc[fr >> 3] & (1 << (fr & 7)):
                sec[0:12] = SYNC
                ecc_generate(sec)
            out += sec
            out += sub[fr * CD_SUB:(fr + 1) * CD_SUB].ljust(CD_SUB, b"\0")
        return bytes(out)

    def frame(self, n):
        per = self.hunkbytes // CD_UNIT
        h = self.hunk(n // per)
        o = (n % per) * CD_UNIT
        return h[o:o + CD_FRAME]


def parse_tracks(chd):
    tracks = []
    for tag, val in chd.metadata():
        if tag not in (b"CHT2", b"CHTR"):
            continue
        d = {}
        for kv in val.rstrip(b"\0").decode().split():
            k, v = kv.split(":", 1)
            d[k] = v
        tracks.append(d)
    return tracks


def swap16(b):
    a = bytearray(b)
    a[0::2], a[1::2] = b[1::2], b[0::2]
    return bytes(a)


_worker_chd = None


def _worker_init(path):
    global _worker_chd
    _worker_chd = CHD(path)


def _worker_hunk(hn):
    return _worker_chd.hunk(hn)


def _all_frames(chd_path, chd):
    """Yield every CHD frame in order, decoding hunks on all CPU cores."""
    import multiprocessing as mp
    per = chd.hunkbytes // CD_UNIT
    with mp.Pool(initializer=_worker_init, initargs=(chd_path,)) as pool:
        for h in pool.imap(_worker_hunk, range(chd.hunks), chunksize=8):
            for i in range(per):
                yield h[i * CD_UNIT:i * CD_UNIT + CD_FRAME]


def convert(chd_path, out_base):
    chd = CHD(chd_path)
    print("hunk codecs:", chd.stats())
    tracks = parse_tracks(chd)
    bin_path = out_base + ".bin"
    cue = ['FILE "%s" BINARY' % os.path.basename(bin_path)]
    file_frame = 0
    total = sum(int(t["FRAMES"]) for t in tracks)
    frames = _all_frames(chd_path, chd)
    with open(bin_path, "wb") as out:
        for t in tracks:
            n = int(t["FRAMES"])
            pregap = int(t.get("PREGAP", "0"))
            in_file_pregap = pregap if t.get("PGTYPE", "").startswith("V") else 0
            typ = t["TYPE"]
            cuetype = {"AUDIO": "AUDIO", "MODE1_RAW": "MODE1/2352", "MODE2_RAW": "MODE2/2352"}.get(typ, typ)
            cue.append("  TRACK %02d %s" % (int(t["TRACK"]), cuetype))
            if in_file_pregap:
                cue.append("    INDEX 00 %s" % msf(file_frame))
                cue.append("    INDEX 01 %s" % msf(file_frame + in_file_pregap))
            else:
                if pregap:
                    cue.append("    PREGAP %s" % msf(pregap))
                cue.append("    INDEX 01 %s" % msf(file_frame))
            for i in range(n):
                fr = next(frames)
                if typ == "AUDIO":
                    fr = swap16(fr)
                out.write(fr)
                file_frame += 1
                if file_frame % 20000 == 0:
                    print("  %d / %d frames" % (file_frame, total), flush=True)
            for _ in range(((n + 3) & ~3) - n):  # CHD pads each track to 4 frames
                next(frames)
    with open(out_base + ".cue", "w") as f:
        f.write("\n".join(cue) + "\n")
    print("wrote", bin_path, "and", out_base + ".cue")


def verify(chd_path):
    """Check every decoded hunk against the raw SHA-1 stored in the header."""
    import hashlib
    import multiprocessing as mp
    chd = CHD(chd_path)
    chd.f.seek(64)
    want = chd.f.read(20)
    h = hashlib.sha1()
    left = chd.logical
    with mp.Pool(initializer=_worker_init, initargs=(chd_path,)) as pool:
        for data in pool.imap(_worker_hunk, range(chd.hunks), chunksize=8):
            h.update(data[:left])
            left -= len(data)
    ok = h.digest() == want
    print("raw SHA-1", h.hexdigest(), "OK" if ok else "MISMATCH (expected %s)" % want.hex())
    return ok


def msf(fr):
    return "%02d:%02d:%02d" % (fr // 4500, (fr // 75) % 60, fr % 75)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    if len(sys.argv) > 2 and sys.argv[2] == "--verify":
        sys.exit(0 if verify(src) else 1)
    base = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0]
    convert(src, base)
