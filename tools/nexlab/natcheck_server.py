"""NAT check server (NNCS) for the NEX lab/public server.

After a matchmaking notification the game resolves nncs1.app.nintendowifi.net (and, by
hypothesis, nncs2) to learn how its NAT behaves before peer-to-peer; when the name does not
resolve it leaves the session. The port maps those names to loopback addresses
(RBREW_DNS_OVERRIDE, scripts/Launch-Online.ps1) and this answers on UDP 10025 and 10125.

Message format (kinnay/NintendoClients wiki "NAT Check Server", PretendoNetwork/nncs): 16 bytes
of big-endian u32, type first. The reply echoes the type, then the sender's port, IP and the
server's own IP. Types 1, 4, 5, 101, 103 answer from the receiving socket; 3 and 102 from the
same IP on another port; 2 from the other server's IP on another port. Unknown types: no reply.

The addresses are configurable (server_config.natcheck_addresses) so a public server can answer
on its own IP; with a single address, type 2 has no second IP and is left unanswered.
"""
import asyncio, ipaddress, logging, struct, time

log = logging.getLogger("nexserver")

ADDRESSES = ("127.0.0.1", "127.0.0.2")       # nncs1, nncs2 (lab)
PORTS = (10025, 10125)


ALTERNATE = {}          # IP -> socket bound to a random port on that IP
# Observed peer-to-peer endpoints, per source IP: ip -> [(port, monotonic time)] newest last.
# A global "last sender" was both a race (two players checking NAT at once) and a spoofing
# target: one player's check could become another player's announced endpoint. Everything
# here is keyed by the address the datagram really came from.
OBSERVATIONS = {}
MAX_OBSERVATIONS_PER_IP = 16
OBSERVATION_MAX_AGE = 120.0


RATE_LIMIT = (100, 10.0)          # at most 100 probes per 10 s from one source (a real NAT check sends about 20)
MAX_RATE_HOSTS = 65536
_RATE = {}


def rate_ok(host, now=None):
    """False when this source already used its probes for the current window (the probe is then ignored)."""
    now = time.monotonic() if now is None else now
    count, start = _RATE.get(host, (0, now))
    if now - start >= RATE_LIMIT[1]:
        count, start = 0, now
    if host not in _RATE and len(_RATE) >= MAX_RATE_HOSTS:
        _RATE.pop(next(iter(_RATE)))
    _RATE[host] = (count + 1, start)
    return count < RATE_LIMIT[0]


MAX_OBSERVED_HOSTS = 65536       # spoofed sources must not grow the table without end


def observe(host, port):
    if host not in OBSERVATIONS and len(OBSERVATIONS) >= MAX_OBSERVED_HOSTS:
        OBSERVATIONS.pop(next(iter(OBSERVATIONS)))      # oldest first (dicts keep insertion order)
    entries = OBSERVATIONS.setdefault(host, [])
    entries.append((port, time.monotonic()))
    del entries[:-MAX_OBSERVATIONS_PER_IP]


def consume(host, max_age=OBSERVATION_MAX_AGE):
    """Most recent endpoint seen from `host`, consumed once (None when unknown or stale)."""
    if not host:
        return None
    entries = OBSERVATIONS.get(host)
    if not entries:
        return None
    now = time.monotonic()
    while entries and now - entries[-1][1] > max_age:
        entries.pop()
    if not entries:
        OBSERVATIONS.pop(host, None)
        return None
    port = entries.pop()[0]
    if not entries:
        OBSERVATIONS.pop(host, None)
    return (host, port)


class Endpoint(asyncio.DatagramProtocol):
    def connection_made(self, transport):
        self.transport = transport


class NatCheck(Endpoint):
    def __init__(self, address, port, addresses=(ADDRESSES[0], ADDRESSES[1]), same_socket=False):
        self.address, self.port, self.transport = address, port, None
        self.addresses = tuple(addresses)
        # Opt-in (natcheck_same_socket): answer the "other port" (3, 102) and "other IP" (2) probes
        # from the socket that received them. A home router that filters replies from any other
        # source then no longer makes the game give up its NAT check; real reachability between
        # players is still decided by the probe/hole-punching that follows.
        self.same_socket = same_socket

    def datagram_received(self, data, sender):
        host, port = sender[0], sender[1]
        if not rate_ok(host):
            log.debug("NATCHECK %s:%d <- %s:%d rate limited", self.address, self.port, host, port)
            return
        if len(data) < 16:
            log.debug("NATCHECK %s:%d <- %s:%d short %s", self.address, self.port, host, port, data.hex())
            return
        kind = struct.unpack(">I", data[:4])[0]
        if kind in (1, 4, 5, 101, 103):
            out, origin = self.transport, self.address
        elif kind in (3, 102):
            out, origin = (self.transport if self.same_socket else ALTERNATE.get(self.address)), self.address
        elif kind == 2:
            others = [address for address in self.addresses if address != self.address]
            origin = others[0] if others else self.address
            out = self.transport if self.same_socket else (ALTERNATE.get(origin) if others else None)
        else:
            out = None
        if kind in (1, 2, 3, 4, 5, 101, 102, 103):
            observe(host, port)      # only a real NAT check registers a peer-to-peer endpoint
        log.debug("NATCHECK %s:%d <- %s:%d type=%d%s", self.address, self.port, host, port, kind,
                  "" if out else " (no reply)")
        if out:
            reply = struct.pack(">IIII", kind, port, int(ipaddress.IPv4Address(host)),
                                int(ipaddress.IPv4Address(origin)))
            out.sendto(reply, sender)


async def start(addresses=None, ports=None, same_socket=False):
    """Bind the NAT check sockets; `addresses` and `ports` default to the lab ones."""
    addresses = tuple(addresses or ADDRESSES)
    ports = tuple(ports or PORTS)
    loop = asyncio.get_running_loop()
    transports = []
    for address in addresses:
        try:
            transport, _ = await loop.create_datagram_endpoint(Endpoint, local_addr=(address, 0))
            ALTERNATE[address] = transport
            transports.append(transport)
        except OSError as error:
            log.warning("NAT check %s alternate socket unavailable: %s", address, error)
        for port in ports:
            try:
                transport, _ = await loop.create_datagram_endpoint(
                    lambda a=address, p=port: NatCheck(a, p, addresses, same_socket), local_addr=(address, port))
                transports.append(transport)
            except OSError as error:
                log.warning("NAT check %s:%d unavailable: %s", address, port, error)
    log.info("NAT check on %s ports %s", ", ".join(addresses), ports)
    return transports
