"""Security regression tests.

Each class covers findings from the audit: S-02, S-03, S-04, S-05, S-06, S-07, S-08, S-09,
S-10, S-11, S-12, plus the S-01 URL guard of the client tool. Run from the repository root:
    py -3.12 -m unittest tools.tests.test_security -v
"""
import asyncio
import json
import pathlib
import secrets
import socket
import threading
import time
import unittest
import urllib.error
import urllib.request
from types import SimpleNamespace
from unittest import mock

from nintendo.nex import authentication, common, matchmaking, rmc, settings
from tools.nexlab import (accounts as account_store, matchmake, natcheck_server,
                          nexserver, register, register_server)

DIRECTORY = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"


def free_udp_pair():
    for _ in range(100):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        if port > 65533:
            continue
        held = []
        try:
            for candidate in (port, port + 1):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                held.append(sock)
                sock.bind(("127.0.0.1", candidate))
        except OSError:
            continue
        finally:
            for sock in held:
                sock.close()
        return port
    raise RuntimeError("no free UDP port pair")


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def cleanup_db(accounts):
    accounts.close()
    for suffix in ("", "-wal", "-shm"):
        pathlib.Path(str(accounts.path) + suffix).unlink(missing_ok=True)


def find_exception(error, kind):
    """anyio wraps a refused handshake in an ExceptionGroup; find the real cause."""
    if isinstance(error, kind):
        return error
    for inner in getattr(error, "exceptions", []):
        found = find_exception(inner, kind)
        if found is not None:
            return found
    return None


class FakeClient:
    def __init__(self, pid, ip="127.0.0.1"):
        self._pid, self._ip = pid, ip

    def pid(self):
        return self._pid

    def remote_address(self):
        return (self._ip, 5000)


def new_session(max_participants=8, game_mode=1):
    session = matchmaking.MatchmakeSession()
    session.game_mode = game_mode
    session.min_participants = 2
    session.max_participants = max_participants
    return session


class Base(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.saved = (matchmake.FILL_TIMEOUT, matchmake.MAX_SESSIONS,
                      matchmake.SESSION_TTL, matchmake.MAX_JOIN_MESSAGE)
        matchmake.configure("lab")
        matchmake.SESSIONS = matchmake.Sessions()
        matchmake.PEER.clear()
        matchmake.STATIONS.clear()
        natcheck_server.OBSERVATIONS.clear()

    async def asyncTearDown(self):
        (matchmake.FILL_TIMEOUT, matchmake.MAX_SESSIONS,
         matchmake.SESSION_TTL, matchmake.MAX_JOIN_MESSAGE) = self.saved
        matchmake.configure("lab")
        matchmake.SESSIONS = matchmake.Sessions()
        matchmake.PEER.clear()
        matchmake.STATIONS.clear()
        natcheck_server.OBSERVATIONS.clear()


class NatObservationTests(Base):
    """S-02: the peer-to-peer endpoint is per source address, never a global."""

    async def test_report_nat_properties_keeps_each_players_own_endpoint(self):
        natcheck_server.observe("127.0.0.2", 1111)
        natcheck_server.observe("127.0.0.3", 2222)
        server = matchmake.NATTraversalServer()
        await server.report_nat_properties(FakeClient(0xA1, "127.0.0.2"), 1, 1, 10)
        await server.report_nat_properties(FakeClient(0xA2, "127.0.0.3"), 1, 1, 10)
        self.assertEqual(matchmake.PEER[0xA1], ("127.0.0.2", 1111))
        self.assertEqual(matchmake.PEER[0xA2], ("127.0.0.3", 2222))

    async def test_public_mode_never_uses_peer(self):
        matchmake.configure("public")
        natcheck_server.observe("127.0.0.2", 3333)
        server = matchmake.NATTraversalServer()
        await server.report_nat_properties(FakeClient(0xA3, "127.0.0.2"), 1, 1, 10)
        self.assertNotIn(0xA3, matchmake.PEER)
        session = matchmake.SESSIONS.create(new_session(), 0xA3)
        matchmake.SESSIONS.join(session, 0xA3, 1)
        matchmake.STATIONS[0xA3] = [common.StationURL("prudps", address="203.0.113.9", port=9000)]
        urls = await matchmake.MatchMakingServer().get_session_urls(
            FakeClient(0xA3, "203.0.113.9"), session.id)
        self.assertEqual(urls[0]["address"], "203.0.113.9")


class MatchmakingAuthorizationTests(Base):
    """S-03: only the host changes or closes its session; only members read its URLs."""

    async def test_close_and_modify_by_a_stranger_are_refused(self):
        host, other = 0xB1, 0xB2
        session = matchmake.SESSIONS.create(new_session(), host)
        matchmake.SESSIONS.join(session, host, 1)
        extension = matchmake.MatchmakeExtensionServer()
        await extension.close_participation(FakeClient(other), session.id)
        self.assertTrue(matchmake.SESSIONS.get(session.id).open_participation)
        await extension.modify_current_game_attribute(FakeClient(other), session.id, 0, 99)
        self.assertEqual(matchmake.SESSIONS.get(session.id).attribs[0], 0)
        await extension.update_progress_score(FakeClient(other), session.id, 77)
        self.assertEqual(matchmake.SESSIONS.get(session.id).progress_score, 100)
        await extension.close_participation(FakeClient(host), session.id)
        self.assertFalse(matchmake.SESSIONS.get(session.id).open_participation)

    async def test_session_urls_are_only_for_members(self):
        host = 0xB3
        session = matchmake.SESSIONS.create(new_session(), host)
        matchmake.SESSIONS.join(session, host, 1)
        matchmake.STATIONS[host] = [common.StationURL("prudps", address="203.0.113.9", port=9000)]
        server = matchmake.MatchMakingServer()
        with self.assertRaises(common.RMCError):
            await server.get_session_urls(FakeClient(0xB4), session.id)
        urls = await server.get_session_urls(FakeClient(host), session.id)
        self.assertEqual(urls[0]["port"], 9000)


class SessionLimitTests(Base):
    """S-05: a bounded number of sessions, closed ones expire; S-12: claimed count bounded."""

    async def test_session_cap(self):
        matchmake.MAX_SESSIONS = 2
        for index in range(2):
            matchmake.SESSIONS.create(new_session(), 0xC0 + index)
        with self.assertRaises(common.RMCError):
            matchmake.SESSIONS.create(new_session(), 0xCF)
        self.assertEqual(len(matchmake.SESSIONS.by_gid), 2)

    async def test_closed_sessions_expire(self):
        matchmake.SESSION_TTL = 0.05
        session = matchmake.SESSIONS.create(new_session(), 0xC5)
        matchmake.SESSIONS.join(session, 0xC5, 1)
        matchmake.SESSIONS.close(session, "test")
        self.assertIn(session.id, matchmake.SESSIONS.by_gid)
        await asyncio.sleep(0.08)
        matchmake.SESSIONS.purge()
        self.assertNotIn(session.id, matchmake.SESSIONS.by_gid)

    async def test_join_count_is_clamped(self):
        session = matchmake.SESSIONS.create(new_session(max_participants=8), 0xC6)
        matchmake.SESSIONS.join(session, 0xC6, 60000)
        self.assertEqual(session.num_participants, 8)


class LimiterTests(unittest.TestCase):
    """S-04: connection and rate limiters; B-05: refusal warnings are throttled."""

    def test_connection_limits(self):
        limiter = nexserver.ConnectionLimiter(per_ip=2, total=3)
        self.assertTrue(limiter.enter("a"))
        self.assertTrue(limiter.enter("a"))
        self.assertFalse(limiter.enter("a"))
        self.assertTrue(limiter.enter("b"))
        self.assertFalse(limiter.enter("c"))
        limiter.leave("a")
        self.assertTrue(limiter.enter("c"))
        self.assertEqual(limiter.in_use(), 3)

    def test_refusal_logging_is_throttled_per_ip(self):
        now = [1000.0]
        limiter = nexserver.ConnectionLimiter(per_ip=1, total=4, clock=lambda: now[0])
        self.assertTrue(limiter.should_log_refusal("ip"))
        self.assertFalse(limiter.should_log_refusal("ip"))
        self.assertTrue(limiter.should_log_refusal("other"))
        now[0] += 11.0
        self.assertTrue(limiter.should_log_refusal("ip"))

    def test_rate_limiter_window(self):
        now = [1000.0]
        limiter = nexserver.RateLimiter(2, 60.0, clock=lambda: now[0])
        self.assertTrue(limiter.allow("ip"))
        self.assertTrue(limiter.allow("ip"))
        self.assertFalse(limiter.allow("ip"))
        self.assertTrue(limiter.allow("other"))
        now[0] += 61.0
        self.assertTrue(limiter.allow("ip"))


class InputValidationTests(unittest.TestCase):
    """S-11 (username) and S-10 (registration token)."""

    def test_huge_username_is_refused(self):
        with self.assertRaises(common.RMCError):
            nexserver.parse_username("1" * 5000)
        with self.assertRaises(common.RMCError):
            nexserver.parse_username("")

    def test_non_ascii_registration_token_does_not_crash(self):
        self.assertTrue(register_server.token_ok("sécret", {}, {"token": "sécret"}))
        self.assertFalse(register_server.token_ok("sécret", {}, {"token": "autre"}))
        self.assertTrue(register_server.token_ok(None, {}, {}))


class LoggingTests(Base):
    """S-06: bounded logs."""

    async def test_send_report_is_truncated(self):
        payload = bytes(range(100))
        with self.assertLogs("nexserver", level="INFO") as logs:
            await nexserver.SecureServer().send_report(None, 7, payload)
        line = " ".join(logs.output)
        self.assertIn("bytes=100", line)
        self.assertIn("...", line)
        self.assertNotIn(payload.hex(), line)

    async def test_natcheck_packets_are_not_logged_at_info(self):
        loop = asyncio.get_running_loop()
        transport, protocol = await loop.create_datagram_endpoint(
            lambda: natcheck_server.NatCheck("127.0.0.1", 12025, ("127.0.0.1", "127.0.0.2")),
            local_addr=("127.0.0.1", 12025))
        self.addCleanup(transport.close)
        alternate, _ = await loop.create_datagram_endpoint(
            natcheck_server.Endpoint, local_addr=("127.0.0.1", 0))
        self.addCleanup(alternate.close)
        natcheck_server.ALTERNATE["127.0.0.1"] = alternate
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.bind(("127.0.0.1", 0))
        client.setblocking(False)
        try:
            with self.assertNoLogs("nexserver", level="INFO"):
                client.sendto(b"\x00\x00\x00\x65" + bytes(12), ("127.0.0.1", 12025))
                await asyncio.sleep(0.2)
        finally:
            client.close()


class AccountsPermissionTests(unittest.TestCase):
    """S-08: the account file is restricted as soon as it exists."""

    def test_chmod_is_applied(self):
        path = DIRECTORY / f"perm-{secrets.token_hex(4)}.sqlite3"
        path.parent.mkdir(parents=True, exist_ok=True)
        with mock.patch("os.chmod") as chmod:
            accounts = account_store.Accounts(path)
            try:
                self.assertTrue(any(call.args[1] == 0o600 for call in chmod.call_args_list))
            finally:
                cleanup_db(accounts)


class HttpHardeningTests(unittest.TestCase):
    """S-09: a second connection is refused when all request slots are taken."""

    def test_extra_connection_is_closed(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        accounts = account_store.Accounts(DIRECTORY / f"http-{secrets.token_hex(4)}.sqlite3")
        server = SimpleNamespace(accounts=accounts, token=None)
        httpd = register_server.BoundedHTTPServer(("127.0.0.1", 0),
                                                  register_server._make_handler(server),
                                                  max_threads=1)
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            port = httpd.server_address[1]
            idle = socket.create_connection(("127.0.0.1", port), timeout=2)
            try:
                time.sleep(0.3)                    # occupies the single request slot
                with self.assertRaises(Exception):
                    urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2).read()
            finally:
                idle.close()
        finally:
            httpd.shutdown()
            httpd.server_close()
            cleanup_db(accounts)


class RegistrationLimitTests(unittest.TestCase):
    """S-07: successful registrations count towards the per-IP window."""

    def test_successes_are_limited(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        accounts = account_store.Accounts(DIRECTORY / f"limit-{secrets.token_hex(4)}.sqlite3",
                                          max_attempts=2)
        server = register_server.RegistrationServer(accounts, "127.0.0.1", free_tcp_port())
        server.start()
        try:
            def post(pid):
                body = json.dumps({"pid": f"0x{pid:08x}", "password": f"{pid:08x}" * 2}).encode()
                request = urllib.request.Request(f"http://127.0.0.1:{server.port}/register",
                                                 data=body,
                                                 headers={"Content-Type": "application/json"})
                try:
                    with urllib.request.urlopen(request, timeout=5) as response:
                        return response.status
                except urllib.error.HTTPError as error:
                    return error.code
            self.assertEqual(post(0x100000D1), 200)
            self.assertEqual(post(0x100000D2), 200)
            self.assertEqual(post(0x100000D3), 429)
        finally:
            server.stop()
            cleanup_db(accounts)


class ServerWiringTests(unittest.IsolatedAsyncioTestCase):
    """S-04: the limits are actually wired into the served connections."""

    IDENTITY = "pid = 00001001\npassword = 0000100100001001\n"

    async def asyncSetUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.identity = DIRECTORY / f"id-{secrets.token_hex(4)}.ini"
        self.identity.write_text(self.IDENTITY, encoding="utf-8")
        self.addCleanup(self.identity.unlink, missing_ok=True)
        self.saved = (nexserver.MAX_LOGINS_PER_IP, nexserver.MAX_CONNECTIONS_PER_IP,
                      nexserver.MAX_CONNECTIONS)

    async def asyncTearDown(self):
        (nexserver.MAX_LOGINS_PER_IP, nexserver.MAX_CONNECTIONS_PER_IP,
         nexserver.MAX_CONNECTIONS) = self.saved

    def neap(self):
        config = settings.default()
        config.configure(nexserver.ACCESS_KEY, nexserver.NEX_VERSION)
        return config

    async def login(self, port):
        async with rmc.connect(self.neap(), "127.0.0.1", port) as client:
            await authentication.AuthenticationClient(client).login("4097")

    async def test_login_flood_is_refused(self):
        nexserver.MAX_LOGINS_PER_IP = 2
        port = free_udp_pair()
        async with nexserver.serve("127.0.0.1", port, identities=[self.identity],
                                   identity_roots=[], natcheck=False, trace=False):
            for _ in range(2):
                async with rmc.connect(self.neap(), "127.0.0.1", port) as client:
                    await authentication.AuthenticationClient(client).login("4097")
            async with rmc.connect(self.neap(), "127.0.0.1", port) as client:
                with self.assertRaises(common.RMCError):
                    await authentication.AuthenticationClient(client).login("4097")

    async def test_connection_cap_is_enforced(self):
        nexserver.MAX_CONNECTIONS_PER_IP = 1
        nexserver.MAX_CONNECTIONS = 4
        port = free_udp_pair()
        async with nexserver.serve("127.0.0.1", port, identities=[self.identity],
                                   identity_roots=[], natcheck=False, trace=False):
            async with rmc.connect(self.neap(), "127.0.0.1", port):
                # The PRUDP handshake happens before the handler, so the refused connection
                # is closed right after it: its RMC client must not answer any more.
                async with rmc.connect(self.neap(), "127.0.0.1", port) as second:
                    await asyncio.sleep(0.3)
                    with self.assertRaises(RuntimeError):
                        async with asyncio.timeout(2):
                            await second.request(10, 1, b"")


class SessionReuseTests(unittest.TestCase):
    """B-11: a new login drops the PIA endpoint learned by the previous session."""

    def test_login_clears_stale_peer(self):
        server = nexserver.SecureServer("lab")

        class Client:
            def pid(self):
                return 0x1234

            def remote_address(self):
                return ("127.0.0.1", 5000)

        matchmake.configure("lab")
        matchmake.PEER[0x1234] = ("127.0.0.1", 61000)
        try:
            answer = server.answer(Client(), [])
            self.assertNotIn(0x1234, matchmake.PEER)
            self.assertEqual(answer.public_station["type"], 3)
        finally:
            matchmake.PEER.clear()
            matchmake.STATIONS.clear()
            matchmake.BY_RVCID.clear()


class RegisterClientTests(unittest.TestCase):
    """S-01: the client refuses to send the password over plain HTTP to a remote host."""

    def test_url_guard(self):
        self.assertTrue(register.check_server_url("https://example.net:60002"))
        self.assertTrue(register.check_server_url("http://127.0.0.1:60002"))
        self.assertTrue(register.check_server_url("http://[::1]:60002"))
        self.assertTrue(register.check_server_url("http://localhost:60002"))
        self.assertTrue(register.check_server_url("http://127.1:60002"))   # B-12, inet_aton form
        with self.assertRaises(ValueError):
            register.check_server_url("http://example.net:60002")
        self.assertTrue(register.check_server_url("http://example.net:60002", allow_http=True))
        with self.assertRaises(ValueError):
            register.check_server_url("ftp://example.net")

    def test_remote_http_is_refused_before_any_request(self):
        with mock.patch("urllib.request.urlopen") as opener:
            code = register.main(["--server", "http://example.net:60002",
                                  "--identity", str(DIRECTORY / "missing.ini")])
        self.assertEqual(code, 1)
        opener.assert_not_called()


if __name__ == "__main__":
    unittest.main()
