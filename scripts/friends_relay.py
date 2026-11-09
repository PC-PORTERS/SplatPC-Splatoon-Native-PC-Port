"""Friends relay: runs next to the game and links its friend list to the server (Python 3, standard library only).

The game reads friends from files in <Game>\\friends (one <pid>.txt per ONLINE friend: pid, name, mode) and
writes its own <pid>.txt (the game mode, which tells friends what room it is in). This program:
  - sends the player's own file to the server every few seconds (the heartbeat) with the list of friends
    the player added, and writes back one file per friend who is online AND added this player too;
  - answers the in-game Friends window through small files in the same folder:
        add.cmd     a pseudo to add          -> add.res   (one line: "<code>|<pseudo>|<text>")
        remove.cmd  a pseudo or pid to drop  -> remove.res
        accept.cmd  the pid (or pseudo) of a friend request received -> accept.res
        decline.cmd the same, to refuse it                           -> decline.res
        state.res   "friend|<pid>|<pseudo>|<online 0/1>" lines, "request|<pid>|<pseudo>" lines (requests received,
                    not answered yet) and a "msg|<text>" line (rewritten each cycle)
  - keeps the player's list in <Game>\\friends.txt ("<pid> <pseudo>" per line); the server stores no list.
Nothing here ever leaves the PC except the heartbeat, the pseudo search, the pseudo claim and the friend
requests (send, answer, cancel), over HTTPS.
"""
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request


def game_integrity():
    """{'version', 'code'}: this install's version and the hash of its code (integrity.py, next to this file)."""
    try:
        try:
            import integrity
        except ImportError:                       # loaded by path (tests): the folder is not on sys.path
            import importlib.util
            spec = importlib.util.spec_from_file_location("integrity", os.path.join(os.path.dirname(os.path.abspath(__file__)), "integrity.py"))
            integrity = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(integrity)
        return integrity.attestation()
    except Exception:                             # a broken install simply cannot prove itself
        return {"version": "", "code": ""}


PSEUDO_RE = re.compile(r"^[A-Za-z0-9 _.\-]{2,10}$")
ZERO_MODE = "00" * 44
INTERVAL = 5.0


def read_kv(path):
    values = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                key, sep, value = line.rstrip("\r\n").partition("=")
                if sep:
                    values[key.strip().lower()] = value.strip()
    except OSError:
        pass
    return values


def write_atomic(path, text):
    temporary = f"{path}.{os.getpid()}.tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    os.replace(temporary, path)


class Relay:
    def __init__(self, base_url, pid, password, pseudo, game_dir, timeout=6.0, client=None):
        self.base = base_url.rstrip("/")
        self.pid, self.password, self.pseudo = pid, password, pseudo
        self.game_dir = game_dir
        self.dir = os.path.join(game_dir, "friends")
        self.list_path = os.path.join(game_dir, "friends.txt")
        self.timeout = timeout
        self.client = client or {}              # the integrity proof sent with each sign-in (see integrity.py)
        self.token = None
        self.message = ""
        self.online = {}                        # pid hex -> (name, mode) from the last good heartbeat
        self.requests = []                      # [(pid hex, pseudo)] friend requests received, not answered yet
        os.makedirs(self.dir, exist_ok=True)

    # -- network ------------------------------------------------------------------------------
    def call(self, route, body):
        request = urllib.request.Request(f"{self.base}/social/{route}", data=json.dumps(body).encode(),
                                         headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as reply:
                return reply.status, json.loads(reply.read() or b"{}")
        except urllib.error.HTTPError as error:
            try:
                return error.code, json.loads(error.read() or b"{}")
            except ValueError:
                return error.code, {}
        except (urllib.error.URLError, OSError, ValueError):
            return 0, {}

    def ensure_session(self):
        if self.token:
            return True
        status, answer = self.call("session", {"pid": f"0x{self.pid:08x}", "password": self.password, "client": self.client})
        if status == 200 and answer.get("token"):
            self.token = answer["token"]
            return True
        if status == 426:                       # the server only takes official, up-to-date game files
            self.message = str(answer.get("error") or "update the game")[:120]
        else:
            self.message = "server unreachable" if status == 0 else "sign-in refused"
        return False

    def claim_pseudo(self):
        """(True, '') when this pseudo is the player's; (False, reason) otherwise."""
        status, answer = self.call("pseudo", {"pid": f"0x{self.pid:08x}", "password": self.password, "pseudo": self.pseudo})
        if status == 200:
            return True, ""
        return False, answer.get("error", "server unreachable" if status == 0 else f"error {status}")

    # -- the local list -------------------------------------------------------------------------
    def load_list(self):
        friends = {}
        try:
            with open(self.list_path, encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    parts = line.strip().split(" ", 1)
                    if len(parts) == 2 and re.fullmatch(r"[0-9a-f]{8}", parts[0]):
                        friends[parts[0]] = parts[1].strip()
        except OSError:
            pass
        return friends

    def save_list(self, friends):
        write_atomic(self.list_path, "".join(f"{pid} {name}\n" for pid, name in sorted(friends.items())))

    # -- commands from the game window ------------------------------------------------------------
    def take_command(self, name):
        path = os.path.join(self.dir, name)
        if not os.path.exists(path):
            return None
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read().strip()
        finally:
            try:
                os.remove(path)
            except OSError:
                pass
        return text

    def answer(self, name, code, pseudo, text):
        write_atomic(os.path.join(self.dir, name), f"{code}|{pseudo}|{text}\n")

    def handle_add(self, text, friends):
        pseudo = text.strip()
        if not PSEUDO_RE.fullmatch(pseudo):
            return self.answer("add.res", "invalid", pseudo, "2 to 10 characters: letters, digits, space, _ - .")
        if not self.ensure_session():
            return self.answer("add.res", "error", pseudo, self.message)
        status, found = self.call("lookup", {"token": self.token, "pseudo": pseudo})
        if status == 401:
            self.token = None
            return self.answer("add.res", "error", pseudo, "session expired, try again")
        if status == 429:
            return self.answer("add.res", "error", pseudo, "too many searches, wait a moment")
        if status != 200:
            return self.answer("add.res", "error", pseudo, "server unreachable" if status == 0 else f"error {status}")
        if not found.get("found"):
            return self.answer("add.res", "notfound", pseudo, "no player has this pseudo")
        if found.get("self"):
            return self.answer("add.res", "self", found["pseudo"], "this is you")
        pid = found["pid"]
        if pid in friends:
            return self.answer("add.res", "already", found["pseudo"], "already in your list")
        if len(friends) >= 100:
            return self.answer("add.res", "full", found["pseudo"], "the list holds 100 friends")
        result, problem = self.request_friend(pid)
        if result == "error":
            return self.answer("add.res", "error", found["pseudo"], problem)
        friends[pid] = found["pseudo"]
        self.save_list(friends)
        state = "online now" if found.get("online") else "not online now"
        follow_up = {"sent": "request sent, they must accept", "friends": "they had asked you too: you are friends"}
        self.answer("add.res", "ok", found["pseudo"], f"added ({state}); {follow_up.get(result, 'they must add you too')}")

    def request_friend(self, pid):
        """('sent' | 'friends', '') when the server took the request, ('old', '') for a server without friend
        requests (the list alone, as before), ('error', text) when the player must not be added."""
        status, answer = self.call("request", {"token": self.token, "pid": pid})
        if status == 200 and answer.get("state") in ("sent", "friends"):
            return answer["state"], ""
        if status == 401:
            self.token = None
            return "error", "session expired, try again"
        if status == 429:
            return "error", str(answer.get("error") or "too many requests, wait a moment")[:80]
        return "old", ""

    def handle_remove(self, text, friends):
        wanted = text.strip().lower()
        for pid, name in list(friends.items()):
            if wanted in (pid, name.lower()):
                del friends[pid]
                self.save_list(friends)
                if self.ensure_session():                   # withdraws a request still waiting; a server without them says 404
                    self.call("cancel", {"token": self.token, "pid": pid})
                return self.answer("remove.res", "ok", name, "removed")
        self.answer("remove.res", "notfound", wanted, "not in your list")

    def handle_answer(self, text, friends, accept):
        result = "accept.res" if accept else "decline.res"
        wanted = text.strip().lower()
        match = next(((pid, name) for pid, name in self.requests if wanted in (pid, name.lower())), None)
        if match is None:
            return self.answer(result, "notfound", wanted, "no such request")
        pid, name = match
        if accept and pid not in friends and len(friends) >= 100:
            return self.answer(result, "full", name, "the list holds 100 friends")
        if not self.ensure_session():
            return self.answer(result, "error", name, self.message)
        status, _ = self.call("answer", {"token": self.token, "pid": pid, "accept": accept})
        if status == 401:
            self.token = None
            return self.answer(result, "error", name, "session expired, try again")
        if status not in (200, 404):
            return self.answer(result, "error", name, "server unreachable" if status == 0 else f"error {status}")
        self.requests.remove(match)
        if status == 404:                                   # the sender withdrew it meanwhile
            return self.answer(result, "gone", name, "this request is gone")
        if accept:
            friends[pid] = name
            self.save_list(friends)
        self.answer(result, "ok", name, "accepted, you are friends now" if accept else "declined")

    def read_requests(self, answer, friends):
        """The requests received, as the server lists them; one from a player already in the list is accepted at once."""
        received = []
        for item in answer.get("requests", []) or []:
            pid = str(item.get("pid", ""))
            if not re.fullmatch(r"[0-9a-f]{8}", pid):
                continue
            name = re.sub(r"[^A-Za-z0-9 _.\-]", "?", str(item.get("name", "Ami")))[:10]
            if pid in friends:
                self.call("answer", {"token": self.token, "pid": pid, "accept": True})
            else:
                received.append((pid, name))
        self.requests = received

    # -- one cycle --------------------------------------------------------------------------------
    def step(self):
        friends = self.load_list()
        command = self.take_command("add.cmd")
        if command is not None:
            self.handle_add(command, friends)
        command = self.take_command("remove.cmd")
        if command is not None:
            self.handle_remove(command, friends)
        for name, accept in (("accept.cmd", True), ("decline.cmd", False)):
            command = self.take_command(name)
            if command is not None:
                self.handle_answer(command, friends, accept)
        own = read_kv(os.path.join(self.dir, f"{self.pid:08x}.txt"))
        mode = own.get("mode", "")
        if not re.fullmatch(r"[0-9a-f]{88}", mode):
            mode = ""
        reachable = False
        if self.ensure_session():
            status, answer = self.call("presence", {"token": self.token, "mode": mode, "friends": list(friends)})
            if status == 401:                   # the server forgot the session (a restart): sign in again at once, which
                self.token = None               # also sends the integrity proof back before the game logs in again
                if self.ensure_session():
                    status, answer = self.call("presence", {"token": self.token, "mode": mode, "friends": list(friends)})
            if status == 200:
                reachable = True
                self.message = ""
                self.online = {item["pid"]: (item.get("name", "Ami"), item.get("mode") or ZERO_MODE)
                               for item in answer.get("friends", []) if re.fullmatch(r"[0-9a-f]{8}", str(item.get("pid", "")))}
                self.read_requests(answer, friends)
            elif status == 401:
                self.token = None
                self.message = "session expired"
            else:
                self.message = "server unreachable" if status == 0 else f"error {status}"
        if not reachable:
            self.online = {}
            self.requests = []
        self.write_friend_files()
        lines = [f"friend|{pid}|{name}|{1 if pid in self.online else 0}" for pid, name in sorted(friends.items(), key=lambda kv: kv[1].lower())]
        lines += [f"request|{pid}|{name}" for pid, name in self.requests]
        lines.append(f"msg|{self.message}")
        write_atomic(os.path.join(self.dir, "state.res"), "\n".join(lines) + "\n")

    def write_friend_files(self):
        """One <pid>.txt per friend who is online and mutual; the files of everybody else are removed."""
        keep = set()
        for pid, (name, mode) in self.online.items():
            keep.add(f"{pid}.txt")
            clean_name = re.sub(r"[^A-Za-z0-9 _.\-]", "?", name)[:10]
            write_atomic(os.path.join(self.dir, f"{pid}.txt"), f"pid={pid}\nname={clean_name}\nmode={mode}\n")
        own = f"{self.pid:08x}.txt"
        for entry in os.listdir(self.dir):
            if entry.endswith(".txt") and re.fullmatch(r"[0-9a-f]{8}\.txt", entry) and entry not in keep and entry != own:
                try:
                    os.remove(os.path.join(self.dir, entry))
                except OSError:
                    pass


def load_identity(game_dir):
    values = read_kv(os.path.join(game_dir, "online_identity.ini"))
    try:
        return int(values["pid"], 16), values["password"]
    except (KeyError, ValueError):
        return None, None


def process_alive(pid):
    if pid <= 0:
        return True
    if os.name == "nt":
        import ctypes
        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)      # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(handle)
        return code.value == 259                                              # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def main(argv):
    import argparse
    parser = argparse.ArgumentParser(description="Friends relay (see the top of this file)")
    parser.add_argument("--game-dir", required=True, help="the Game folder (settings.ini, online_identity.ini)")
    parser.add_argument("--server", required=True, help="base URL, for example https://splatoonline.duckdns.org")
    parser.add_argument("--watch-pid", type=int, default=0, help="stop when this process (the game) has ended")
    args = parser.parse_args(argv)
    pid, password = load_identity(args.game_dir)
    if pid is None:
        sys.exit("no online identity yet")
    pseudo = read_kv(os.path.join(args.game_dir, "settings.ini")).get("pseudo", "")
    relay = Relay(args.server, pid, password, pseudo, args.game_dir, client=game_integrity())
    if pseudo:
        claimed, reason = relay.claim_pseudo()
        if not claimed:
            relay.message = f"pseudo: {reason}"
    while process_alive(args.watch_pid):
        try:
            relay.step()
        except Exception as error:               # never stop on a bad cycle
            relay.message = f"relay error: {type(error).__name__}"
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main(sys.argv[1:])
