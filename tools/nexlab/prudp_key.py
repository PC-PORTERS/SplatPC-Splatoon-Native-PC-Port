"""Check NEX access-key candidates against a captured PRUDP V1 packet signature.

V1 signature (NintendoClients wiki, "PRUDP Protocol"): HMAC-MD5 keyed with MD5(access key) over
header bytes after magic+version+option size+payload size (src..seq, 8 bytes), the session key
(empty before CONNECT), the sum of the access key's bytes (u32 LE), the connection signature
(empty for SYN), the options and the payload. The key is never stored in the game in clear.

usage: py -3.12 prudp_key.py RAW_HEX [candidate ...]
       py -3.12 prudp_key.py RAW_HEX --brute   (all 8-hex-digit lowercase keys; slow in Python)
"""
import hashlib, hmac, struct, sys

def split(raw):
    assert raw[:2] == b"\xEA\xD0" and raw[2] == 1, "not PRUDP V1"
    optlen, size = raw[3], struct.unpack_from("<H", raw, 4)[0]
    return raw[6:14], raw[14:30], raw[30:30 + optlen], raw[30 + optlen:30 + optlen + size]

def signature(key, header, options, payload, session_key=b"", connection_signature=b""):
    mac = hmac.new(hashlib.md5(key).digest(), digestmod=hashlib.md5)
    mac.update(header); mac.update(session_key)
    mac.update(struct.pack("<I", sum(key)))
    mac.update(connection_signature); mac.update(options); mac.update(payload)
    return mac.digest()

def main(argv):
    raw = bytes.fromhex(argv[0])
    header, sig, options, payload = split(raw)
    if "--brute" in argv:
        for n in range(1 << 32):
            k = b"%08x" % n
            if signature(k, header, options, payload) == sig:
                print("FOUND", k.decode()); return
            if n % 1000000 == 0: print(n, file=sys.stderr)
        print("not found"); return
    for c in argv[1:] or ["6f599f81"]:
        ok = signature(c.encode(), header, options, payload) == sig
        print(f"{c}: {'MATCH' if ok else 'no'}")

if __name__ == "__main__":
    main(sys.argv[1:])
