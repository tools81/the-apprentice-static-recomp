"""OS-9/68K module parsing."""
import struct


class Module:
    def __init__(self, data, base):
        self.data = data
        self.base = base
        assert data[0:2] == b'\x4a\xfc'
        (self.size, self.owner, self.nameoff) = struct.unpack('>III', data[4:16])
        (self.access, self.typelang, self.attrrev, self.edition) = struct.unpack('>HBBH', data[16:22])
        (self.execoff, self.excptoff, self.memsize, self.stacksize,
         self.idataoff, self.irefsoff) = struct.unpack('>IIIIII', data[0x30:0x48])
        e = data.index(b'\0', self.nameoff)
        self.name = data[self.nameoff:e].decode('latin-1')
        self.data = data[:self.size]
        if self.typelang == 2:
            # subroutine modules have a short header: the code follows the
            # execution offset and there is no static storage
            self.excptoff = self.memsize = self.stacksize = 0
            self.idataoff = self.irefsoff = 0

    def code_range(self):
        """(lo, hi) offsets of the code: after the header, before the
        initialized data, the module name or the CRC"""
        lo = min(0x48, self.execoff) if self.execoff >= 0x30 else 0x48
        hi = self.size - 3
        if lo < self.idataoff < hi:
            hi = self.idataoff
        if lo < self.nameoff < hi:
            hi = self.nameoff
        return lo, hi

    def w(self, off):
        return struct.unpack('>H', self.data[off:off + 2])[0]

    def l(self, off):
        return struct.unpack('>I', self.data[off:off + 4])[0]

    def idata(self):
        """returns (data_offset, bytes) for initialized data"""
        o = self.idataoff
        if not o:
            return 0, b''
        doff, cnt = struct.unpack('>II', self.data[o:o + 8])
        return doff, self.data[o + 8:o + 8 + cnt]

    def irefs(self):
        """Microware irefs: groups of (hi word, count, offsets...) terminated by 0,0.
        Two tables: code-relative refs then data-relative refs.
        returns (code_refs, data_refs) as lists of data-area offsets"""
        o = self.irefsoff
        if not o:
            return [], []
        tables = []
        for _ in range(2):
            refs = []
            while True:
                hi, cnt = struct.unpack('>HH', self.data[o:o + 4])
                o += 4
                if cnt == 0:
                    break
                for _ in range(cnt):
                    lo = struct.unpack('>H', self.data[o:o + 2])[0]
                    o += 2
                    refs.append((hi << 16) | lo)
            tables.append(refs)
        return tables[0], tables[1]
