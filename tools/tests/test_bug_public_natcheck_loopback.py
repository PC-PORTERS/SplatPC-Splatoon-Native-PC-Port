"""Regression test for a public-config validation bug (bug hunt 2026-10-28, B-03).

`mode: "public"` used to accept addresses that can never work: an `advertise` that is a
loopback address (players are told to connect to their own PC), a loopback `listen`, and
NAT-check addresses on loopback (the NAT type is always answered locally, so no outside
player sees a reply). The server started normally and failed only when a player joined.
`server_config.validate()` now refuses them; this file keeps the four cases covered.
"""
import unittest

from tools.nexlab import server_config as sc


class PublicModeLoopbackValidation(unittest.TestCase):
    def test_loopback_advertise_is_refused_in_public_mode(self):
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="127.0.0.1", natcheck=False)

    def test_loopback_natcheck_addresses_are_refused_in_public_mode(self):
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10",
                            natcheck=True, natcheck_addresses=["127.0.0.1", "127.0.0.2"])

    def test_one_loopback_natcheck_address_is_refused_in_public_mode(self):
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(mode="public", listen="0.0.0.0", advertise="203.0.113.10",
                            natcheck=True, natcheck_addresses=["203.0.113.10", "127.0.0.2"])

    def test_loopback_listen_is_refused_in_public_mode(self):
        with self.assertRaises(sc.ConfigError):
            sc.ServerConfig(mode="public", listen="127.0.0.1", advertise="203.0.113.10",
                            natcheck=False)


if __name__ == "__main__":
    unittest.main()
