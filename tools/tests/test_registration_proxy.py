"""Registration behind a local HTTPS proxy (registration_proxy). Run from the repository root:
    py -3.12 -m unittest tools.tests.test_registration_proxy -v
"""
import unittest
from types import SimpleNamespace

from tools.nexlab import register_server, server_config as sc


class ClientIpTests(unittest.TestCase):
    proxied = SimpleNamespace(behind_proxy=True)
    direct = SimpleNamespace(behind_proxy=False)

    def test_direct_mode_ignores_forwarded_header(self):
        headers = {"X-Forwarded-For": "198.51.100.7"}
        self.assertEqual(register_server.client_ip(self.direct, "203.0.113.5", headers), "203.0.113.5")

    def test_proxy_mode_uses_the_entry_the_proxy_appended(self):
        headers = {"X-Forwarded-For": "10.0.0.1, 198.51.100.7"}
        self.assertEqual(register_server.client_ip(self.proxied, "127.0.0.1", headers), "198.51.100.7")

    def test_proxy_mode_trusts_the_header_only_from_the_loopback(self):
        headers = {"X-Forwarded-For": "198.51.100.7"}
        self.assertEqual(register_server.client_ip(self.proxied, "203.0.113.5", headers), "203.0.113.5")

    def test_missing_or_invalid_header_keeps_the_peer(self):
        self.assertEqual(register_server.client_ip(self.proxied, "127.0.0.1", {}), "127.0.0.1")
        headers = {"X-Forwarded-For": "not-an-ip"}
        self.assertEqual(register_server.client_ip(self.proxied, "127.0.0.1", headers), "127.0.0.1")


class ConfigTests(unittest.TestCase):
    def test_option_is_off_by_default_and_read_when_set(self):
        self.assertFalse(sc.ServerConfig().registration_proxy)
        self.assertTrue(sc.ServerConfig(registration_proxy=True).registration_proxy)


class MinClientVersionTests(unittest.TestCase):
    def test_config_accepts_a_version_and_rejects_garbage(self):
        self.assertIsNone(sc.ServerConfig().min_client_version)
        self.assertEqual(sc.ServerConfig(min_client_version="0.2.0").min_client_version, "0.2.0")
        for bad in ("abc", "1", "1.2.3.4", "v1.0"):
            with self.assertRaises(sc.ConfigError):
                sc.ServerConfig(min_client_version=bad)

    def test_health_publishes_the_minimum_only_when_set(self):
        import json
        import urllib.request
        for minimum, expected in ((None, {"ok": True}), ("0.2.0", {"ok": True, "min_client_version": "0.2.0"})):
            # /health never touches the account store, so none is needed here.
            server = register_server.RegistrationServer(None, "127.0.0.1", 0, min_client_version=minimum)
            server.start()
            try:
                port = server._httpd.server_address[1]
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=5) as reply:
                    self.assertEqual(json.loads(reply.read()), expected)
            finally:
                server.stop()


if __name__ == "__main__":
    unittest.main()
