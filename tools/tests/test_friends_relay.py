"""The friends relay between two simulated players and a real local server.
    py -3.12 -m unittest tools.tests.test_friends_relay -v
"""
import importlib.util
import json
import os
import pathlib
import secrets
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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

    def test_forgotten_session_is_reopened_in_the_same_cycle(self):
        self.own_file("Alice", "aa" * 44)
        self.own_file("Bob", "bb" * 44)
        self.assertEqual(self.command("Alice", "add.cmd", "Bob")[0], "ok")
        self.assertEqual(self.command("Bob", "add.cmd", "Alice")[0], "ok")
        relay = self.relays["Alice"]
        relay.token = "0" * 32                                              # unknown to the server, as after a restart
        relay.step()
        self.assertNotEqual(relay.token, "0" * 32)
        self.assertEqual(self.friend_files("Alice"), ["10000002.txt"])      # one cycle: no 'session expired' gap
        self.assertIn("msg|\n", pathlib.Path(relay.dir, "state.res").read_text())

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


class FakeSocialServer:
    """A server that knows friend requests (tools.nexlab does not): a few players, requests kept in memory."""

    PLAYERS = {"alice": "10000001", "bob": "10000002"}

    def __init__(self):
        self.calls = []                      # (route, body) as the relay sent them
        self.pending = {}                    # to pid -> [(from pid, pseudo)]
        self.request_reply = (200, {"state": "sent"})
        self.answer_status = 200
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                route = self.path.rsplit("/", 1)[-1]
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.calls.append((route, body))
                status, reply = owner.reply(route, body)
                data = json.dumps(reply).encode()
                self.send_response(status)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def reply(self, route, body):
        if route == "session":
            return 200, {"token": "t-" + body["pid"].removeprefix("0x")}
        if route == "lookup":
            pid = self.PLAYERS.get(body["pseudo"].lower())
            return 200, {"found": True, "pid": pid, "pseudo": body["pseudo"], "online": False,
                         "self": False} if pid else {"found": False}
        if route == "presence":
            me = body["token"][-8:]
            return 200, {"friends": [], "requests": [{"pid": pid, "name": name} for pid, name in self.pending.get(me, [])]}
        if route == "request":
            return self.request_reply
        if route == "answer":
            return self.answer_status, {"ok": True}
        return 200, {"ok": True}

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class FriendRequestTests(unittest.TestCase):
    def setUp(self):
        self.server = FakeSocialServer()
        self.folder = tempfile.TemporaryDirectory()
        self.relay = friends_relay.Relay(self.server.url, 0x10000001, "p" * 16, "Alice", self.folder.name)

    def tearDown(self):
        self.server.stop()
        self.folder.cleanup()

    def command(self, command, text):
        pathlib.Path(self.relay.dir, command).write_text(text)
        self.relay.step()
        return pathlib.Path(self.relay.dir, command.replace(".cmd", ".res")).read_text().strip().split("|")

    def state(self):
        return pathlib.Path(self.relay.dir, "state.res").read_text().splitlines()

    def routes(self):
        return [route for route, _ in self.server.calls]

    def test_add_sends_a_request(self):
        code, pseudo, text = self.command("add.cmd", "Bob")
        self.assertEqual((code, pseudo), ("ok", "Bob"))
        self.assertIn("request sent", text)
        self.assertIn(("request", {"token": "t-10000001", "pid": "10000002"}), self.server.calls)
        self.assertIn("10000002 Bob", pathlib.Path(self.folder.name, "friends.txt").read_text())

    def test_crossed_request_says_so(self):
        self.server.request_reply = (200, {"state": "friends"})
        self.assertIn("you are friends", self.command("add.cmd", "Bob")[2])

    def test_a_full_box_or_limit_refuses_the_add(self):
        self.server.request_reply = (429, {"error": "too many pending requests (20), wait for answers"})
        code, _, text = self.command("add.cmd", "Bob")
        self.assertEqual(code, "error")
        self.assertIn("too many pending", text)
        self.assertFalse(pathlib.Path(self.folder.name, "friends.txt").exists())   # not listed: nothing to cancel later

    def test_server_without_requests_keeps_the_old_behaviour(self):
        self.server.request_reply = (404, {"error": "not found"})
        code, _, text = self.command("add.cmd", "Bob")
        self.assertEqual(code, "ok")
        self.assertIn("they must add you too", text)

    def test_requests_received_are_listed_and_accepted(self):
        self.server.pending["10000001"] = [("10000002", "Bob")]
        self.relay.step()
        self.assertIn("request|10000002|Bob", self.state())
        code, pseudo, text = self.command("accept.cmd", "10000002")
        self.assertEqual((code, pseudo), ("ok", "Bob"))
        self.assertIn(("answer", {"token": "t-10000001", "pid": "10000002", "accept": True}), self.server.calls)
        self.assertIn("10000002 Bob", pathlib.Path(self.folder.name, "friends.txt").read_text())
        self.assertNotIn("request|10000002|Bob", self.state())     # answered: gone from the list at once

    def test_decline_does_not_list_the_player(self):
        self.server.pending["10000001"] = [("10000002", "Bob")]
        self.relay.step()
        self.assertEqual(self.command("decline.cmd", "bob")[0], "ok")
        self.assertIn(("answer", {"token": "t-10000001", "pid": "10000002", "accept": False}), self.server.calls)
        self.assertFalse(pathlib.Path(self.folder.name, "friends.txt").exists())

    def test_answer_errors(self):
        self.assertEqual(self.command("accept.cmd", "10000002")[0], "notfound")
        self.server.pending["10000001"] = [("10000002", "Bob")]
        self.relay.step()
        self.server.answer_status = 404                          # withdrawn by the sender meanwhile
        self.assertEqual(self.command("accept.cmd", "10000002")[0], "gone")
        self.assertFalse(pathlib.Path(self.folder.name, "friends.txt").exists())
        self.server.pending["10000001"] = [("10000002", "Bob")]
        self.relay.step()
        self.server.answer_status = 500
        self.assertEqual(self.command("accept.cmd", "10000002")[0], "error")
        self.assertIn("request|10000002|Bob", self.state())      # still waiting, nothing was lost

    def test_a_request_from_a_listed_player_is_accepted_quietly(self):
        self.command("add.cmd", "Bob")
        self.server.pending["10000001"] = [("10000002", "Bob")]
        self.relay.step()
        self.assertNotIn("request|10000002|Bob", self.state())
        self.assertIn(("answer", {"token": "t-10000001", "pid": "10000002", "accept": True}), self.server.calls)

    def test_remove_cancels_the_pending_request(self):
        self.command("add.cmd", "Bob")
        self.assertEqual(self.command("remove.cmd", "Bob")[0], "ok")
        self.assertIn(("cancel", {"token": "t-10000001", "pid": "10000002"}), self.server.calls)

    def test_requests_disappear_when_the_server_does(self):
        self.server.pending["10000001"] = [("10000002", "Bob")]
        self.relay.step()
        self.relay.base = "http://127.0.0.1:9"
        self.relay.step()
        self.assertFalse([line for line in self.state() if line.startswith("request|")])


if __name__ == "__main__":
    unittest.main()
