"""Configuration tests for the lab/public server.

Run from the repository root:
    py -3.12 -m unittest tools.tests.test_server_config -v
"""
import argparse
import json
import pathlib
import secrets
import unittest

from tools.nexlab import nexserver as lab, server_config as sc

DIRECTORY = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"


def namespace(**values):
    """The attributes nexserver.parse_config reads, with argparse defaults."""
    base = dict(configfile=None, mode=None, public=False, listen=None, advertise=None, port=None,
                secure_port=None, natcheck=None, natcheck_address=None, identity=[],
                identity_root=None, accounts_db=None, registration=None, registration_port=None,
                registration_token=None, fill_timeout=None)
    base.update(values)
    return argparse.Namespace(**base)


class ServerConfigTests(unittest.TestCase):
    def setUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.files = []

    def tearDown(self):
        for path in self.files:
            path.unlink(missing_ok=True)

    def make_file(self, **values):
        path = DIRECTORY / f"config-{secrets.token_hex(4)}.json"
        path.write_text(json.dumps(values), encoding="utf-8")
        self.files.append(path)
        return path

    # -- defaults ----------------------------------------------------------
    def test_defaults_are_exactly_the_lab_server(self):
        config = sc.ServerConfig()
        self.assertEqual(config.mode, "lab")
        self.assertEqual((config.listen, config.station_host()), ("127.0.0.1", "127.0.0.1"))
        self.assertEqual((config.auth_port, config.secure_port), (60000, 60001))
        self.assertTrue(config.natcheck)
        self.assertIsNone(config.identity_roots)
        self.assertFalse(config.is_public())

    def test_public_defaults(self):
        config = sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10")
        self.assertFalse(config.natcheck)                      # completed in step 4
        self.assertEqual(config.secure_port, 60001)
        self.assertTrue(config.is_public())

    def test_registration_defaults_and_validation(self):
        lab = sc.ServerConfig()
        self.assertEqual((lab.registration, lab.registration_port), ("closed", 0))
        public = sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10")
        self.assertEqual((public.registration, public.registration_port), ("closed", 0))
        self.assertEqual(public.accounts_db, "nexlab-accounts.sqlite3")
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10", registration="token")
        token = sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10", registration="token", registration_token="s3cret")
        self.assertEqual(token.registration, "token")
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10", registration="nope")

    def test_fill_timeout(self):
        self.assertEqual(sc.ServerConfig().fill_timeout, 20.0)
        self.assertEqual(sc.ServerConfig(fill_timeout=5).fill_timeout, 5.0)
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(fill_timeout=0)
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(fill_timeout="soon")
        self.assertEqual(lab.parse_config(namespace(fill_timeout=45)).fill_timeout, 45.0)

    def test_public_natcheck_single_ip_needs_explicit_opt_in(self):
        base = dict(mode="public", listen="0.0.0.0", advertise="203.0.113.10", natcheck=True,
                    natcheck_addresses=["203.0.113.10"])
        with self.assertRaises(sc.ConfigError):          # still refused by default
            sc.ServerConfig(**base)
        config = sc.ServerConfig(**base, natcheck_single_ip=True)
        self.assertEqual(config.natcheck_addresses, ["203.0.113.10"])
        with self.assertRaises(sc.ConfigError):          # the opt-in never allows two equal IPs
            sc.ServerConfig(**dict(base, natcheck_addresses=["203.0.113.10", "203.0.113.10"]),
                            natcheck_single_ip=True)

    def test_public_natcheck_needs_two_different_ips(self):
        # Pretendo: nncs1 and nncs2 must be two different public IPs (UDP 10025 and 10125).
        base = dict(mode="public", listen="0.0.0.0", advertise="203.0.113.10", natcheck=True)
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(**base)
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(**base, natcheck_addresses=["203.0.113.10"])
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(**base, natcheck_addresses=["203.0.113.10", "203.0.113.10"])
        config = sc.ServerConfig(**base, natcheck_addresses=["203.0.113.10", "203.0.113.11"])
        self.assertEqual(config.natcheck_addresses, ["203.0.113.10", "203.0.113.11"])
        # The lab keeps its loopback pair without asking for anything.
        self.assertIsNone(sc.ServerConfig().natcheck_addresses)

    # -- validation --------------------------------------------------------
    def test_wildcard_listen_requires_an_advertised_address(self):
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(mode="public", listen="0.0.0.0")
        config = sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="srv.example.net")
        self.assertEqual(config.station_host(), "srv.example.net")

    def test_rejects_bad_values(self):
        for values in (
            {"mode": "nope"},
            {"listen": "not-an-address"},
            {"advertise": "http://example.com"},
            {"auth_port": 0},
            {"auth_port": 70000},
            {"auth_port": 5000, "secure_port": 5000},
            {"natcheck_addresses": ["nope"]},
            {"natcheck_addresses": []},
            {"unknown_key": 1},
        ):
            with self.subTest(values=values), self.assertRaises(sc.ConfigError):
                sc.ServerConfig(**values)

    # -- file + overrides --------------------------------------------------
    def test_load_json_ignores_comment_keys(self):
        path = self.make_file(_help="notes are ignored", mode="public", listen="0.0.0.0",
                              advertise="203.0.113.10", auth_port=7000)
        config = sc.ServerConfig.load(path)
        self.assertEqual((config.mode, config.auth_port, config.secure_port), ("public", 7000, 7001))
        self.assertEqual(config.station_host(), "203.0.113.10")

    def test_load_errors(self):
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig.load(DIRECTORY / "does-not-exist.json")
        path = DIRECTORY / f"broken-{secrets.token_hex(4)}.json"
        path.write_text('{"mode": ', encoding="utf-8")
        self.files.append(path)
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig.load(path)

    def test_updated_keeps_the_rest(self):
        config = sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10", auth_port=7000,
                                 natcheck_addresses=["203.0.113.10"])
        pushed = config.updated(auth_port=7100, identities=["a.ini"])
        self.assertEqual((pushed.mode, pushed.auth_port, pushed.identities), ("public", 7100, ["a.ini"]))
        self.assertEqual(pushed.natcheck_addresses, ["203.0.113.10"])
        self.assertEqual(pushed.advertise, "203.0.113.10")

    # -- command line ------------------------------------------------------
    def test_command_line_defaults_match_lab(self):
        config = lab.parse_config(namespace())
        self.assertEqual((config.mode, config.listen, config.auth_port, config.secure_port),
                         ("lab", "127.0.0.1", 60000, 60001))
        self.assertTrue(config.natcheck)

    def test_port_moves_the_pair_and_public_switch(self):
        config = lab.parse_config(namespace(port=7100))
        self.assertEqual((config.auth_port, config.secure_port), (7100, 7101))
        public = lab.parse_config(namespace(public=True, port=7200, advertise="203.0.113.10"))
        self.assertEqual((public.mode, public.auth_port, public.secure_port), ("public", 7200, 7201))

    def test_command_line_conflict_and_no_natcheck(self):
        with self.assertRaises(sc.ConfigError):
            lab.parse_config(namespace(public=True, mode="lab"))
        config = lab.parse_config(namespace(natcheck=False))
        self.assertFalse(config.natcheck)
        config = lab.parse_config(namespace(natcheck_address=["203.0.113.10"]))
        self.assertEqual(config.natcheck_addresses, ["203.0.113.10"])

    def test_command_line_registration(self):
        config = lab.parse_config(namespace(public=True, registration="token", registration_token="abc",
                                            accounts_db="players.sqlite3", registration_port=60080,
                                            advertise="203.0.113.10"))
        self.assertEqual((config.registration, config.registration_port, config.accounts_db),
                         ("token", 60080, "players.sqlite3"))
        self.assertEqual(config.registration_token, "abc")

    def test_command_line_overrides_a_file(self):
        path = self.make_file(mode="lab", listen="0.0.0.0", advertise="203.0.113.10", auth_port=7000)
        config = lab.parse_config(namespace(configfile=path, public=True))
        self.assertEqual((config.mode, config.auth_port), ("public", 7000))
        self.assertEqual(config.listen, "0.0.0.0")


if __name__ == "__main__":
    unittest.main()
