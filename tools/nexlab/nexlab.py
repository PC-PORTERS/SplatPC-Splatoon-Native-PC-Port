"""NEX lab server for the Splatoon PC port. Stage 1: listen and log.

Listens on 127.0.0.1 (the port confines the game to loopback), decodes PRUDP V0/V1
headers (layout: NintendoClients wiki, "PRUDP Protocol") and logs each datagram. It
answers nothing yet: the goal is the game's first packet.

usage: py -3.12 nexlab.py [--port 60000] [--log nexlab.log]
Run the game with online_server=127.0.0.1:60000 (settings.ini) or RBREW_ONLINE_SERVER.
"""
import argparse, socket, struct, sys, time

TYPES = {0: "SYN", 1: "CONNECT", 2: "DATA", 3: "DISCONNECT", 4: "PING"}
FLAGS = [(0x001, "ACK"), (0x002, "RELIABLE"), (0x004, "NEED_ACK"), (0x008, "HAS_SIZE"), (0x200, "MULTI_ACK")]

def describe(data):
    """One line per datagram; never throws."""
    if len(data) >= 2 and data[:2] == b"\xEA\xD0":
        if len(data) < 30:
            return f"PRUDP V1 truncated ({len(data)} bytes)"
        version, optlen, size = data[2], data[3], struct.unpack_from("<H", data, 4)[0]
        src, dst, typeflags = data[6], data[7], struct.unpack_from("<H", data, 8)[0]
        session, substream, seq = data[10], data[11], struct.unpack_from("<H", data, 12)[0]
        kind = TYPES.get(typeflags & 0xF, f"type{typeflags & 0xF}")
        flags = "|".join(n for b, n in FLAGS if (typeflags >> 4) & b) or "-"
        options = data[30:30 + optlen]
        payload = data[30 + optlen:30 + optlen + size]
        return (f"PRUDP V{version} {kind:<10} flags={flags:<20} src=0x{src:02X} dst=0x{dst:02X} session={session} "
                f"substream={substream} seq={seq} opt={options.hex()} payload={len(payload)}B sig={data[14:30].hex()}")
    if len(data) >= 12:   # V0 has no magic: src, dst, type/flags (2), session, signature (4), seq (2)
        src, dst = data[0], data[1]
        typeflags = struct.unpack_from("<H", data, 2)[0]
        return f"PRUDP V0? src=0x{src:02X} dst=0x{dst:02X} type={TYPES.get(typeflags & 7, typeflags & 7)} flags=0x{typeflags >> 3:X}"
    return "unknown"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=60000)
    ap.add_argument("--log", default="nexlab.log")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = forever)")
    a = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", a.port))
    sock.settimeout(0.5)
    start = time.monotonic()
    with open(a.log, "w", encoding="utf-8") as log:
        log.write(f"nexlab listening on 127.0.0.1:{a.port}\n"); log.flush()
        print(f"nexlab listening on 127.0.0.1:{a.port}", flush=True)
        count = 0
        while not a.seconds or time.monotonic() - start < a.seconds:
            try:
                data, peer = sock.recvfrom(65536)
            except socket.timeout:
                continue
            count += 1
            line = f"{time.monotonic() - start:9.3f} #{count} {peer[0]}:{peer[1]} {len(data)}B {describe(data)}\n    raw={data[:96].hex()}\n"
            log.write(line); log.flush()
            print(line, end="", flush=True)

if __name__ == "__main__":
    main()
