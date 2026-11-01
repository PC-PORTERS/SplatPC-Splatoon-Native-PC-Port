"""Friends by pseudo: unique pseudos, lookup, mutual presence, over the real HTTP handler.
    py -3.12 -m unittest tools.tests.test_social -v
"""
import json
import pathlib
import secrets
import tempfile
import unittest
import urllib.error
import urllib.request

from tools.nexlab import accounts as account_store, register_server, social

MODE_A = "11" * 44
MODE_B = "22" * 44


class SocialTests(unittest.TestCase):
    def setUp(self):
        self.path = pathlib.Path(tempfile.gettempdir()) / f"social-{secrets.token_hex(3)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)
        self.pids = [0x10000001, 0x10000002, 0x10000003]
        self.passwords = {pid: f"{pid:08x}" * 2 for pid in self.pids}
        for pid in self.pids:
            self.accounts.register(pid, self.passwords[pid])
        self.server = register_server.RegistrationServer(self.accounts, "127.0.0.1", 0).start()
        self.port = self.server._httpd.server_address[1]

    def tearDown(self):
        self.server.stop()
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    def post(self, route, body):
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}/social/{route}", data=json.dumps(body).encode(),
                                         headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=5) as reply:
                return reply.status, json.loads(reply.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read() or b"{}")

    def session(self, pid):
        status, answer = self.post("session", {"pid": f"0x{pid:08x}", "password": self.passwords[pid]})
        self.assertEqual(status, 200)
        return answer["token"]

    def claim(self, pid, pseudo):
        return self.post("pseudo", {"pid": f"0x{pid:08x}", "password": self.passwords[pid], "pseudo": pseudo})

    def test_session_needs_the_right_password(self):
        status, _ = self.post("session", {"pid": f"0x{self.pids[0]:08x}", "password": "wrongwrongwrong1"})
        self.assertEqual(status, 401)
        status, _ = self.post("session", {"pid": "0x10000099", "password": "abcdefghijklmnop"})
        self.assertEqual(status, 401)
        self.assertTrue(self.session(self.pids[0]))

    def test_pseudos_are_unique_ignoring_case_and_can_be_changed(self):
        a, b, c = self.pids
        self.assertEqual(self.claim(a, "Inkling")[0], 200)
        self.assertEqual(self.claim(a, "Inkling")[0], 200)                 # again: still the owner
        self.assertEqual(self.claim(b, "inkling")[0], 409)                 # same name, other case
        self.assertEqual(self.claim(b, "Octoling")[0], 200)
        self.assertEqual(self.claim(a, "Squid")[0], 200)                   # a changes name: "Inkling" is free again
        self.assertEqual(self.claim(c, "INKLING")[0], 200)
        self.assertEqual(self.claim(c, "x")[0], 400)                       # too short
        self.assertEqual(self.claim(c, "bad<name>")[0], 400)

    def test_lookup_tells_if_the_player_is_online(self):
        a, b, _ = self.pids
        self.claim(a, "Alice")
        self.claim(b, "Bob")
        ta, tb = self.session(a), self.session(b)
        status, answer = self.post("lookup", {"token": ta, "pseudo": "bob"})
        self.assertEqual((status, answer["found"], answer["online"], answer["pid"], answer["pseudo"]),
                         (200, True, False, f"{b:08x}", "Bob"))
        self.post("presence", {"token": tb, "mode": MODE_B, "friends": []})
        status, answer = self.post("lookup", {"token": ta, "pseudo": "BOB"})
        self.assertTrue(answer["online"])
        self.assertEqual(self.post("lookup", {"token": ta, "pseudo": "Nobody"})[1], {"found": False})
        self.assertEqual(self.post("lookup", {"token": "bad", "pseudo": "Bob"})[0], 401)

    def test_presence_is_shared_only_between_mutual_friends(self):
        a, b, c = self.pids
        for pid, name in ((a, "Alice"), (b, "Bob"), (c, "Carol")):
            self.claim(pid, name)
        ta, tb, tc = self.session(a), self.session(b), self.session(c)
        # Alice adds Bob and Carol; Bob adds Alice; Carol adds nobody.
        status, answer = self.post("presence", {"token": ta, "mode": MODE_A, "friends": [f"{b:08x}", f"{c:08x}"]})
        self.assertEqual(answer["friends"], [])                           # nobody else is online yet
        status, answer = self.post("presence", {"token": tb, "mode": MODE_B, "friends": [f"{a:08x}"]})
        self.assertEqual(answer["friends"], [{"pid": f"{a:08x}", "name": "Alice", "mode": MODE_A}])
        self.post("presence", {"token": tc, "mode": "", "friends": []})
        status, answer = self.post("presence", {"token": ta, "mode": MODE_A, "friends": [f"{b:08x}", f"{c:08x}"]})
        self.assertEqual(answer["friends"], [{"pid": f"{b:08x}", "name": "Bob", "mode": MODE_B}])   # Carol did not add Alice back

    def test_presence_expires_and_input_is_checked(self):
        a, b, _ = self.pids
        clock = [1000.0]
        self.server.social.clock = lambda: clock[0]
        self.claim(a, "Alice")
        self.claim(b, "Bob")
        ta, tb = self.session(a), self.session(b)
        self.post("presence", {"token": ta, "mode": MODE_A, "friends": [f"{b:08x}"]})
        self.post("presence", {"token": tb, "mode": MODE_B, "friends": [f"{a:08x}"]})
        self.assertEqual(len(self.post("presence", {"token": ta, "mode": MODE_A, "friends": [f"{b:08x}"]})[1]["friends"]), 1)
        clock[0] += social.ONLINE_WINDOW + 5                               # Bob stops sending heartbeats
        self.assertEqual(self.post("presence", {"token": ta, "mode": MODE_A, "friends": [f"{b:08x}"]})[1]["friends"], [])
        self.assertEqual(self.post("presence", {"token": ta, "mode": "zz", "friends": []})[0], 400)
        self.assertEqual(self.post("presence", {"token": ta, "mode": MODE_A, "friends": ["nothex", "-5", "0"]})[0], 200)
        self.assertEqual(self.post("unknown", {})[0], 404)

    def test_rate_limits(self):
        a, _, _ = self.pids
        token = self.session(a)
        codes = [self.post("lookup", {"token": token, "pseudo": "Nobody"})[0] for _ in range(25)]
        self.assertIn(429, codes)
        self.assertEqual(codes[0], 200)


class SocialRulesTests(unittest.TestCase):
    """Rules that need no HTTP: addresses, one token per player, bans, reserved names, hostile input."""

    def setUp(self):
        self.path = pathlib.Path(tempfile.gettempdir()) / f"social-rules-{secrets.token_hex(3)}.sqlite3"
        self.accounts = account_store.Accounts(self.path)
        self.pids = [0x10000011, 0x10000012]
        for pid in self.pids:
            self.accounts.register(pid, self.password(pid))
        self.social = social.Social(self.accounts)

    def tearDown(self):
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)

    @staticmethod
    def password(pid):
        return f"{pid:08x}" * 2

    def test_ipv6_addresses_of_one_home_share_a_limit_key(self):
        self.assertEqual(social.ip_key("2001:db8:1:2::1"), social.ip_key("2001:db8:1:2:ffff::9"))
        self.assertNotEqual(social.ip_key("2001:db8:1:2::1"), social.ip_key("2001:db8:1:3::1"))
        self.assertEqual(social.ip_key("::ffff:203.0.113.5"), "203.0.113.5")
        self.assertEqual(social.ip_key("not an address"), "unknown")

    def test_a_new_session_replaces_the_old_token(self):
        pid = self.pids[0]
        first = self.social.open_session(pid, self.password(pid))
        second = self.social.open_session(pid, self.password(pid))
        self.assertIsNone(self.social.pid_of(first))
        self.assertEqual(self.social.pid_of(second), pid)
        self.assertEqual(len(self.social._tokens), 1)

    def test_a_banned_player_is_refused_everywhere(self):
        pid = self.pids[0]
        token = self.social.open_session(pid, self.password(pid))
        self.accounts.ban(pid, None, "test")
        for call in (lambda: self.social.open_session(pid, self.password(pid)),
                     lambda: self.social.claim_pseudo(pid, self.password(pid), "Alice"),
                     lambda: self.social.lookup(token, "Alice"),
                     lambda: self.social.heartbeat(token, "", [])):
            with self.assertRaises(social.SocialError) as caught:
                call()
            self.assertEqual(caught.exception.status, 403)

    def test_reserved_and_malformed_pseudos_are_refused(self):
        pid = self.pids[0]
        for name in ("Admin", "NINTENDO", "moderator", " Staff "):
            with self.assertRaises(social.SocialError) as caught:
                self.social.claim_pseudo(pid, self.password(pid), name)
            self.assertEqual(caught.exception.status, 400)
        for name in ("a", "x" * 11, "bad<name>", "new\nline", "'; DROP TABLE accounts;--", "\u202eevil", None, 42, ["a"]):
            with self.assertRaises(social.SocialError):
                self.social.claim_pseudo(pid, self.password(pid), name)
        self.assertEqual(self.accounts.count(), 2)                      # nothing was damaged
        self.assertEqual(self.social.claim_pseudo(pid, self.password(pid), "Alice"), "Alice")

    def test_heartbeat_ignores_hostile_input(self):
        pid = self.pids[0]
        token = self.social.open_session(pid, self.password(pid))
        friends = ["../../etc", "0x41", "-1", "99999999999999999999", "zz", None, 7, [], {}, f"{pid:08x}"] + ["20000001"] * 500
        self.assertEqual(self.social.heartbeat(token, "", friends), [])
        self.assertLessEqual(len(self.social._presence[pid][2]), social.MAX_FRIENDS)
        self.assertNotIn(pid, self.social._presence[pid][2])             # never lists itself
        with self.assertRaises(social.SocialError):
            self.social.heartbeat(token, "00" * 43, [])                  # 43 bytes, not 44
        with self.assertRaises(social.SocialError):
            self.social.heartbeat(token, "0" * 87 + "G", [])


if __name__ == "__main__":
    unittest.main()
