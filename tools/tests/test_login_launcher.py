"""The Linux sign-in launcher (scripts/splatoon_login.py) against a real local registration server.
    python3 -m unittest tools.tests.test_login_launcher -v
"""
import importlib.util
import os
import pathlib
import secrets
import shutil
import tempfile
import time
import unittest

from tools.nexlab import accounts as account_store, register_server

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("splatoon_login", ROOT / "scripts" / "splatoon_login.py")
login = importlib.util.module_from_spec(spec)
spec.loader.exec_module(login)


class Install:
    """A throwaway install folder: scripts/, Game/, VERSION."""
    def __init__(self, version="0.1.4"):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="login-"))
        (self.dir / "scripts").mkdir()
        (self.dir / "Game").mkdir()
        (self.dir / "VERSION").write_text(version + "\n")
        self.paths = login.Paths(str(self.dir))

    def server_file(self, name, text):
        (self.dir / "scripts" / name).write_text(text)

    def remove(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class PureTests(unittest.TestCase):
    def setUp(self):
        self.install = Install()
        self.addCleanup(self.install.remove)

    def test_versions(self):
        self.assertEqual(login.parse_version("0.2"), login.parse_version("v0.2.0"))
        self.assertLess(login.parse_version("0.1.4"), login.parse_version("0.1.10"))
        self.assertIsNone(login.parse_version("latest"))
        self.assertEqual(login.installed_version(str(self.install.dir)), (0, 1, 4))

    def test_pseudo(self):
        for good in ("Ana", "Le_Roi.2", "a b"):
            self.assertTrue(login.valid_pseudo(good), good)
        for bad in ("A", "abcdefghijk", "Ana;rm", "été"):
            self.assertFalse(login.valid_pseudo(bad), bad)

    def test_settings_keep_other_lines(self):
        settings = self.install.paths.settings
        pathlib.Path(settings).write_text("fullscreen=1\npseudo=Old\n")
        login.save_setting(settings, "pseudo", "New")
        self.assertEqual(login.get_setting(settings, "pseudo"), "New")
        self.assertEqual(login.get_setting(settings, "fullscreen"), "1")

    def test_server_file(self):
        self.install.server_file("server.public.txt", "server=pub.example:60000\nregister=https://pub.example/register\n")
        cfg, custom, bad = login.read_server_file(self.install.paths.scripts)
        self.assertEqual((cfg["server"], custom, bad), ("pub.example:60000", False, []))
        self.assertFalse(login.needs_foreign_confirmation(cfg, self.install.paths))
        self.install.server_file("server.txt", "server=bad_host:1\nnncs1=300.1.1.1\nregister=http://evil.example/r\n")
        cfg, custom, bad = login.read_server_file(self.install.paths.scripts)
        self.assertTrue(custom)
        self.assertEqual(sorted(bad), ["nncs1", "register", "server"])
        self.install.server_file("server.txt", "server=other.example:60000\nregister=https://other.example/register\n")
        cfg, _, bad = login.read_server_file(self.install.paths.scripts)
        self.assertEqual(bad, [])
        self.assertTrue(login.needs_foreign_confirmation(cfg, self.install.paths))

    def test_newest_executable(self):
        game = self.install.paths.game
        names = ("Splatoon.exe", "Splatoon-v288.exe") if os.name == "nt" else ("Splatoon", "Splatoon-v288")
        self.assertIsNone(login.find_executable(game))
        for age, name in enumerate(names):
            path = os.path.join(game, name)
            pathlib.Path(path).write_text("")
            os.utime(path, (time.time() - 100 * (len(names) - age),) * 2)
        self.assertEqual(os.path.basename(login.find_executable(game)), names[-1])

    def test_identity_format(self):
        path = self.install.paths.identity
        login.ensure_identity(path)
        fields = login.read_identity(path)
        self.assertTrue(0x10000000 <= int(fields["pid"], 16) <= 0x7FFFFFFE)
        self.assertRegex(fields["password"], r"^[0-9A-Za-z]{16}$")
        login.ensure_identity(path)                  # never replaced
        self.assertEqual(login.read_identity(path), fields)

    def test_environment(self):
        cfg = {"server": "pub.example:60000", "nncs1": "1.2.3.4", "nncs2": "1.2.3.5"}
        env = login.game_environment(cfg, self.install.paths, online=True, diag=False, run="x")
        self.assertEqual(env["RBREW_ONLINE_SERVER"], "pub.example:60000")
        self.assertEqual(env["RBREW_DNS_OVERRIDE"], "nncs1.app.nintendowifi.net=1.2.3.4,nncs2.app.nintendowifi.net=1.2.3.5")
        self.assertEqual(env["RBREW_PSEUDO"], "Player")
        self.assertTrue(env["RBREW_FRIENDS_DIR"].endswith("friends"))
        offline = login.game_environment({}, self.install.paths, online=False, diag=False, run="x")
        self.assertNotIn("RBREW_ONLINE_SERVER", offline)


class ServerTests(unittest.TestCase):
    def start(self, **options):
        db = pathlib.Path(tempfile.gettempdir()) / f"login-{secrets.token_hex(3)}.sqlite3"
        self.accounts = account_store.Accounts(db)
        server = register_server.RegistrationServer(self.accounts, "127.0.0.1", 0, **options).start()
        self.addCleanup(server.stop)
        url = f"http://127.0.0.1:{server._httpd.server_address[1]}"
        self.cfg = {"server": "127.0.0.1:60000", "register": url + "/register"}

    def setUp(self):
        self.install = Install()
        self.addCleanup(self.install.remove)

    def test_sign_up_then_claim(self):
        self.start()
        paths = self.install.paths
        self.assertEqual(login.prepare_online(self.cfg, paths, "Ana"), "")
        self.assertTrue(os.path.isfile(paths.identity))
        self.assertEqual(login.read_lines(paths.marker), [self.cfg["register"]])
        self.assertEqual(login.prepare_online(self.cfg, paths, "Ana"), "")      # already signed up: fine

        other = Install()
        self.addCleanup(other.remove)
        self.assertEqual(login.prepare_online(self.cfg, other.paths, "Ana"),
                         "This pseudo is already taken by another player. Choose a different one.")
        self.assertEqual(login.prepare_online(self.cfg, other.paths, "Bob"), "")

    def test_wrong_password_is_reported(self):
        self.start()
        paths = self.install.paths
        self.assertEqual(login.prepare_online(self.cfg, paths, "Ana"), "")
        fields = login.read_identity(paths.identity)
        pathlib.Path(paths.identity).write_text(f"pid={fields['pid']}\npassword={'x' * 16}\n")
        os.remove(paths.marker)
        self.assertEqual(login.register_online(self.cfg, paths),
                         "This account exists with another password. Keep your old online_identity.ini.")

    def test_min_client_version(self):
        self.start(min_client_version="0.2.0")
        message = login.prepare_online(self.cfg, self.install.paths, "Ana")
        self.assertEqual(login.Texts()(message), "This server needs version 0.2.0 or newer. Update the game, then try again.")
        self.assertFalse(os.path.isfile(self.install.paths.identity))          # stopped before sign-up

    def test_enforce_uses_latest_version(self):
        self.start(integrity_mode="enforce", integrity_releases={"0.1.4": ["a" * 64], "0.1.5": ["b" * 64]})
        message = login.check_server_version(self.cfg, (0, 1, 4))
        self.assertEqual(message[1], "0.1.5")
        self.assertEqual(login.check_server_version(self.cfg, (0, 1, 5)), "")

    def test_log_mode_does_not_block(self):
        self.start(integrity_mode="log", integrity_releases={"0.1.5": ["b" * 64]})
        self.assertEqual(login.check_server_version(self.cfg, (0, 1, 4)), "")

    def test_unreachable_server(self):
        cfg = {"register": "http://127.0.0.1:9/register"}
        self.assertEqual(login.check_server_version(cfg, (0, 1, 4)), "")
        self.assertEqual(login.register_online(cfg, self.install.paths),
                         "The server cannot be reached. Check your Internet connection.")

    def test_plain_http_refused_for_remote(self):
        self.assertEqual(login.register_online({"register": "http://pub.example/register"}, self.install.paths),
                         "The server sign-up must use https.")


if __name__ == "__main__":
    unittest.main()
