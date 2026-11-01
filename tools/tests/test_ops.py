"""Operator features: bans, backup/restore, /metrics and admin routes (chantier 4)."""
import asyncio
import json
import pathlib
import secrets
import time
import unittest
import urllib.error
import urllib.request

from nintendo.nex import common, settings
from tools.nexlab import accounts as account_store, matchmake, nexserver, register_server

DIRECTORY = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"
PASSWORD = "0000100100001001"


def free_tcp_port():
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def reset_matchmaking():
    """Drop every shared matchmaking structure (sessions, players, timers, kicks)."""
    import contextlib
    with contextlib.suppress(Exception):
        matchmake.shutdown()
    matchmake.SESSIONS = matchmake.Sessions()
    matchmake.configure("lab")
    matchmake.KICKS.clear()


class AccountBanTests(unittest.TestCase):
    def setUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.path = DIRECTORY / f"bans-{secrets.token_hex(4)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)

    def tearDown(self):
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def test_pid_ip_and_pair_bans(self):
        self.accounts.ban(pid="0x1001", reason="cheat")
        self.assertTrue(self.accounts.is_banned(pid=0x1001))
        self.assertTrue(self.accounts.is_banned(pid=0x1001, ip="203.0.113.5"))   # pid banned anywhere
        self.accounts.unban(pid="0x1001")
        self.assertFalse(self.accounts.is_banned(pid=0x1001))
        self.accounts.ban(ip="203.0.113.5")
        self.assertTrue(self.accounts.is_banned(ip="203.0.113.5"))
        self.assertTrue(self.accounts.is_banned(pid=0x1001, ip="203.0.113.5"))
        self.accounts.unban(ip="203.0.113.5")
        self.assertFalse(self.accounts.is_banned(ip="203.0.113.5"))
        self.accounts.ban(pid=0x1001, ip="198.51.100.9")
        self.assertTrue(self.accounts.is_banned(pid=0x1001, ip="198.51.100.9"))
        self.assertFalse(self.accounts.is_banned(pid=0x1001))
        self.assertEqual(len(self.accounts.bans()), 1)

    def test_backup_and_restore(self):
        self.accounts.register(0x1001, PASSWORD)
        self.accounts.register(0x1002, PASSWORD)
        backup = DIRECTORY / f"backup-{secrets.token_hex(4)}.sqlite3"
        self.accounts.backup(backup)
        self.accounts.register(0x1003, PASSWORD)
        self.assertEqual(self.accounts.count(), 3)
        self.accounts.restore(backup)
        self.assertEqual(self.accounts.count(), 2)
        self.assertEqual(self.accounts.key(0x1001), self.accounts.key(0x1001))
        self.assertIsNotNone(self.accounts.key(0x1002))
        self.assertIsNone(self.accounts.key(0x1003))
        for path in (backup, pathlib.Path(str(backup) + "-wal"), pathlib.Path(str(backup) + "-shm")):
            path.unlink(missing_ok=True)


class BanEnforcementTests(unittest.TestCase):
    def setUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.path = DIRECTORY / f"enforce-{secrets.token_hex(4)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)
        self.accounts.register(0x1001, PASSWORD)

    def tearDown(self):
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def test_banned_pid_cannot_log_in(self):
        config = settings.default()
        config.configure(nexserver.ACCESS_KEY, nexserver.NEX_VERSION)
        station = "prudps:/address=127.0.0.1;port=60001;CID=1;PID=2;sid=1;stream=10;type=2"
        server = nexserver.AuthServer(config, b"k" * 16, station, "public", self.accounts)

        class Client:
            def pid(self):
                return 0x1001

            def remote_address(self):
                return ("203.0.113.5", 5000)

        self.assertIsInstance(server.user_key(0x1001, Client()), bytes)
        self.accounts.ban(pid=0x1001)
        with self.assertRaises(common.RMCError):
            server.user_key(0x1001, Client())


class AdminEndpointTests(unittest.TestCase):
    def setUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        # Global matchmaking state is shared with the other suites: start from a clean slate
        # so this file passes in any order (alphabetical, reverse, or after a session test).
        reset_matchmaking()
        self.path = DIRECTORY / f"admin-{secrets.token_hex(4)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)
        self.token = "s3cret"
        self.server = register_server.RegistrationServer(self.accounts, "127.0.0.1",
                                                         free_tcp_port(), self.token)
        self.server.start()

    def tearDown(self):
        self.server.stop()
        self.accounts.close()
        reset_matchmaking()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def get(self, path, token=None):
        request = urllib.request.Request(f"http://127.0.0.1:{self.server.port}{path}")
        if token:
            request.add_header("Authorization", f"Bearer {token}")
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, json.loads(response.read())

    def test_metrics_needs_no_token_and_hides_identities(self):
        status, payload = self.get("/metrics")
        self.assertEqual(status, 200)
        self.assertEqual(payload["players"], 0)
        self.assertEqual(payload["sessions"], 0)
        self.assertIn("errors", payload)
        self.assertNotIn("pids", payload)

    def test_admin_needs_token_and_kick_queues(self):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.get("/admin/sessions")
        self.assertEqual(caught.exception.code, 401)
        status, payload = self.get("/admin/sessions", self.token)
        self.assertEqual(status, 200)
        self.assertEqual(payload["sessions"], [])
        status, payload = self.get("/admin/kick?pid=0x1001", self.token)
        self.assertEqual((status, payload["kick"]), (200, "00001001"))
        self.assertIn(0x1001, matchmake.KICKS)


class KickApplyTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        reset_matchmaking()

    async def asyncTearDown(self):
        reset_matchmaking()

    async def test_kick_closes_client_and_forgets_player(self):
        closed = []

        class Client:
            async def close(self):
                closed.append(True)

        matchmake.CLIENTS[0x1001] = Client()
        matchmake.STATIONS[0x1001] = []
        matchmake.PEER[0x1001] = ("127.0.0.1", 1)
        matchmake.request_kick(0x1001)
        await matchmake.apply_kicks()
        self.assertTrue(closed)
        self.assertNotIn(0x1001, matchmake.CLIENTS)
        self.assertNotIn(0x1001, matchmake.STATIONS)
        self.assertNotIn(0x1001, matchmake.PEER)


if __name__ == "__main__":
    unittest.main()
