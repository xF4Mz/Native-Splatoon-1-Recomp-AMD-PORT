"""sarc_lite.py -- just enough of the Wii U archive formats to read a model's skeleton: Yaz0, SARC, BFRES's FSKL.
Our own code (formats as documented by the homebrew community); used by emote_convert.py."""
import struct

def yaz0(d):
    if d[:4] != b'Yaz0': return d
    size = struct.unpack('>I', d[4:8])[0]; out = bytearray(); p = 16
    while len(out) < size:
        code = d[p]; p += 1
        for _ in range(8):
            if len(out) >= size: break
            if code & 0x80: out.append(d[p]); p += 1
            else:
                b1, b2 = d[p], d[p + 1]; p += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0: n = d[p] + 0x12; p += 1
                else: n += 2
                for _ in range(n): out.append(out[-dist])
            code <<= 1
    return bytes(out)

def get(arc, name):
    """One file out of a (Yaz0-compressed) SARC, itself unpacked if it is Yaz0."""
    d = yaz0(arc)
    assert d[:4] == b'SARC'
    data_off = struct.unpack_from('>I', d, 0x0C)[0]
    n = struct.unpack_from('>H', d, 0x14 + 6)[0]
    names_off = 0x14 + 12 + n * 16 + 8
    for i in range(n):
        h, attr, s, e = struct.unpack_from('>IIII', d, 0x14 + 12 + i * 16)
        if not attr >> 24: continue
        no = names_off + (attr & 0xFFFF) * 4
        if d[no:d.index(b'\0', no)].decode() == name: return yaz0(d[data_off + s:data_off + e])
    raise KeyError(name)

def fskl(d):
    """The first skeleton in a BFRES: [{name, parent, flags, rot (4), trans (3)}] (offsets are self-relative)."""
    assert d[:4] == b'FRES'
    p = d.find(b'FSKL')
    def rel(at):
        v = struct.unpack_from('>i', d, at)[0]
        return at + v if v else 0
    nb = struct.unpack_from('>H', d, p + 8)[0]
    barr = rel(p + 0x14)
    out = []
    for i in range(nb):
        b = barr + i * 0x40
        no = rel(b)
        name = d[no:d.index(b'\0', no)].decode('ascii', 'replace')
        idx, parent, sm, rg, bb, nud, flags = struct.unpack_from('>HHhhHHI', d, b + 4)
        out.append({'name': name, 'parent': parent if parent != 0xFFFF else -1, 'flags': flags,
                    'rot': struct.unpack_from('>4f', d, b + 0x20), 'trans': struct.unpack_from('>3f', d, b + 0x30)})
    return out
