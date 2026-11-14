"""Fixes that follow the outside security audit. Run from the repository root:
    py -3.12 -m unittest tools.tests.test_audit_fixes -v
"""
import json
import pathlib
import secrets
import tempfile
import time
import unittest
import urllib.error
import urllib.request

from nintendo.nex import common, matchmaking
from tools.nexlab import accounts as account_store, matchmake, natcheck_server, register_server, server_config


def temp_db(prefix):
    return pathlib.Path(tempfile.gettempdir()) / f"{prefix}-{secrets.token_hex(3)}.sqlite3"


def remove_db(path):
    for suffix in ("", "-wal", "-shm"):
        pathlib.Path(str(path) + suffix).unlink(missing_ok=True)


class SourceAddressTests(unittest.TestCase):
    def test_ipv6_is_grouped_by_64_and_mapped_v4_is_plain_v4(self):
        self.assertEqual(account_store.check_ip("2001:db8:1:2:aaaa:bbbb:cccc:dddd"),
                         account_store.check_ip("2001:db8:1:2::1"))
        self.assertNotEqual(account_store.check_ip("2001:db8:1:2::1"), account_store.check_ip("2001:db8:1:3::1"))
        self.assertEqual(account_store.check_ip("::ffff:203.0.113.5"), "203.0.113.5")
        self.assertEqual(account_store.check_ip("203.0.113.5"), "203.0.113.5")
        self.assertEqual(account_store.check_ip("not an address"), "unknown")

    def test_rate_limit_uses_wall_clock_and_ignores_rows_from_the_old_monotonic_clock(self):
        path = temp_db("audit")
        try:
            accounts = account_store.Accounts(path, max_attempts=2)
            self.assertLess(abs(accounts.clock() - time.time()), 5)
            # A row written long ago by the monotonic clock (uptime in seconds) must not count.
            accounts._db.execute("INSERT INTO failures (ip, at) VALUES ('203.0.113.9', 90000.0)")
            accounts._db.commit()
            self.assertTrue(accounts.allow("203.0.113.9"))
            accounts.failure("203.0.113.9")
            accounts.failure("203.0.113.9")
            self.assertFalse(accounts.allow("203.0.113.9"))
            accounts.close()
        finally:
            remove_db(path)


class NatCheckTableTests(unittest.TestCase):
    def test_observation_table_is_bounded(self):
        natcheck_server.OBSERVATIONS.clear()
        old = natcheck_server.MAX_OBSERVED_HOSTS
        natcheck_server.MAX_OBSERVED_HOSTS = 50
        try:
            for index in range(200):
                natcheck_server.observe(f"198.51.100.{index}", 40000)
            self.assertLessEqual(len(natcheck_server.OBSERVATIONS), 50)
            self.assertIn("198.51.100.199", natcheck_server.OBSERVATIONS)      # newest kept
        finally:
            natcheck_server.MAX_OBSERVED_HOSTS = old
            natcheck_server.OBSERVATIONS.clear()


class SessionPolicyTests(unittest.TestCase):
    def setUp(self):
        self.mode = matchmake.MODE
        matchmake.configure("public")
        matchmake.SESSIONS = matchmake.Sessions()

    def tearDown(self):
        matchmake.configure(self.mode)
        matchmake.SESSIONS = matchmake.Sessions()

    def make(self, pid):
        return matchmake.SESSIONS.create(matchmaking.MatchmakeSession(), pid)

    def test_public_gids_are_random_unique_and_not_sequential(self):
        gids = []
        for index in range(60):
            sessions = matchmake.Sessions()
            gids.append(sessions.create(matchmaking.MatchmakeSession(), 0x10000001).id)
        self.assertGreater(len(set(gids)), 55)
        self.assertNotEqual(sorted(gids), list(range(min(gids), min(gids) + len(gids))))
        one = matchmake.Sessions()
        ids = {one.create(matchmaking.MatchmakeSession(), 0x10000100 + i).id for i in range(40)}
        self.assertEqual(len(ids), 40)                                    # unique inside one table

    def test_lab_gids_stay_sequential(self):
        matchmake.configure("lab")
        sessions = matchmake.Sessions()
        a = sessions.create(matchmaking.MatchmakeSession(), 1).id
        b = sessions.create(matchmaking.MatchmakeSession(), 2).id
        self.assertEqual(b, a + 1)

    def test_one_player_cannot_own_more_than_three_sessions(self):
        pid = 0x10000001
        for _ in range(matchmake.MAX_SESSIONS_PER_PID):
            self.make(pid)
        with self.assertRaises(common.RMCError):
            self.make(pid)
        self.make(0x10000002)          # somebody else is not affected

    def test_public_join_refuses_closed_and_full_sessions_but_not_members(self):
        session = self.make(0x10000001)
        matchmake.SESSIONS.join(session, 0x10000001, 1)
        stranger = 0x10000009
        matchmake.SESSIONS.check_joinable(session, stranger)            # open and not full: fine
        matchmake.SESSIONS.close(session, "test")
        with self.assertRaises(common.RMCError):
            matchmake.SESSIONS.check_joinable(session, stranger)
        matchmake.SESSIONS.check_joinable(session, 0x10000001)          # a member may come back
        session.open_participation = True
        for index in range(8):
            matchmake.SESSIONS.join(session, 0x10000100 + index, 0)
        with self.assertRaises(common.RMCError):
            matchmake.SESSIONS.check_joinable(session, stranger)

    def test_lab_join_is_not_restricted(self):
        matchmake.configure("lab")
        session = matchmake.SESSIONS.create(matchmaking.MatchmakeSession(), 1)
        matchmake.SESSIONS.close(session, "test")
        matchmake.SESSIONS.check_joinable(session, 99)

    def test_session_key_is_zero_unless_the_server_asks_for_a_random_one(self):
        self.assertEqual(self.make(0x10000001).session_key, bytes(32))
        matchmake.configure("public", random_session_key=True)
        key = self.make(0x10000002).session_key
        self.assertEqual(len(key), 32)
        self.assertNotEqual(key, bytes(32))


class ProbePolicyTests(unittest.TestCase):
    def setUp(self):
        matchmake.configure("public")
        matchmake.SESSIONS = matchmake.Sessions()
        matchmake.STATIONS.clear()

    def tearDown(self):
        matchmake.configure("lab")
        matchmake.SESSIONS = matchmake.Sessions()
        matchmake.STATIONS.clear()

    def test_probe_targets_must_share_a_session_with_the_caller(self):
        a, b, c = 0x10000001, 0x10000002, 0x10000003
        session = matchmake.SESSIONS.create(matchmaking.MatchmakeSession(), a)
        matchmake.SESSIONS.join(session, a, 1)
        matchmake.SESSIONS.join(session, b, 0)
        self.assertTrue(matchmake._share_a_session(a, b))
        self.assertFalse(matchmake._share_a_session(a, c))

    def test_probe_station_must_be_an_address_of_the_caller(self):
        class Client:
            def pid(self):
                return 0x10000001

            def remote_address(self):
                return ("203.0.113.5", 50000)

        matchmake.STATIONS[0x10000001] = [{"address": "192.168.1.20"}]
        ok = matchmake._probe_station_allowed
        self.assertTrue(ok(Client(), {"address": "203.0.113.5"}))        # where the packets come from
        self.assertTrue(ok(Client(), {"address": "192.168.1.20"}))       # an address it registered
        self.assertTrue(ok(Client(), {"address": ""}))
        self.assertFalse(ok(Client(), {"address": "198.51.100.77"}))     # somebody else's address


class RegistrationRangeTests(unittest.TestCase):
    def test_http_registration_refuses_pids_the_game_never_generates(self):
        path = temp_db("audit-reg")
        accounts = account_store.Accounts(path)
        server = register_server.RegistrationServer(accounts, "127.0.0.1", 0).start()
        try:
            port = server._httpd.server_address[1]

            def post(pid):
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/register", headers={"Content-Type": "application/json"},
                    data=json.dumps({"pid": f"0x{pid:08x}", "password": f"{pid:08x}" * 2}).encode())
                try:
                    with urllib.request.urlopen(request, timeout=5) as reply:
                        return reply.status
                except urllib.error.HTTPError as error:
                    return error.code
            self.assertEqual(post(2), 400)
            self.assertEqual(post(0x0FFFFFFF), 400)
            self.assertEqual(post(0x10000000), 200)
        finally:
            server.stop()
            accounts.close()
            remove_db(path)


class LimiterTableTests(unittest.TestCase):
    def test_rate_limiter_table_stays_bounded_under_a_flood_of_new_keys(self):
        from tools.nexlab import nexserver
        limiter = nexserver.RateLimiter(5, 60.0)
        for index in range(20000):
            self.assertTrue(limiter.allow(f"198.51.{index // 250}.{index % 250}"))
        self.assertLessEqual(len(limiter._events), 8192 + 1)
        for _ in range(5):
            limiter.allow("203.0.113.1")
        self.assertFalse(limiter.allow("203.0.113.1"))          # a real offender is still limited


class AccountErasureTests(unittest.TestCase):
    def test_an_account_can_be_erased(self):
        path = temp_db("audit-del")
        accounts = account_store.Accounts(path)
        try:
            accounts.register(0x10000042, "abcdefghijklmnop")
            self.assertEqual(accounts.count(), 1)
            self.assertTrue(accounts.delete("0x10000042"))
            self.assertEqual(accounts.count(), 0)
            self.assertFalse(accounts.delete("0x10000042"))
        finally:
            accounts.close()
            remove_db(path)


class ConfigTests(unittest.TestCase):
    def test_random_session_key_option(self):
        self.assertFalse(server_config.ServerConfig().random_session_key)
        self.assertTrue(server_config.ServerConfig(random_session_key=True).random_session_key)




class NatCheckRateTests(unittest.TestCase):
    def test_one_source_cannot_use_more_than_its_probe_budget(self):
        natcheck_server._RATE.clear()
        limit, window = natcheck_server.RATE_LIMIT
        answers = [natcheck_server.rate_ok("198.51.100.9", now=1000.0 + index * 0.01) for index in range(limit + 20)]
        self.assertEqual(answers.count(True), limit)
        self.assertTrue(natcheck_server.rate_ok("198.51.100.10", now=1001.0))        # another source is not affected
        self.assertTrue(natcheck_server.rate_ok("198.51.100.9", now=1000.0 + window + 1))   # a new window starts
        natcheck_server._RATE.clear()


if __name__ == "__main__":
    unittest.main()
