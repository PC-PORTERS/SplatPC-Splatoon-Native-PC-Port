"""Game integrity (scripts/integrity.py + the server's integrity_mode) and the stronger moderation.
    py -3.12 -m unittest tools.tests.test_integrity -v
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import secrets
import shutil
import sqlite3
import tempfile
import unittest
import urllib.error
import urllib.request

from tools.nexlab import accounts as account_store, ops, register_server, server_config, social

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


integrity = load("integrity", "scripts/integrity.py")
friends_relay = load("friends_relay", "scripts/friends_relay.py")


def make_install(base, files, version="0.1.3"):
    """A fake install: files {relative: bytes}, a VERSION and a manifest listing the files."""
    root = pathlib.Path(base)
    for relative, data in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (root / "VERSION").write_text(version + "\n")
    names = sorted(files) + ["VERSION"]
    text = "".join(f"{integrity.hash_file(str(root / n))} {n}\n" for n in names)
    (root / "scripts").mkdir(exist_ok=True)
    (root / "scripts" / "integrity-manifest.txt").write_text(text, newline="\n")
    return root, text


class ClientHashTests(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="integrity-"))
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.root, self.text = make_install(self.dir, {"recompiler/a.cpp": b"int a;\nint b;\n", "scripts/x.py": b"print(1)\n"})

    def test_a_clean_install_proves_the_release_hash(self):
        proof = integrity.attestation(str(self.root))
        self.assertEqual(proof["version"], "0.1.3")
        self.assertEqual(proof["code"], integrity.release_hash(self.text))
        self.assertEqual(integrity.differing(str(self.root)), [])

    def test_line_endings_do_not_change_the_hash(self):
        (self.root / "recompiler" / "a.cpp").write_bytes(b"int a;\r\nint b;\r\n")
        self.assertEqual(integrity.attestation(str(self.root))["code"], integrity.release_hash(self.text))

    def test_an_edited_or_missing_file_changes_the_hash(self):
        good = integrity.release_hash(self.text)
        (self.root / "recompiler" / "a.cpp").write_bytes(b"int a;\nint cheat;\n")
        self.assertNotEqual(integrity.attestation(str(self.root))["code"], good)
        self.assertEqual(integrity.differing(str(self.root)), ["recompiler/a.cpp"])
        (self.root / "recompiler" / "a.cpp").unlink()
        self.assertNotEqual(integrity.attestation(str(self.root))["code"], good)
        self.assertIn("recompiler/a.cpp", integrity.differing(str(self.root)))

    def test_the_version_comes_from_VERSION_not_from_installed_version(self):
        stale = self.root / "build"
        stale.mkdir()
        (stale / "installed-version.txt").write_text("0.0.1\n")
        self.assertEqual(integrity.read_version(str(self.root)), "0.1.3")
        (self.root / "VERSION").unlink()
        self.assertEqual(integrity.read_version(str(self.root)), "")

    def test_no_manifest_means_no_proof(self):
        (self.root / "scripts" / "integrity-manifest.txt").unlink()
        self.assertEqual(integrity.attestation(str(self.root))["code"], "")

    def test_what_is_covered(self):
        for yes in ("recompiler/port/os/x.cpp", "scripts/Launch-Online.ps1", "VERSION", "profiles/agmp-v288.json"):
            self.assertTrue(integrity.covered(yes), yes)
        for no in ("docs/x.md", "scripts/tests/t.ps1", "scripts/integrity-manifest.txt", "scripts/server.txt",
                   "tools/nexlab/social.py", "scripts/__pycache__/x.pyc"):
            self.assertFalse(integrity.covered(no), no)

    def test_the_real_install_hashes(self):
        """The repository itself: the proof is computed (even if the manifest is stale until a release)."""
        proof = integrity.attestation()
        self.assertRegex(proof["version"], r"^\d+\.\d+")


class ConfigTests(unittest.TestCase):
    def test_defaults_and_validation(self):
        self.assertEqual(server_config.ServerConfig().integrity_mode, "off")
        good = "ab" * 32
        config = server_config.ServerConfig(integrity_mode="enforce", integrity_releases={"0.1.3": good, "0.1.4": [good]})
        self.assertEqual(config.integrity_releases["0.1.3"], [good])
        for bad in ({"integrity_mode": "strict"},
                    {"integrity_mode": "enforce"},
                    {"integrity_releases": {"0.1.3": "xyz"}},
                    {"integrity_releases": {"latest": good}},
                    {"integrity_releases": [good]}):
            with self.assertRaises(server_config.ConfigError, msg=str(bad)):
                server_config.ServerConfig(**bad)


class SocialIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.path = pathlib.Path(tempfile.gettempdir()) / f"integ-{secrets.token_hex(3)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)
        self.pid, self.password = 0x10000001, "A" * 16
        self.accounts.register(self.pid, self.password)
        self.release = "cd" * 32
        self.now = [1000.0]

    def tearDown(self):
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def make(self, mode):
        return social.Social(self.accounts, clock=lambda: self.now[0], integrity_mode=mode, releases={"0.1.3": [self.release]})

    def test_off_ignores_the_proof(self):
        service = self.make("off")
        self.assertTrue(service.open_session(self.pid, self.password))
        self.assertTrue(service.is_attested(self.pid))

    def test_log_lets_everybody_in(self):
        service = self.make("log")
        self.assertTrue(service.open_session(self.pid, self.password, {"version": "0.1.0", "code": "00" * 32}))
        self.assertTrue(service.is_attested(self.pid))              # only enforce gates the NEX login

    def test_enforce_refuses_unknown_code_and_accepts_a_release(self):
        service = self.make("enforce")
        self.assertFalse(service.is_attested(self.pid))
        for client in (None, "x", {}, {"code": 5}, {"code": "00" * 32}, {"code": self.release.upper()}, {"code": "ab"}):
            with self.assertRaises(social.SocialError) as caught:
                service.open_session(self.pid, self.password, client)
            self.assertEqual(caught.exception.status, 426, client)
        self.assertFalse(service.is_attested(self.pid))
        token = service.open_session(self.pid, self.password, {"version": "0.1.3", "code": self.release})
        self.assertTrue(token)
        self.assertTrue(service.is_attested(self.pid))

    def test_the_proof_runs_out_unless_the_player_keeps_playing(self):
        service = self.make("enforce")
        token = service.open_session(self.pid, self.password, {"code": self.release})
        self.now[0] += social.TOKEN_TTL - 60
        service.heartbeat(token, "", [])                            # a heartbeat renews it
        self.now[0] += social.TOKEN_TTL - 60
        self.assertTrue(service.is_attested(self.pid))
        self.now[0] += 120
        self.assertFalse(service.is_attested(self.pid))

    def test_a_refused_proof_withdraws_an_earlier_one(self):
        service = self.make("enforce")
        service.open_session(self.pid, self.password, {"code": self.release})
        with self.assertRaises(social.SocialError):
            service.open_session(self.pid, self.password, {"code": "00" * 32})
        self.assertFalse(service.is_attested(self.pid))

    def test_revoke_drops_session_presence_and_proof(self):
        service = self.make("enforce")
        token = service.open_session(self.pid, self.password, {"code": self.release})
        service.revoke(self.pid)
        self.assertIsNone(service.pid_of(token))
        self.assertFalse(service.is_attested(self.pid))


class RelayIntegrityTests(unittest.TestCase):
    """The relay against a real local server in enforce mode."""

    def setUp(self):
        self.path = pathlib.Path(tempfile.gettempdir()) / f"integ-relay-{secrets.token_hex(3)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)
        self.pid, self.password = 0x10000001, "B" * 16
        self.accounts.register(self.pid, self.password)
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="integrity-relay-"))
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.root, self.text = make_install(self.dir / "install", {"recompiler/a.cpp": b"int a;\n"})
        self.release = integrity.release_hash(self.text)
        self.server = register_server.RegistrationServer(
            self.accounts, "127.0.0.1", 0, integrity_mode="enforce", integrity_releases={"0.1.3": [self.release]}).start()
        self.url = f"http://127.0.0.1:{self.server._httpd.server_address[1]}"
        self.game = self.dir / "game"
        self.game.mkdir()

    def tearDown(self):
        self.server.stop()
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def relay(self, client):
        return friends_relay.Relay(self.url, self.pid, self.password, "Alice", str(self.game), client=client)

    def test_official_files_sign_in(self):
        relay = self.relay(integrity.attestation(str(self.root)))
        relay.step()
        self.assertEqual(relay.message, "")
        self.assertTrue(self.server.social.is_attested(self.pid))

    def test_edited_files_are_refused_with_a_readable_message(self):
        (self.root / "recompiler" / "a.cpp").write_bytes(b"int cheat;\n")
        relay = self.relay(integrity.attestation(str(self.root)))
        relay.step()
        self.assertIn("not recognised", relay.message)
        self.assertFalse(self.server.social.is_attested(self.pid))
        state = (self.game / "friends" / "state.res").read_text()
        self.assertIn("msg|game files not recognised", state)

    def test_health_publishes_mode_and_latest_version_but_no_hash(self):
        import json, urllib.request
        with urllib.request.urlopen(self.url + "/health", timeout=5) as reply:
            body = json.loads(reply.read())
        self.assertEqual(body["integrity_mode"], self.server.social.integrity_mode)
        self.assertEqual(body["latest_version"], self.server.social.latest_version())
        self.assertNotIn("code", json.dumps(body))

    def test_refusal_names_the_version_to_update_to(self):
        (self.root / "recompiler" / "a.cpp").write_bytes(b"int cheat;\n")
        relay = self.relay(integrity.attestation(str(self.root)))
        relay.step()
        self.assertIn("update the game to " + self.server.social.latest_version(), relay.message)

    def test_an_old_relay_without_a_proof_is_refused(self):
        relay = self.relay(None)
        relay.step()
        self.assertIn("update the game", relay.message)


class ModerationTests(unittest.TestCase):
    def setUp(self):
        self.path = pathlib.Path(tempfile.gettempdir()) / f"mod-{secrets.token_hex(3)}.sqlite3"
        self.now = [5000.0]
        self.accounts = account_store.Accounts(self.path, clock=lambda: self.now[0])

    def tearDown(self):
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def test_a_temporary_ban_ends(self):
        self.accounts.ban(0x10000001, None, "spam", hours=2)
        self.assertTrue(self.accounts.is_banned(pid=0x10000001))
        self.assertEqual(len(self.accounts.bans()), 1)
        self.now[0] += 2 * 3600 + 1
        self.assertFalse(self.accounts.is_banned(pid=0x10000001))
        self.assertEqual(self.accounts.bans(), [])

    def test_a_permanent_ban_stays_and_a_reban_replaces_it(self):
        self.accounts.ban(0x10000001, None, "first")
        self.accounts.ban(0x10000001, None, "second", hours=1)
        self.assertEqual(len(self.accounts.bans()), 1)
        self.assertEqual(self.accounts.bans()[0]["reason"], "second")
        self.accounts.ban(0x10000001, None, "for good")
        self.now[0] += 10 ** 6
        self.assertTrue(self.accounts.is_banned(pid=0x10000001))

    def test_bad_bans_are_refused(self):
        for kwargs in ({"ip": "not an address"}, {"pid": 5, "hours": 0}, {"pid": 5, "hours": -3},
                       {"pid": 5, "hours": "soon"}, {"pid": 5, "hours": 10 ** 6}):
            with self.assertRaises(account_store.InvalidIdentity, msg=str(kwargs)):
                self.accounts.ban(**kwargs)
        self.assertFalse(self.accounts.is_banned(ip="unknown"))

    def test_every_action_is_logged_newest_first_and_bounded(self):
        self.accounts.ban(0x10000001, None, "spam", hours=1)
        self.now[0] += 1
        self.accounts.unban(0x10000001)
        self.accounts.log_action("kick", 0x10000002)
        log = self.accounts.mod_log()
        self.assertEqual([entry["action"] for entry in log], ["kick", "unban", "ban"])
        self.assertIn("1 h: spam", log[2]["detail"])
        for _ in range(2100):
            self.accounts.log_action("kick", 1)
        self.assertLessEqual(len(self.accounts.mod_log(500)), 500)
        with contextlib.closing(sqlite3.connect(str(self.path))) as db:
            self.assertLessEqual(db.execute("SELECT COUNT(*) FROM mod_log").fetchone()[0], 2001)

    def test_an_old_database_gets_the_new_columns(self):
        old = pathlib.Path(tempfile.gettempdir()) / f"mod-old-{secrets.token_hex(3)}.sqlite3"
        with contextlib.closing(sqlite3.connect(str(old))) as db:
            db.execute("CREATE TABLE bans (pid INTEGER, ip TEXT, reason TEXT, created REAL NOT NULL, UNIQUE(pid, ip))")
            db.execute("INSERT INTO bans VALUES (?, NULL, 'old', 1.0)", (0x10000009,))
            db.commit()
        accounts = account_store.Accounts(old)
        try:
            self.assertTrue(accounts.is_banned(pid=0x10000009))
            self.assertIsNone(accounts.bans()[0]["expires"])
        finally:
            accounts.close()
            for suffix in ("", "-wal", "-shm"):
                pathlib.Path(str(old) + suffix).unlink(missing_ok=True)


class AdminRouteTests(unittest.TestCase):
    TOKEN = "panel-secret"

    def setUp(self):
        self.path = pathlib.Path(tempfile.gettempdir()) / f"adm-{secrets.token_hex(3)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)
        self.pid, self.password = 0x10000001, "C" * 16
        self.accounts.register(self.pid, self.password)
        self.server = register_server.RegistrationServer(self.accounts, "127.0.0.1", 0, admin_token=self.TOKEN).start()
        self.base = f"http://127.0.0.1:{self.server._httpd.server_address[1]}"

    def tearDown(self):
        self.server.stop()
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def call(self, path, body=None, token=TOKEN):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.base + path, data=data, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=5) as reply:
                return reply.status, json.loads(reply.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read() or b"{}")

    def test_routes_need_the_admin_token(self):
        for path, body in (("/admin/ban", {"pid": "0x10000001"}), ("/admin/unban", {"pid": "0x10000001"}),
                           ("/admin/bans", None), ("/admin/log", None)):
            self.assertEqual(self.call(path, body, token="wrong")[0], 401, path)
            self.assertEqual(self.call(path, body, token="")[0], 401, path)
        self.assertFalse(self.accounts.is_banned(pid=self.pid))

    def test_ban_kicks_logs_and_ends_the_session(self):
        token = self.server.social.open_session(self.pid, self.password)
        status, _ = self.call("/admin/ban", {"pid": "0x10000001", "hours": 3, "reason": "insults"})
        self.assertEqual(status, 200)
        self.assertTrue(self.accounts.is_banned(pid=self.pid))
        self.assertIsNone(self.server.social.pid_of(token))
        from tools.nexlab import matchmake
        self.assertIn(self.pid, matchmake.KICKS)
        matchmake.KICKS.discard(self.pid)
        status, listed = self.call("/admin/bans")
        self.assertEqual(listed["bans"][0]["pid"], "10000001")
        self.assertIsNotNone(listed["bans"][0]["expires"])
        status, logged = self.call("/admin/log")
        self.assertEqual(logged["log"][0]["action"], "ban")
        self.assertEqual(self.call("/admin/unban", {"pid": "0x10000001"})[0], 200)
        self.assertFalse(self.accounts.is_banned(pid=self.pid))
        self.assertEqual([e["action"] for e in self.call("/admin/log")[1]["log"]], ["unban", "ban"])

    def test_bad_input_is_a_400_not_a_ban(self):
        for body in ({"ip": "banana"}, {"pid": "zzz"}, {"pid": "0x10000001", "hours": "soon"}, [1], {}):
            self.assertEqual(self.call("/admin/ban", body)[0], 400, body)
        self.assertEqual(self.accounts.bans(), [])

    def test_ops_client_builds_the_requests(self):
        calls = []
        original = ops.request
        ops.request = lambda url, token, path, body=None: (calls.append((path, body)) or (200, {"ok": True}))
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ops.main(["--url", "x", "--token", "t", "ban", "--pid", "0x10000001", "--hours", "2", "--reason", "r"]), 0)
                self.assertEqual(ops.main(["--url", "x", "--token", "t", "unban", "--ip", "1.2.3.4"]), 0)
                self.assertEqual(ops.main(["--url", "x", "--token", "t", "bans"]), 0)
        finally:
            ops.request = original
        self.assertEqual(calls[0], ("/admin/ban", {"pid": "0x10000001", "ip": None, "hours": 2.0, "reason": "r"}))
        self.assertEqual(calls[1], ("/admin/unban", {"pid": None, "ip": "1.2.3.4"}))
        self.assertEqual(calls[2], ("/admin/bans", None))


if __name__ == "__main__":
    unittest.main()
