"""BYAML version 1, big-endian (Wii U / Splatoon), read and write.

Layout checked on Static.pack Mush/MapInfo.byaml: "BY", u16 version 1, u32 key table, u32 string
table, u32 root; no path table. Node types: 0xA0 string, 0xC0 array, 0xC1 dict, 0xC2 string table,
0xD0 bool, 0xD1 int, 0xD2 float, 0xFF null. Key and string tables are sorted (the game binary-searches
keys); dict entries are ordered by key index.

    data = load(bytes) -> dict/list/str/int/float/bool/None
    bytes = dump(obj)
"""
import struct

STRING, ARRAY, DICT, TABLE, BOOL, INT, FLOAT, NULL = 0xA0, 0xC0, 0xC1, 0xC2, 0xD0, 0xD1, 0xD2, 0xFF


def _table(data, off):
    assert data[off] == TABLE, "not a string table"
    count = int.from_bytes(data[off + 1:off + 4], "big")
    offs = struct.unpack_from(">%dI" % (count + 1), data, off + 4)
    return [data[off + offs[i]:data.index(b"\0", off + offs[i])].decode("utf-8") for i in range(count)]


def load(data):
    assert data[:2] == b"BY", "not a big-endian BYAML"
    keys_off, strings_off, root_off = struct.unpack_from(">III", data, 4)
    keys = _table(data, keys_off) if keys_off else []
    strings = _table(data, strings_off) if strings_off else []

    def value(kind, raw):
        if kind == STRING: return strings[raw]
        if kind == BOOL: return bool(raw)
        if kind == INT: return struct.unpack(">i", struct.pack(">I", raw))[0]
        if kind == FLOAT: return struct.unpack(">f", struct.pack(">I", raw))[0]
        if kind == NULL: return None
        if kind in (ARRAY, DICT): return node(raw)
        raise ValueError("unknown node type 0x%02X" % kind)

    def node(off):
        kind, count = data[off], int.from_bytes(data[off + 1:off + 4], "big")
        if kind == ARRAY:
            types = data[off + 4:off + 4 + count]
            base = off + 4 + ((count + 3) & ~3)
            return [value(types[i], struct.unpack_from(">I", data, base + 4 * i)[0]) for i in range(count)]
        if kind == DICT:
            out = {}
            for i in range(count):
                e = off + 4 + 8 * i
                key = keys[int.from_bytes(data[e:e + 3], "big")]
                out[key] = value(data[e + 3], struct.unpack_from(">I", data, e + 4)[0])
            return out
        raise ValueError("root/container node type 0x%02X" % kind)

    return node(root_off) if root_off else None


def dump(root):
    keys, strings = set(), set()

    def collect(o):
        if isinstance(o, dict):
            for k, v in o.items():
                keys.add(k); collect(v)
        elif isinstance(o, list):
            for v in o: collect(v)
        elif isinstance(o, str):
            strings.add(o)
    collect(root)
    keys = sorted(keys, key=lambda s: s.encode("utf-8"))
    strings = sorted(strings, key=lambda s: s.encode("utf-8"))
    kidx = {k: i for i, k in enumerate(keys)}
    sidx = {s: i for i, s in enumerate(strings)}

    def table(items):
        if not items: return b""
        enc = [s.encode("utf-8") + b"\0" for s in items]
        head = 4 + 4 * (len(enc) + 1)
        offs, pos = [], head
        for e in enc:
            offs.append(pos); pos += len(e)
        offs.append(pos)
        blob = bytes([TABLE]) + len(enc).to_bytes(3, "big") + struct.pack(">%dI" % len(offs), *offs) + b"".join(enc)
        return blob + b"\0" * (-len(blob) % 4)

    out = bytearray(b"BY" + struct.pack(">HIII", 1, 0, 0, 0))
    key_blob, str_blob = table(keys), table(strings)
    if key_blob:
        struct.pack_into(">I", out, 4, len(out)); out += key_blob
    if str_blob:
        struct.pack_into(">I", out, 8, len(out)); out += str_blob

    def kind_raw(v):
        if isinstance(v, bool): return BOOL, int(v)
        if isinstance(v, int):
            if not -2**31 <= v < 2**31: raise ValueError("int out of range: %r" % v)
            return INT, v & 0xFFFFFFFF
        if isinstance(v, float): return FLOAT, struct.unpack(">I", struct.pack(">f", v))[0]
        if isinstance(v, str): return STRING, sidx[v]
        if v is None: return NULL, 0
        if isinstance(v, list): return ARRAY, None
        if isinstance(v, dict): return DICT, None
        raise TypeError("cannot store %r" % (v,))

    def write(o):
        """Writes container o (children after it) and returns its offset."""
        start = len(out)
        if isinstance(o, dict):
            items = sorted(o.items(), key=lambda kv: kidx[kv[0]])
            out.extend(bytes([DICT]) + len(items).to_bytes(3, "big") + b"\0" * (8 * len(items)))
            for i, (k, v) in enumerate(items):
                kind, raw = kind_raw(v)
                if raw is None: raw = write(v)
                struct.pack_into(">I", out, start + 4 + 8 * i, kidx[k] << 8 | kind)
                struct.pack_into(">I", out, start + 8 + 8 * i, raw)
        else:
            n = len(o)
            kinds = [kind_raw(v) for v in o]
            pad = (n + 3) & ~3
            out.extend(bytes([ARRAY]) + n.to_bytes(3, "big") + bytes(k for k, _ in kinds) + b"\0" * (pad - n) + b"\0" * (4 * n))
            base = start + 4 + pad
            for i, (v, (kind, raw)) in enumerate(zip(o, kinds)):
                if raw is None: raw = write(v)
                struct.pack_into(">I", out, base + 4 * i, raw)
        return start

    if root is not None:
        if not isinstance(root, (dict, list)): raise TypeError("root must be a dict or a list")
        struct.pack_into(">I", out, 12, len(out))
        write(root)
    return bytes(out)
