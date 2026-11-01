"""The friends relay between two simulated players and a real local server.
    py -3.12 -m unittest tools.tests.test_friends_relay -v
"""
import importlib.util
import os
import pathlib
import secrets
import tempfile
import unittest

from tools.nexlab import accounts as account_store, register_server

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("friends_relay", ROOT / "scripts" / "friends_relay.py")
friends_relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(friends_relay)


class RelayTests(unittest.TestCase):
    def setUp(self):
        base = pathlib.Path(tempfile.gettempdir())
        self.db = base / f"relay-{secrets.token_hex(3)}.sqlite3"
        self.accounts = account_store.Accounts(self.db)
        self.server = register_server.RegistrationServer(self.accounts, "127.0.0.1", 0).start()
        self.url = f"http://127.0.0.1:{self.server._httpd.server_address[1]}"
        self.dirs, self.relays = [], {}
        for pid, pseudo in ((0x10000001, "Alice"), (0x10000002, "Bob"), (0x10000003, "Carol")):
            password = f"{pid:08x}" * 2
            self.accounts.register(pid, password)
            game = base / f"relay-game-{secrets.token_hex(3)}"
            game.mkdir()
            self.dirs.append(game)
            relay = friends_relay.Relay(self.url, pid, password, pseudo, str(game))
            self.assertEqual(relay.claim_pseudo(), (True, ""))
            self.relays[pseudo] = relay

    def tearDown(self):
        self.server.stop()
        self.accounts.close()
        for suffix in ("", "-wal", "-shm"):
            pathlib.Path(str(self.db) + suffix).unlink(missing_ok=True)

    def command(self, name, command, text):
        relay = self.relays[name]
        pathlib.Path(relay.dir, command).write_text(text)
        relay.step()
        result = pathlib.Path(relay.dir, command.replace(".cmd", ".res")).read_text().strip().split("|")
        return result

    def own_file(self, name, mode):
        relay = self.relays[name]
        pathlib.Path(relay.dir, f"{relay.pid:08x}.txt").write_text(f"pid={relay.pid:08x}\nname={name}\nmode={mode}\n")

    def friend_files(self, name):
        return sorted(p.name for p in pathlib.Path(self.relays[name].dir).glob("*.txt")
                      if p.name != f"{self.relays[name].pid:08x}.txt")

    def test_add_by_pseudo_then_mutual_presence_files(self):
        self.own_file("Alice", "aa" * 44)
        self.own_file("Bob", "bb" * 44)
        self.relays["Bob"].step()                                          # Bob is online
        code, pseudo, text = self.command("Alice", "add.cmd", "bob")
        self.assertEqual((code, pseudo), ("ok", "Bob"))
        self.assertIn("online now", text)
        self.assertEqual(self.friend_files("Alice"), [])                   # Bob has not added Alice yet: nothing shown
        self.assertEqual(self.command("Bob", "add.cmd", "ALICE")[0], "ok")
        self.relays["Alice"].step()
        self.assertEqual(self.friend_files("Alice"), ["10000002.txt"])     # now mutual: Bob appears
        text = pathlib.Path(self.relays["Alice"].dir, "10000002.txt").read_text()
        self.assertIn("name=Bob", text)
        self.assertIn("mode=" + "bb" * 44, text)
        state = pathlib.Path(self.relays["Alice"].dir, "state.res").read_text()
        self.assertIn("friend|10000002|Bob|1", state)
        # Bob quits: his file disappears from Alice's folder once the server forgets him.
        self.relays["Bob"].online = {}
        self.server.social.clock = lambda: __import__("time").time() + 100
        self.relays["Alice"].token = None                                  # a new session, as after a restart
        self.relays["Alice"].step()
        self.assertEqual(self.friend_files("Alice"), [])
        self.assertIn("friend|10000002|Bob|0", pathlib.Path(self.relays["Alice"].dir, "state.res").read_text())

    def test_add_errors_and_remove(self):
        self.assertEqual(self.command("Alice", "add.cmd", "Nobody")[0], "notfound")
        self.assertEqual(self.command("Alice", "add.cmd", "alice")[0], "self")
        self.assertEqual(self.command("Alice", "add.cmd", "x")[0], "invalid")
        self.assertEqual(self.command("Alice", "add.cmd", "Carol")[0], "ok")
        self.assertEqual(self.command("Alice", "add.cmd", "carol")[0], "already")
        self.assertIn("carol", pathlib.Path(self.relays["Alice"].game_dir, "friends.txt").read_text().lower())
        self.assertEqual(self.command("Alice", "remove.cmd", "Carol")[0], "ok")
        self.assertEqual(self.command("Alice", "remove.cmd", "Carol")[0], "notfound")

    def test_server_down_keeps_the_game_working(self):
        relay = self.relays["Alice"]
        relay.base = "http://127.0.0.1:9"                                   # nothing listens there
        relay.token = None
        relay.step()
        state = pathlib.Path(relay.dir, "state.res").read_text()
        self.assertIn("msg|server unreachable", state)

    def test_pseudo_taken_is_reported(self):
        base = pathlib.Path(tempfile.gettempdir())
        game = base / f"relay-game-{secrets.token_hex(3)}"
        game.mkdir()
        self.dirs.append(game)
        self.accounts.register(0x10000009, "abcdefghijklmnop")
        thief = friends_relay.Relay(self.url, 0x10000009, "abcdefghijklmnop", "alice", str(game))
        claimed, reason = thief.claim_pseudo()
        self.assertFalse(claimed)
        self.assertIn("taken", reason)


if __name__ == "__main__":
    unittest.main()
