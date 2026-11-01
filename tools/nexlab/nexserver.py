"""NEX lab server for the Splatoon PC port. Stage 3: login + matchmaking (matchmake.py).

Built on NintendoClients (Kinnay, MIT): PRUDP V1, RMC and Kerberos come from the library;
this file only fills the authentication (protocol 10) and secure connection (protocol 11)
answers. Every other protocol the game calls is logged and answered NotImplemented, which
shows the next step.

The auth server needs the player's NEX password to encrypt the ticket. The port writes it to
online_identity.ini next to the exe (one per benchmark run directory); the server looks the
pid up in those files.

usage: py -3.12 nexserver.py [--port 60000] [--log nexserver.log]
Run the game with online_server=127.0.0.1:60000 (settings.ini) or RBREW_ONLINE_SERVER.
"""
import argparse, asyncio, ipaddress, logging, logging.handlers, pathlib, secrets, math, contextlib, threading, time, weakref
import os
import socket
import anyio
from nintendo.nex import authentication, common, kerberos, prudp, rmc, secure, settings
try:
    from . import accounts as account_store, matchmake, natcheck_server, register_server, server_config
except ImportError:
    import accounts as account_store, matchmake, natcheck_server, register_server, server_config

ACCESS_KEY = "6f599f81"            # confirmed by tools/nexlab/prudp_key.py
NEX_VERSION = 30800                # hypothesis: PRUDP minor version 4 means NEX >= 3.5
SECURE_PID = 2                     # the secure server's principal id, as on Nintendo's servers
SERVER_NAME = "branch:origin/project/wup-agmj build:3_8_15_2004_0"

# Denial-of-service limits (S-04). Module level so tests can tighten them.
MAX_CONNECTIONS = 1024             # PRUDP connections in use at once (auth + secure)
MAX_CONNECTIONS_PER_IP = 32        # per source address
MAX_LOGINS_PER_IP = 60             # login/ticket requests per minute and per source address
MAX_TICKETS_PER_PID = 20           # tickets per minute and per player
LOG_MAX_BYTES = 8 * 1024 * 1024    # one rotated log file
LOG_BACKUPS = 3

# A player that disappears without a DISCONNECT (killed process, network cut) leaves a PRUDP
# client that the library never times out: its port stays allocated until a 16-port stream is
# exhausted. Any packet counts as life; after this many seconds the connection is closed and
# the normal cleanup releases sessions and NEX ids. Both values are rescaled by tests.
IDLE_CONNECTION_TIMEOUT = float(os.environ.get("NEXLAB_IDLE_TIMEOUT", "60"))
PRUDP_RESEND_TIMEOUT = 1.5           # seconds between resends of a reliable PRUDP packet
PRUDP_RESEND_LIMIT = 10              # resends before the connection is given up
IDLE_SCAN_SECONDS = float(os.environ.get("NEXLAB_IDLE_SCAN", "5"))

ROOT = pathlib.Path(__file__).resolve().parents[2]
log = logging.getLogger("nexserver")
# Counters exposed by /metrics (register_server.py): no secret, no player identity.
METRICS = {"errors": 0, "started": time.time()}

# --- PRUDP idle watchdog -------------------------------------------------------------
# NintendoClients has no dead-peer detection for UDP. The two patches below only record the
# last packet per client and remember the streams; a janitor (serve()) closes silent ones.
_STREAMS = weakref.WeakSet()
_LAST_SEEN = {}
_ORIGINAL_STREAM_INIT = prudp.PRUDPServerStream.__init__
_ORIGINAL_STREAM_HANDLE = prudp.PRUDPServerStream.handle


def _stream_init(self, *args, **kwargs):
    _ORIGINAL_STREAM_INIT(self, *args, **kwargs)
    _STREAMS.add(self)


async def _stream_handle(self, packet, addr):
    client = self.get_client(packet, addr)
    if client is not None:
        client.nexlab_last_seen = time.monotonic()
    return await _ORIGINAL_STREAM_HANDLE(self, packet, addr)


prudp.PRUDPServerStream.__init__ = _stream_init
prudp.PRUDPServerStream.handle = _stream_handle


async def _idle_janitor():
    """Close PRUDP clients that sent nothing for IDLE_CONNECTION_TIMEOUT seconds."""
    while True:
        await asyncio.sleep(IDLE_SCAN_SECONDS)
        now = time.monotonic()
        for stream in list(_STREAMS):
            for client in list(stream._clients.values()):
                last = getattr(client, "nexlab_last_seen", None)
                if last is None:
                    client.nexlab_last_seen = now      # first scan: one full timeout window
                    continue
                if now - last <= IDLE_CONNECTION_TIMEOUT:
                    continue
                log.warning("closing idle connection %s (no packet for %.0fs)",
                            client.remote_address(), now - last)
                with contextlib.suppress(Exception):
                    await client.cleanup()
        with contextlib.suppress(Exception):
            await matchmake.apply_kicks()


class ConnectionLimiter:
    """Caps PRUDP connections in use, per source address and in total (S-04)."""

    def __init__(self, per_ip=None, total=None, clock=time.monotonic):
        # Resolve the module limits at construction time so tests (and operators) can tighten
        # them before the server starts.
        self.per_ip = int(MAX_CONNECTIONS_PER_IP if per_ip is None else per_ip)
        self.total = int(MAX_CONNECTIONS if total is None else total)
        self.clock = clock
        self._counts = {}
        self._in_use = 0
        self._lock = threading.Lock()
        self._refusal_log = {}

    def enter(self, ip):
        with self._lock:
            if self._in_use >= self.total:
                return False
            if self._counts.get(ip, 0) >= self.per_ip:
                return False
            self._counts[ip] = self._counts.get(ip, 0) + 1
            self._in_use += 1
            return True

    def leave(self, ip):
        with self._lock:
            count = self._counts.get(ip, 0) - 1
            if count > 0:
                self._counts[ip] = count
            else:
                self._counts.pop(ip, None)
            if self._in_use > 0:
                self._in_use -= 1

    def in_use(self):
        with self._lock:
            return self._in_use

    def should_log_refusal(self, ip, interval=10.0):
        """B-05: a refused flood must not become a log flood; one warning per IP per interval."""
        now = self.clock()
        with self._lock:
            last = self._refusal_log.get(ip)
            if last is not None and now - last < interval:
                return False
            self._refusal_log[ip] = now
            if len(self._refusal_log) > 4096:      # bounded, like RateLimiter._events
                self._refusal_log = {k: v for k, v in self._refusal_log.items() if now - v < interval}
            return True


class RateLimiter:
    """At most `limit` events per key inside `window` seconds (S-04)."""

    def __init__(self, limit, window, clock=time.monotonic):
        self.limit, self.window, self.clock = int(limit), float(window), clock
        self._events = {}
        self._next_sweep = 0.0
        self._lock = threading.Lock()

    def allow(self, key):
        now = self.clock()
        with self._lock:
            events = [at for at in self._events.get(key, []) if now - at < self.window]
            if len(events) >= self.limit:
                self._events[key] = events
                return False
            events.append(now)
            self._events[key] = events
            if len(self._events) > 4096 and now >= self._next_sweep:      # keep the table bounded, sweep once a second
                self._next_sweep = now + 1.0
                for stale in [k for k, v in self._events.items() if not v or now - v[-1] >= self.window]:
                    self._events.pop(stale, None)
            while len(self._events) > 8192:       # a flood of fresh keys: forget the oldest, in O(1)
                self._events.pop(next(iter(self._events)))
            return True


IDENTITY_FILES = []
LAST_SEEN_STORE = None      # the account store, when serving in public mode: updated when a secure connection opens
IDENTITY_ROOTS = [ROOT / "Game", ROOT / "build" / "benchmarks"]

def find_password(pid, files=None, roots=None):
    """Newest valid matching identity; unreadable or corrupt candidates are skipped."""
    candidates = list(IDENTITY_FILES if files is None else files)
    for root in (IDENTITY_ROOTS if roots is None else roots):
        candidates.append(pathlib.Path(root) / "online_identity.ini")
        candidates.extend(pathlib.Path(root).glob("*/online_identity.ini"))
    dated = []
    for f in set(map(pathlib.Path, candidates)):
        try:
            dated.append((f.stat().st_mtime_ns, str(f), f))
        except OSError:
            continue
    for _, _, f in sorted(dated, reverse=True):
        try:
            fields = {}
            for line in f.read_text(encoding="utf-8").splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    fields[k.strip()] = v.strip()
            raw = fields.get("pid", "")
            password = fields.get("password", "")
            if not raw or any(c not in "0123456789abcdefABCDEF" for c in raw):
                continue
            number = int(raw, 16)
            if not 0 < number <= 0x7fffffff or len(password) != 16 or not all(c in "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" for c in password):
                continue
            if number == pid:
                return password.encode("ascii")
        except (OSError, UnicodeError, ValueError):
            continue
    return None

def parse_username(username):
    if not isinstance(username, str) or not username or not username.isascii() or not username.isdecimal():
        raise common.RMCError("RendezVous::InvalidUsername")
    if len(username) > 10:          # 0x7fffffff has 10 digits; avoids huge int() work (S-11)
        raise common.RMCError("RendezVous::InvalidUsername")
    pid = int(username)
    if not 0 < pid <= 0x7fffffff:
        raise common.RMCError("RendezVous::InvalidUsername")
    return pid



class AuthServer(authentication.AuthenticationServer):
    def station_for(self, client):
        """Secure station for this login. A server hosted behind a home router is reached by
        remote players on its public address and by players on its own LAN on the private one
        (most routers do not loop a LAN packet back to their own public address)."""
        if self.lan_station and client is not None:
            try:
                if ipaddress.ip_address(client.remote_address()[0]).is_private:
                    return self.lan_station
            except ValueError:
                pass
        return self.secure_station

    def __init__(self, nex_settings, secure_key, secure_station, mode="lab", accounts=None):
        super().__init__()
        self.settings = nex_settings
        self.secure_key = secure_key
        self.secure_station = secure_station
        self.lan_station = None       # set by serve(advertise_lan=...)
        self.mode = mode
        self.accounts = accounts        # public mode only, see accounts.py
        # S-04: tickets and logins are the expensive part of the auth server.
        self.ip_rate = RateLimiter(MAX_LOGINS_PER_IP, 60.0)
        self.pid_rate = RateLimiter(MAX_TICKETS_PER_PID, 60.0)

    async def login_ex(self, client, username, extra_data):
        info = extra_data        # stream.anydata() returns the decoded structure itself
        log.info("LoginEx user=%r token=%r ngs=%s token_type=%s server_version=%s", username,
                 getattr(info, "token", None), getattr(info, "ngs_version", None),
                 getattr(info, "token_type", None), getattr(info, "server_version", None))
        return self.answer(username, client)

    async def login(self, client, username):
        log.info("Login user=%r", username)
        return self.answer(username, client)

    async def request_ticket(self, client, source, target):
        log.info("RequestTicket source=%08x target=%d", source, target)
        if target != SECURE_PID:
            raise common.RMCError("RendezVous::InvalidUsername")
        response = rmc.RMCResponse()
        response.result = common.Result.success()
        response.ticket = self.ticket(source, self.user_key(source, client))
        return response

    def user_key(self, pid, client):
        """Kerberos key of a player: the .ini password in lab, the account store in public."""
        origin = client.remote_address()[0] if client is not None else "unknown"
        if not self.ip_rate.allow(origin):
            log.warning("login from %s refused: too many login attempts per minute", origin)
            raise common.RMCError("RendezVous::InvalidUsername")
        if not self.pid_rate.allow((pid, origin)):     # per player AND address: a stranger cannot lock a pid out
            log.warning("login for pid %08x refused: too many tickets per minute", pid)
            raise common.RMCError("RendezVous::InvalidUsername")
        if self.mode == "public":
            if self.accounts is None:
                raise common.RMCError("RendezVous::InvalidUsername")
            if self.accounts.is_banned(pid=pid, ip=origin):
                log.warning("banned login refused: pid %08x from %s", pid, origin)
                raise common.RMCError("RendezVous::InvalidUsername")
            key = self.accounts.key(pid)
            if key is None:
                # Only unknown pids count against an address: behind a shared address (mobile network,
                # LAN party) other players' sign-ups must not lock out the accounts that already exist.
                if not self.accounts.allow(origin):
                    log.warning("login from %s: too many unknown pids, refused for now", origin)
                    raise common.RMCError("RendezVous::InvalidUsername")
                self.accounts.failure(origin)
                log.warning("unknown pid %08x from %s: register the account first", pid, origin)
                raise common.RMCError("RendezVous::InvalidUsername")
            return key
        password = find_password(pid)
        if password is None:
            log.warning("no online_identity.ini for pid %08x", pid)
            raise common.RMCError("RendezVous::InvalidUsername")
        return kerberos.KeyDerivationOld().derive_key(password, pid)

    def ticket(self, pid, user_key):
        """Kerberos ticket for the secure server, encrypted with the player's key."""
        session_key = secrets.token_bytes(self.settings["kerberos.key_size"])

        internal = kerberos.ServerTicket()
        internal.timestamp = common.DateTime.now()
        internal.source = pid
        internal.session_key = session_key

        ticket = kerberos.ClientTicket()
        ticket.session_key = session_key
        ticket.target = SECURE_PID
        ticket.internal = internal.encrypt(self.secure_key, self.settings)
        return ticket.encrypt(user_key, self.settings)

    def answer(self, username, client):
        pid = parse_username(username)
        response = rmc.RMCResponse()
        response.result = common.Result.success()
        response.pid = pid
        response.ticket = self.ticket(pid, self.user_key(pid, client))
        response.connection_data = authentication.RVConnectionData()
        station = self.station_for(client)
        response.connection_data.main_station = common.StationURL.parse(station)
        response.connection_data.special_protocols = []
        response.connection_data.special_station = common.StationURL.parse("")
        response.connection_data.server_time = common.DateTime.now()
        response.server_name = SERVER_NAME
        log.info("ticket issued for pid %08x -> %s", pid, station)
        return response


class SecureServer(secure.SecureConnectionServer):
    def __init__(self, mode="lab"):
        super().__init__()
        self.mode = mode
        self.connection_id = 0x1000
        self.public_ip = {}          # lab only: pid -> loopback address shown as its public IP

    async def register_ex(self, client, urls, login_data):
        log.info("RegisterEx pid=%08x urls=%s login_data=%s", client.pid(), urls,
                 type(login_data).__name__)
        return self.answer(client, urls)

    async def register(self, client, urls):
        log.info("Register pid=%08x urls=%s", client.pid(), urls)
        return self.answer(client, urls)

    async def replace_url(self, client, url, new):
        """Once its PIA socket exists the game swaps its registered URL for the PIA one."""
        log.info("ReplaceURL pid=%08x %s -> %s", client.pid(), url, new)
        stations = matchmake.STATIONS.setdefault(client.pid(), [])
        # Match on port and type: in public mode the stored address was rewritten from 127.0.0.1
        # to the packet source, so the game's copy no longer matches on address. The list must
        # keep 1 or 2 URLs, or PIA refuses it (101B5E68, "communication error").
        key = lambda u: (u["port"], u["type"])
        for i, old in enumerate(stations):
            if key(old) == key(url):
                updated = new.copy()
                if self.mode == "public" and str(updated["address"]).startswith("127."):
                    updated["address"] = client.remote_address()[0]
                stations[i] = updated
                break
        else:
            log.warning("  ReplaceURL: no registered URL matches, list kept")
            return
        if self.mode == "public":
            # The public URL (type 3) still names the NEX socket the game logged in from; peers
            # must reach its PIA socket. Measured 30/10 (same LAN): the joiner was given the host's
            # NEX port and its PIA probes never got an answer. Assume the router keeps the port,
            # as home routers usually do; the address stays the observed packet source.
            for url_ in stations:
                if url_["type"] == 3:
                    url_["port"] = matchmake.NAT_PORT.get(client.pid(), new["port"])
                    for field in ("natf", "natm", "pmp", "upnp"):
                        if new[field] is not None:
                            url_[field] = new[field]

    async def send_report(self, client, report_id, data):
        # Bound the log line (S-06): the body can be large and may carry player data.
        blob = bytes(data)
        head = blob[:32].hex()
        log.info("SendReport id=%d bytes=%d data=%s%s", report_id, len(blob), head,
                 "..." if len(blob) > 32 else "")
        # The generated handler has no return fields: None produces RMC success.

    def answer(self, client, urls):
        self.connection_id += 1
        public = urls[0].copy() if urls else common.StationURL("prudp")
        host, port = client.remote_address()
        if self.mode != "public" and host.startswith("127."):
            # Same-PC lab: every game shows 127.0.0.1, and a PIA host refuses a newcomer with
            # its own public IP and no private address (02CC40E4 -> deny at 02CC423C, "match
            # full"). Give each player its own loopback IP (127.0.0.2 is nncs2). Real servers
            # send RVCID. In public mode the address is the packet source, never rewritten.
            host = self.public_ip.setdefault(client.pid(), "127.0.0.%d" % (10 + len(self.public_ip)))
            matchmake.FAKE_IPS.add(host)
        public_host = host
        if self.mode == "public" and os.environ.get("NEXLAB_LAN_PUBLIC"):
            # Server hosted behind the same home router as this player: the player reached it
            # on the LAN, but remote peers must use the router's public address
            # (NEXLAB_LAN_PUBLIC), with the router keeping the port as home routers usually do.
            try:
                if ipaddress.ip_address(host).is_private:
                    public_host = os.environ["NEXLAB_LAN_PUBLIC"]
            except ValueError:
                pass
        public["address"], public["port"], public["type"] = public_host, port, 3
        public["RVCID"] = self.connection_id
        # B-11: a login is a new session; drop the PIA endpoint learned from the previous one
        # (a reused PID otherwise kept a stale PEER until its next NAT check, lab mode only)
        # and any state left by a connection that died without closing (chantier 2).
        matchmake.registered(client.pid())
        matchmake.BY_RVCID[self.connection_id] = client.pid()
        matchmake.CLIENTS[client.pid()] = client
        stations = []
        for url in urls:
            copy = url.copy()
            address = str(copy["address"] or "")
            # Public mode: the game registers its local address (127.0.0.1 here). Peers cannot
            # use it, and PIA may pick it over the public URL: store the packet source instead.
            if self.mode == "public" and (not address or address == "0.0.0.0" or address.startswith("127.")):
                log.info("  registered URL %s rewritten to %s:%s (public peer address)",
                         address or "?", host, copy["port"])
                copy["address"] = host
            stations.append(copy)
        matchmake.STATIONS[client.pid()] = stations + [public]
        response = rmc.RMCResponse()
        response.result = common.Result.success()
        response.connection_id = self.connection_id
        response.public_station = public
        return response



class LabRMCClient(rmc.RMCClient):
    async def _handle_request(self, request):
        # Log metadata before dispatch, including unknown methods on known protocols.
        # Public servers only trace this at DEBUG: one line per request is a log-flooding
        # vector for an unauthenticated client (S-06); the lab keeps INFO for protocol work.
        log.log(logging.DEBUG if matchmake.is_public() else logging.INFO,
                "RMC request protocol=%d method=%d params=%d unimplemented_protocol=%s",
                request.protocol, request.method, len(request.body),
                request.protocol not in self._servers)
        await super()._handle_request(request)

@contextlib.contextmanager
def trace_rmc_requests():
    # NintendoClients serve() has no client factory; scope the adapter to this lab run.
    original = rmc.RMCClient
    rmc.RMCClient = LabRMCClient
    try:
        yield
    finally:
        rmc.RMCClient = original



@contextlib.asynccontextmanager
async def _serve_rmc(s, servers, host, port, key=None, limiter=None):
    """rmc.serve with a connection cap (S-04); the rest is the library's own wiring.

    B-05 residual: prudp.serve only hands us a client once the PRUDP handshake has been
    answered, so a refused connection has already cost one handshake. The library exposes
    no pre-handshake hook; the refusal is therefore immediate (no RMC work) and its warning
    is throttled per source, so a SYN flood cannot turn into a log flood (bounded rotation
    still applies).
    """
    async def handle(client):
        origin = client.remote_address()[0]
        if limiter is not None and not limiter.enter(origin):
            if limiter.should_log_refusal(origin):
                log.warning("connection from %s refused: connection limit reached", origin)
            else:
                log.debug("connection from %s refused: limit reached (warning throttled)", origin)
            try:
                await client.close()
            except Exception:
                pass
            return
        # The PRUDP login request already ran, so a secure connection knows its pid. Auth
        # connections have none; the count keeps state until the player's last socket closes.
        pid = client.pid()
        matchmake.connection_opened(pid)
        if pid and LAST_SEEN_STORE is not None:
            LAST_SEEN_STORE.touch(pid)          # a real, authenticated connection (not just a login request)
        try:
            rmc_client = rmc.RMCClient(s, client)
            async with rmc_client:
                await rmc_client.start(servers)
        except Exception:
            METRICS["errors"] += 1
            raise
        finally:
            if limiter is not None:
                limiter.leave(origin)
            matchmake.connection_closed(pid)

    async with prudp.serve(handle, s, host, port, 1, 10, None, key):
        yield


def _disable_udp_connreset():
    """Windows only: stop UDP sockets from reporting 'connection reset' when a peer's port is gone.

    A client that closes its socket (the auth connection ends right after login) makes Windows turn
    the ICMP "port unreachable" of a later reply into an error on the SERVER's socket, and under a
    burst of new players this made it drop other players' handshakes (measured: 7 to 82 logins out of
    100 with it, 100 out of 100 without). Linux has no such behaviour. SIO_UDP_CONNRESET = 0x9800000C."""
    if os.name != "nt" or getattr(socket.socket, "_nexlab_connreset_off", False):
        return
    import ctypes
    original = socket.socket.__init__

    def patched(self, *args, **kwargs):
        original(self, *args, **kwargs)
        if self.type == socket.SOCK_DGRAM:
            returned = ctypes.c_ulong(0)
            ctypes.windll.ws2_32.WSAIoctl(ctypes.c_size_t(self.fileno()), 0x9800000C,
                                          ctypes.byref(ctypes.c_ulong(0)), 4, None, 0,
                                          ctypes.byref(returned), None, None)
    socket.socket.__init__ = patched
    socket.socket._nexlab_connreset_off = True


@contextlib.asynccontextmanager
async def serve(host="127.0.0.1", port=60000, *, advertise=None, secure_port=None, mode="lab",
                natcheck=True, natcheck_addresses=None, natcheck_same_socket=False, identities=None, identity_roots=None,
                accounts=None, registration=None, fill_timeout=None, trace=True,
                random_session_key=False):
    """Auth, secure and matchmaking servers (plus the NAT check on the lab ports).

    `port` is the auth port; the secure server listens on `secure_port` (default `port + 1`).
    `advertise` is the address written into the secure station URL of the login answer, the
    one the game connects back to, so it must stay a real address even when `host` is 0.0.0.0.
    `mode` is "lab" (same-PC shortcuts: fake 127.x addresses, PEER from the NAT check) or
    "public" (no address rewriting: a player is where its packets come from).
    `accounts` (public mode), and `registration` (already built RegistrationServer) are
    optional and closed on exit. `identities`/`identity_roots` replace the identity lookup.
    """
    global IDENTITY_FILES, IDENTITY_ROOTS, LAST_SEEN_STORE
    _disable_udp_connreset()
    LAST_SEEN_STORE = accounts
    if mode not in ("lab", "public"):
        raise ValueError("mode must be 'lab' or 'public', not %r" % (mode,))
    if identities is not None:
        IDENTITY_FILES = list(identities)
    if identity_roots is not None:
        IDENTITY_ROOTS = list(identity_roots)
    secure_port = secure_port or port + 1
    address = advertise or ("127.0.0.1" if host in ("", "0.0.0.0", "::") else host)
    matchmake.configure(mode, fill_timeout, random_session_key)
    s = settings.default()
    s.configure(ACCESS_KEY, NEX_VERSION)
    # The library gives up on a reliable packet after 2 resends 1 s apart: three lost
    # datagrams in a row (seen on a home Internet link, 31/10) dropped a player mid-lobby.
    # 1.5 s x 10 resends tolerates ~16 s of loss before the connection is closed.
    s["prudp.resend_timeout"] = PRUDP_RESEND_TIMEOUT
    s["prudp.resend_limit"] = PRUDP_RESEND_LIMIT
    # prudp.supported_functions stays 0: the game drops a SYN/ACK that grants
    # function bit 0 (PRUDP stream 02BB6EEC -> disconnect reason 8).
    secure_key = secrets.token_bytes(16)
    station = f"prudps:/address={address};port={secure_port};CID=1;PID={SECURE_PID};sid=1;stream=10;type=2"
    transports = []
    limiter = ConnectionLimiter()
    if registration is not None:
        registration.start()
    try:
        with trace_rmc_requests() if trace else contextlib.nullcontext():
            auth = AuthServer(s, secure_key, station, mode, accounts)
            # NEXLAB_ADVERTISE_LAN: this server's LAN address, given to players on private
            # addresses when it is hosted behind a home router (see AuthServer.station_for).
            if os.environ.get("NEXLAB_ADVERTISE_LAN"):
                auth.lan_station = station.replace(f"address={address};", f"address={os.environ['NEXLAB_ADVERTISE_LAN']};")
            async with _serve_rmc(s, [auth], host, port,
                                  limiter=limiter):
                async with _serve_rmc(s, [SecureServer(mode)] + matchmake.servers(), host, secure_port,
                                      key=secure_key, limiter=limiter):
                    log.info("mode=%s auth %s:%d, secure %s:%d, advertise %s", mode, host, port,
                             host, secure_port, address)
                    if natcheck:
                        transports = await natcheck_server.start(natcheck_addresses, same_socket=natcheck_same_socket)
                    janitor = asyncio.create_task(_idle_janitor())
                    try:
                        yield
                    finally:
                        janitor.cancel()
                        with contextlib.suppress(asyncio.CancelledError):
                            await janitor
                        for transport in transports:
                            transport.close()
    finally:
        if registration is not None:
            registration.stop()


async def run(a, config):
    accounts = None
    registration = None
    if config.mode == "public":
        accounts = account_store.Accounts(config.accounts_db)
        if config.registration != "closed":
            registration = register_server.RegistrationServer(
                accounts, "127.0.0.1" if config.registration_proxy else
                "0.0.0.0" if config.listen in ("", "0.0.0.0", "::") else config.listen,
                config.registration_port,
                config.registration_token if config.registration == "token" else None,
                behind_proxy=config.registration_proxy,
                min_client_version=config.min_client_version,
                admin_token=config.admin_token)
        log.info("accounts: %d in %s (registration %s, port %d)", accounts.count(),
                 config.accounts_db, config.registration, config.registration_port)
    try:
        async with serve(config.listen, config.auth_port, advertise=config.advertise,
                         secure_port=config.secure_port, mode=config.mode,
                         natcheck=config.natcheck, natcheck_addresses=config.natcheck_addresses,
                         natcheck_same_socket=config.natcheck_same_socket,
                         identities=config.identities, identity_roots=config.identity_roots,
                         accounts=accounts, registration=registration,
                         fill_timeout=config.fill_timeout,
                         random_session_key=config.random_session_key):
            log.info("%s", config.describe())
            if a.seconds:
                await anyio.sleep(a.seconds)
            else:
                await anyio.sleep_forever()
    finally:
        # Clean shutdown (Ctrl+C / SIGTERM / --seconds): close sessions, timers and the DB.
        matchmake.shutdown()
        if accounts is not None:
            accounts.close()
        log.info("server stopped")


def parse_config(a):
    """Config file, then command-line overrides (None means 'not given')."""
    config = server_config.ServerConfig.load(a.configfile) if a.configfile else server_config.ServerConfig()
    if a.public and a.mode and a.mode != "public":
        raise server_config.ConfigError("--public contradicts --mode %s" % a.mode)
    secure_port = a.secure_port
    if secure_port is None and a.port is not None:
        secure_port = a.port + 1      # --port alone moves the whole pair, as in the lab launcher
    return config.updated(
        mode="public" if a.public else a.mode,
        listen=a.listen,
        advertise=a.advertise,
        auth_port=a.port,
        secure_port=secure_port,
        natcheck=a.natcheck,
        natcheck_addresses=a.natcheck_address,
        identities=a.identity or None,
        identity_roots=a.identity_root,
        accounts_db=a.accounts_db,
        registration=a.registration,
        registration_port=a.registration_port,
        registration_token=a.registration_token,
        fill_timeout=a.fill_timeout,
    )


def main():
    ap = argparse.ArgumentParser(description="NEX lab/public server for the Splatoon PC port")
    ap.add_argument("--config", dest="configfile", type=pathlib.Path,
                    help="JSON server config (see server.example.json); command-line switches win")
    ap.add_argument("--mode", choices=server_config.MODES,
                    help="lab (same PC, default) or public (real addresses)")
    ap.add_argument("--public", action="store_true", help="shortcut for --mode public")
    ap.add_argument("--listen", help="bind address, 0.0.0.0 for every interface (default 127.0.0.1)")
    ap.add_argument("--advertise", help="address players connect back to (default: --listen)")
    ap.add_argument("--port", type=int, help="auth port (default 60000)")
    ap.add_argument("--secure-port", type=int, help="secure port (default: auth port + 1)")
    ap.add_argument("--natcheck", action=argparse.BooleanOptionalAction, default=None,
                    help="NAT check server (default: on in lab, off in public)")
    ap.add_argument("--natcheck-address", action="append", dest="natcheck_address",
                    help="address to answer the NAT check on (repeatable)")
    ap.add_argument("--accounts-db", help="public mode: SQLite account file (default nexlab-accounts.sqlite3)")
    ap.add_argument("--registration", choices=server_config.REGISTRATIONS,
                    help="public mode: open | token | closed (default open)")
    ap.add_argument("--registration-token", help="secret required by the registration endpoint in token mode")
    ap.add_argument("--registration-port", type=int,
                    help="HTTP registration port (default: auth port + 2)")
    ap.add_argument("--fill-timeout", type=float,
                    help="public mode: seconds to fill a classic match before starting it (default 20)")
    ap.add_argument("--log", default="nexserver.log")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = forever)")
    ap.add_argument("--debug", action="store_true", help="also log every PRUDP packet")
    ap.add_argument("--identity", action="append", default=[], type=pathlib.Path, help="identity file (repeatable)")
    ap.add_argument("--identity-root", action="append", type=pathlib.Path, help="search directory, replacing Game/build defaults (repeatable)")
    a = ap.parse_args()
    try:
        config = parse_config(a)
    except server_config.ConfigError as error:
        ap.error(str(error))
    if not math.isfinite(a.seconds) or a.seconds < 0: ap.error("seconds must be finite and nonnegative")
    pathlib.Path(a.log).parent.mkdir(parents=True, exist_ok=True)
    # Bounded log files (S-06): an exposed server must not fill its disk with traces.
    logging.basicConfig(level=logging.DEBUG if a.debug else logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s",
                        handlers=[logging.handlers.RotatingFileHandler(
                                      a.log, "w", maxBytes=LOG_MAX_BYTES, backupCount=LOG_BACKUPS,
                                      encoding="utf-8"),
                                  logging.StreamHandler()])
    try:
        anyio.run(run, a, config)
    except KeyboardInterrupt:
        log.info("server stopped")


if __name__ == "__main__":
    main()



