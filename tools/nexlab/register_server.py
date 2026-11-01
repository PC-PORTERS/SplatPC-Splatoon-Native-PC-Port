"""HTTP registration endpoint for the public NEX server.

The game cannot register by itself: its LoginEx token is the opaque string
'rbrew1:<pid>:<game server>' and the NEX password never leaves the console. So a player
registers once before playing, with the small client tool, and the server stores only the
derived Kerberos key (accounts.py).

    POST /register  {"pid": "5eea2491", "password": "16 alphanumeric characters"}
                    -> 200 {"status": "created"|"exists", "pid": "5eea2491"}
                    -> 400 invalid identity, 401 bad token, 409 pid taken, 429 too many tries
    GET  /health    -> 200 {"ok": true, "accounts": N}

Plain HTTP on purpose: the endpoint binds next to the NEX ports and is meant to sit behind a
TLS reverse proxy on a real deployment (documented in docs/SERVEUR.md). The body is never
logged, and the response never echoes the password or the key.
"""
import hmac
import ipaddress
import json
import logging
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from . import accounts as accounts_module
    from . import social as social_module
except ImportError:                          # run as a script
    import accounts as accounts_module
    import social as social_module

log = logging.getLogger("nexserver")

MAX_BODY = 4096
MAX_THREADS = 8            # concurrent registration requests (S-09)
REQUEST_TIMEOUT = 10.0     # seconds a connection may take to send its request (S-09)


class BoundedHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer with a connection timeout and a hard thread cap (S-09)."""

    daemon_threads = True

    def __init__(self, *args, max_threads=MAX_THREADS, **kwargs):
        self._slots = threading.BoundedSemaphore(max_threads)
        super().__init__(*args, **kwargs)

    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            log.warning("registration: %d requests already in flight, connection refused",
                        MAX_THREADS)
            self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()


class RegistrationServer:
    def __init__(self, accounts, host="127.0.0.1", port=60002, token=None, behind_proxy=False,
                 min_client_version=None):
        self.accounts = accounts
        self.social = social_module.Social(accounts) if accounts is not None else None
        self.min_client_version = min_client_version
        self.token = token
        self.behind_proxy = behind_proxy
        self.host = host
        self.port = port
        self._httpd = None
        self._thread = None

    def start(self):
        handler = _make_handler(self)
        self._httpd = BoundedHTTPServer((self.host, self.port), handler)
        self._thread = threading.Thread(target=self._httpd.serve_forever,
                                        name="nexlab-registration", daemon=True)
        self._thread.start()
        log.info("registration endpoint http://%s:%d/register", self.host, self.port)
        return self

    def stop(self):
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None


def client_ip(server, peer, headers):
    """The player's address. Behind the local proxy (connection from the loopback only), the last
    X-Forwarded-For entry, which the proxy itself appends; a forged header from outside cannot
    reach this endpoint, bound to 127.0.0.1 in that mode."""
    if not getattr(server, "behind_proxy", False) or peer not in ("127.0.0.1", "::1"):
        return peer
    forwarded = [part.strip() for part in headers.get("X-Forwarded-For", "").split(",") if part.strip()]
    try:
        return str(ipaddress.ip_address(forwarded[-1])) if forwarded else peer
    except ValueError:
        return peer


def token_ok(server_token, headers, payload):
    """Constant-time token check; bytes, so a non-ASCII token cannot raise (S-10)."""
    if server_token is None:
        return True
    given = (headers.get("Authorization", "").removeprefix("Bearer ").strip()
             or str(payload.get("token", "")))
    return hmac.compare_digest(given.encode("utf-8", "surrogatepass"),
                               server_token.encode("utf-8", "surrogatepass"))


def _make_handler(server):
    class Handler(BaseHTTPRequestHandler):
        server_version = "nexlab-registration/1"
        timeout = REQUEST_TIMEOUT        # a slow client must not hold its thread forever (S-09)

        def log_message(self, fmt, *args):          # never log query strings or bodies
            log.debug("registration %s - %s", self.address_string(), fmt % args)

        def _reply(self, status, payload):
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _token_ok(self, payload):
            return token_ok(server.token, self.headers, payload)

        def _admin_ok(self):
            """Operator routes need the shared token; without one they do not exist."""
            return server.token is not None and token_ok(server.token, self.headers, {})

        def do_GET(self):
            parts = urllib.parse.urlsplit(self.path)
            path = parts.path
            if path == "/health":
                payload = {"ok": True}               # no account count (S-18)
                if server.min_client_version:
                    payload["min_client_version"] = server.min_client_version
                self._reply(200, payload)
                return
            if path == "/metrics":
                # Public and secret-free: counts only, never pids or addresses.
                try:
                    from . import matchmake, nexserver
                except ImportError:                  # run as a script
                    import matchmake, nexserver
                payload = {"uptime": int(time.time() - nexserver.METRICS["started"]),
                           "errors": nexserver.METRICS["errors"], **matchmake.stats()}
                self._reply(200, payload)
                return
            if path in ("/admin/sessions", "/admin/kick"):
                if not self._admin_ok():
                    self._reply(401, {"error": "admin token required"})
                    return
                try:
                    from . import matchmake
                except ImportError:
                    import matchmake
                if path == "/admin/sessions":
                    sessions = []
                    for gid, session in matchmake.SESSIONS.by_gid.items():
                        sessions.append({"id": gid, "host": f"{session.host:08x}",
                                         "members": len(matchmake.SESSIONS.members.get(gid, [])),
                                         "closed": gid in matchmake.SESSIONS.closed_at})
                    self._reply(200, {"sessions": sessions,
                                      "players": [f"{pid:08x}" for pid in matchmake.active_players()]})
                    return
                query = urllib.parse.parse_qs(parts.query)
                try:
                    pid = accounts_module.parse_pid(query.get("pid", [""])[0])
                except Exception:
                    self._reply(400, {"error": "invalid pid"})
                    return
                matchmake.request_kick(pid)
                self._reply(200, {"kick": f"{pid:08x}"})
                return
            self._reply(404, {"error": "not found"})

        def _social(self, path):
            """Friends by pseudo (see social.py): a short JSON request, a short JSON answer."""
            social = server.social
            ip = client_ip(server, self.client_address[0], self.headers)
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if social is None or not 0 < length <= MAX_BODY:
                self._reply(400, {"error": "invalid body"})
                return
            try:
                payload = json.loads(self.rfile.read(length).decode("utf-8"))
                if not isinstance(payload, dict):
                    raise ValueError
            except (UnicodeDecodeError, ValueError):
                self._reply(400, {"error": "invalid JSON"})
                return
            try:
                if path in ("/social/session", "/social/pseudo"):
                    if not social.allow_auth(ip):
                        raise social_module.SocialError(429, "too many attempts, try again later")
                    pid = accounts_module.parse_pid(payload.get("pid"))
                    if server.accounts.is_banned(pid=pid, ip=ip):
                        raise social_module.SocialError(403, "this player or address is banned")
                    password = str(payload.get("password", ""))
                    if path == "/social/session":
                        self._reply(200, {"token": social.open_session(pid, password)})
                    else:
                        self._reply(200, {"pseudo": social.claim_pseudo(pid, password, payload.get("pseudo"))})
                elif path == "/social/lookup":
                    self._reply(200, social.lookup(payload.get("token"), payload.get("pseudo"), ip))
                elif path == "/social/presence":
                    self._reply(200, {"friends": social.heartbeat(payload.get("token"), payload.get("mode"), payload.get("friends"))})
                else:
                    self._reply(404, {"error": "not found"})
            except accounts_module.InvalidIdentity:
                self._reply(401, {"error": "unknown player or wrong password"})
            except social_module.SocialError as error:
                self._reply(error.status, {"error": error.message})

        def do_POST(self):
            route = self.path.split("?")[0]
            if route.startswith("/social/"):
                self._social(route)
                return
            if route != "/register":
                self._reply(404, {"error": "not found"})
                return
            ip = client_ip(server, self.client_address[0], self.headers)
            if not server.accounts.allow(ip):
                self._reply(429, {"error": "too many attempts, try again later"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if not 0 < length <= MAX_BODY:
                self._reply(400, {"error": "invalid body"})
                return
            try:
                payload = json.loads(self.rfile.read(length).decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                server.accounts.attempt(ip)
                self._reply(400, {"error": "invalid JSON"})
                return
            if not isinstance(payload, dict):
                server.accounts.attempt(ip)
                self._reply(400, {"error": "invalid JSON"})
                return
            if not self._token_ok(payload):
                server.accounts.attempt(ip)
                self._reply(401, {"error": "invalid registration token"})
                return
            try:
                banned = server.accounts.is_banned(
                    pid=accounts_module.parse_pid(payload.get("pid")), ip=ip)
            except accounts_module.InvalidIdentity:
                banned = server.accounts.is_banned(ip=ip)
            if banned:
                server.accounts.attempt(ip)
                self._reply(403, {"error": "this player or address is banned"})
                return
            try:
                if accounts_module.parse_pid(payload.get("pid")) < 0x10000000:      # the port never makes smaller ones
                    raise accounts_module.InvalidIdentity("pid out of the range the game generates")
                status = server.accounts.register(payload.get("pid"), str(payload.get("password", "")))
            except accounts_module.InvalidIdentity as error:
                server.accounts.attempt(ip)
                self._reply(400, {"error": str(error)})
                return
            except accounts_module.AlreadyRegistered as error:
                server.accounts.attempt(ip)
                self._reply(409, {"error": str(error)})
                return
            server.accounts.attempt(ip)          # successes are bounded too (S-07)
            self._reply(200, {"status": status, "pid": f"{accounts_module.parse_pid(payload.get('pid')):08x}"})

    return Handler
