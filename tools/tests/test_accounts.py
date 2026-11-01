"""Account store and registration endpoint tests.

Run from the repository root:
    py -3.12 -m unittest tools.tests.test_accounts -v
"""
import json
import pathlib
import secrets
import socket
import urllib.error
import urllib.request
import unittest

from nintendo.nex import kerberos
from tools.nexlab import accounts as account_store, register_server

DIRECTORY = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"
PID = 0x5EEA2491
PASSWORD = "0000100100001001"


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class AccountTests(unittest.TestCase):
    def setUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.db = DIRECTORY / f"accounts-{secrets.token_hex(4)}.sqlite3"
        self.accounts = account_store.Accounts(self.db)

    def tearDown(self):
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.db) + suffix).unlink(missing_ok=True)

    def test_register_stores_the_derived_key_only(self):
        self.assertEqual(self.accounts.register(PID, PASSWORD), "created")
        expected = kerberos.KeyDerivationOld().derive_key(PASSWORD.encode(), PID)
        self.assertEqual(self.accounts.key(PID), expected)
        self.assertTrue(self.accounts.verify(PID, PASSWORD))
        self.assertFalse(self.accounts.verify(PID, "0000100100001002"))
        self.assertFalse(self.accounts.verify(0x1234, PASSWORD))
        # The password must not appear anywhere in the database file.
        raw = self.db.read_bytes()
        self.assertNotIn(PASSWORD.encode(), raw)
        self.assertNotIn(PASSWORD.encode("utf-16-le"), raw)

    def test_same_password_is_idempotent_and_another_is_refused(self):
        self.accounts.register(PID, PASSWORD)
        self.assertEqual(self.accounts.register(PID, PASSWORD), "exists")
        with self.assertRaises(account_store.AlreadyRegistered):
            self.accounts.register(PID, "0000100100001002")
        self.assertEqual(self.accounts.count(), 1)

    def test_invalid_identities_are_refused(self):
        for pid, password in ((0, PASSWORD), (0x80000000, PASSWORD), ("zz", PASSWORD),
                              (PID, "short"), (PID, "000010010000100"), (PID, "0000-1001-0000")):
            with self.subTest(pid=pid, password=password), self.assertRaises(account_store.InvalidIdentity):
                self.accounts.register(pid, password)

    def test_rate_limit_per_ip_and_window(self):
        now = [1000.0]
        store = account_store.Accounts(self.db, max_attempts=3, window=60.0, clock=lambda: now[0])
        try:
            self.assertTrue(store.allow("203.0.113.7"))
            for _ in range(3):
                store.failure("203.0.113.7")
            self.assertFalse(store.allow("203.0.113.7"))
            self.assertTrue(store.allow("203.0.113.8"))       # other IP untouched
            now[0] += 61.0
            self.assertTrue(store.allow("203.0.113.7"))       # window expired
            store.failure("203.0.113.7")
            store.forget_failures("203.0.113.7")
            self.assertTrue(store.allow("203.0.113.7"))
        finally:
            store.close()

    def test_accounts_survive_a_reopen(self):
        self.accounts.register(PID, PASSWORD)
        self.accounts.close()
        reopened = account_store.Accounts(self.db)
        try:
            self.assertTrue(reopened.verify(PID, PASSWORD))
        finally:
            reopened.close()


class RegistrationEndpointTests(unittest.TestCase):
    def setUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.db = DIRECTORY / f"accounts-{secrets.token_hex(4)}.sqlite3"
        self.accounts = account_store.Accounts(self.db)
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            server.stop()
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.db) + suffix).unlink(missing_ok=True)

    def start(self, token=None):
        server = register_server.RegistrationServer(self.accounts, "127.0.0.1", free_tcp_port(), token)
        server.start()
        self.servers.append(server)
        return server

    def post(self, port, payload, token=None, raw=None):
        body = raw if raw is not None else json.dumps(payload).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{port}/register", data=body,
                                         headers={"Content-Type": "application/json"})
        if token:
            request.add_header("Authorization", f"Bearer {token}")
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def test_health_and_register(self):
        server = self.start()
        with urllib.request.urlopen(f"http://127.0.0.1:{server.port}/health", timeout=5) as response:
            self.assertEqual(response.status, 200)
            self.assertNotIn("accounts", json.loads(response.read()))
        status, payload = self.post(server.port, {"pid": "5eea2491", "password": PASSWORD})
        self.assertEqual((status, payload["status"]), (200, "created"))
        self.assertTrue(self.accounts.verify(PID, PASSWORD))
        status, payload = self.post(server.port, {"pid": PID, "password": PASSWORD})
        self.assertEqual((status, payload["status"]), (200, "exists"))

    def test_collision_invalid_body_and_token(self):
        server = self.start(token="s3cret")
        status, _ = self.post(server.port, {"pid": PID, "password": PASSWORD})
        self.assertEqual(status, 401)                          # token required
        status, _ = self.post(server.port, {"pid": PID, "password": PASSWORD}, token="wrong")
        self.assertEqual(status, 401)
        status, payload = self.post(server.port, {"pid": PID, "password": PASSWORD}, token="s3cret")
        self.assertEqual((status, payload["status"]), (200, "created"))
        status, payload = self.post(server.port, {"pid": PID, "password": "0000100100001002"}, token="s3cret")
        self.assertEqual(status, 409)
        status, _ = self.post(server.port, None, token="s3cret", raw=b"{not json")
        self.assertEqual(status, 400)
        status, _ = self.post(server.port, {"pid": PID, "password": "short"}, token="s3cret")
        self.assertEqual(status, 400)


if __name__ == "__main__":
    unittest.main()
