"""Friends by pseudo: unique pseudos, who is online, and presence for MUTUAL friends.

What the server keeps: a pseudo per account (SQLite, a few bytes) and, in memory only, who sent a
heartbeat in the last 30 seconds together with their game mode and the list of pids they added.
The friend lists themselves live on the players' PCs; the server never stores a friend graph.
A friend's presence is returned only when BOTH players listed each other (consent on both sides).

HTTP, JSON, behind the same HTTPS proxy as the sign-up (paths /social/*). A pid sent for authentication is
"0x" + 8 hex digits, as the sign-up takes it (digits alone are read as decimal); pids in friend lists are plain hex.
  /social/session  {pid, password}                -> {token}            (password checked once, then a token)
  /social/pseudo   {pid, password, pseudo}        -> {pseudo}           (409 when somebody else has it)
  /social/lookup   {token, pseudo}                -> {found, pid, pseudo, online}
  /social/presence {token, mode, friends:[pid]}   -> {friends:[{pid, name, mode}]}  (the heartbeat)
"""
import ipaddress
import re
import secrets
import threading
import time

PSEUDO_RE = re.compile(r"^[A-Za-z0-9 _.\-]{2,10}$")
MODE_RE = re.compile(r"^[0-9a-f]{88}$")           # the game's 0x2C-byte GameMode, hex
ONLINE_WINDOW = 30.0                              # seconds without a heartbeat before a player counts as offline
TOKEN_TTL = 12 * 3600.0
MAX_FRIENDS = 100                                 # what the game's friend list holds
MAX_TOKENS = 20000
# Names nobody may take: they would pass for the project, the server or its staff.
RESERVED = frozenset({"admin", "administrator", "moderator", "mod", "staff", "support", "server", "system", "nintendo",
                      "splatoon", "official", "owner", "root", "console", "player"})


class SocialError(Exception):
    """A request the server refuses; carries the HTTP status and a short message."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status, self.message = status, message


def clean_pseudo(value):
    """The pseudo as stored, or None when it is not one the launcher would accept."""
    if not isinstance(value, str):                 # a JSON number or list is not a name
        return None
    text = value.strip()
    return text if PSEUDO_RE.fullmatch(text) else None


def ip_key(ip):
    """Rate-limit key of a source address: IPv6 is grouped by /64 (one home gets 2**64 addresses)."""
    try:
        address = ipaddress.ip_address(str(ip))
    except ValueError:
        return "unknown"
    if address.version == 6:
        if address.ipv4_mapped is not None:
            return str(address.ipv4_mapped)
        return str(ipaddress.ip_network(f"{address}/64", strict=False).network_address) + "/64"
    return str(address)


class Limiter:
    """At most `limit` events per key and per `window` seconds, table bounded."""

    def __init__(self, limit, window, clock=time.monotonic):
        self.limit, self.window, self.clock = limit, window, clock
        self._events = {}
        self._lock = threading.Lock()

    def allow(self, key):
        now = self.clock()
        with self._lock:
            events = [at for at in self._events.get(key, ()) if now - at < self.window]
            if len(events) >= self.limit:
                self._events[key] = events
                return False
            events.append(now)
            self._events[key] = events
            while len(self._events) > 8192:
                self._events.pop(next(iter(self._events)))
            return True


class Social:
    def __init__(self, accounts, clock=time.time):
        self.accounts = accounts
        self.clock = clock
        self._lock = threading.Lock()
        self._tokens = {}         # token -> (pid, expires)
        self._token_of = {}       # pid -> its one token (a new session replaces the old one)
        self._presence = {}       # pid -> (seen, mode hex, frozenset of friend pids)
        self._next_prune = 0.0
        self.session_rate = Limiter(30, 60.0)       # sign-ins and pseudo claims, per source address
        self.auth_rate = Limiter(300, 60.0)         # the same, for the whole server (each check costs CPU)
        self.lookup_rate = Limiter(20, 60.0)        # searches, per player
        self.lookup_ip_rate = Limiter(60, 60.0)     # searches, per source address
        self.presence_rate = Limiter(40, 60.0)      # per token (a heartbeat every 5 s is 12 per minute)

    def allow_auth(self, ip):
        """False when this address, or the server as a whole, has had too many password checks lately."""
        return self.session_rate.allow(ip_key(ip)) and self.auth_rate.allow("all")

    def _banned(self, pid):
        if self.accounts.is_banned(pid=pid):
            raise SocialError(403, "this player is banned")

    # -- sessions ---------------------------------------------------------------------------
    def pid_of(self, token):
        now = self.clock()
        with self._lock:
            entry = self._tokens.get(str(token))
            if entry is None:
                return None
            if entry[1] < now:
                self._forget_token(str(token))
                return None
            return entry[0]

    def open_session(self, pid, password):
        """A token for a player whose password is right (checked once; the token does the rest)."""
        if not self.accounts.verify(pid, password):
            raise SocialError(401, "unknown player or wrong password")
        self._banned(pid)
        token = secrets.token_hex(16)
        now = self.clock()
        with self._lock:
            if len(self._tokens) >= MAX_TOKENS:
                self._prune(now, force=True)
            self._tokens.pop(self._token_of.get(pid), None)       # one session per player: tokens cannot pile up
            self._tokens[token] = (pid, now + TOKEN_TTL)
            self._token_of[pid] = token
        return token

    # -- pseudos ----------------------------------------------------------------------------
    def claim_pseudo(self, pid, password, pseudo):
        if not self.accounts.verify(pid, password):
            raise SocialError(401, "unknown player or wrong password")
        self._banned(pid)
        clean = clean_pseudo(pseudo)
        if clean is None:
            raise SocialError(400, "2 to 10 characters: letters, digits, space, _ - .")
        if clean.lower() in RESERVED:
            raise SocialError(400, "this pseudo is reserved")
        if not self.accounts.claim_pseudo(pid, clean):
            raise SocialError(409, "this pseudo is taken")
        return clean

    # -- finding a player -------------------------------------------------------------------
    def lookup(self, token, pseudo, ip=""):
        pid = self.pid_of(token)
        if pid is None:
            raise SocialError(401, "session expired")
        self._banned(pid)
        if not self.lookup_rate.allow(pid) or not self.lookup_ip_rate.allow(ip_key(ip)):
            raise SocialError(429, "too many searches, wait a moment")
        clean = clean_pseudo(pseudo)
        if clean is None:
            raise SocialError(400, "2 to 10 characters: letters, digits, space, _ - .")
        found = self.accounts.pid_for_pseudo(clean)
        if found is None:
            return {"found": False}
        return {"found": True, "pid": f"{found:08x}", "pseudo": self.accounts.pseudo_of(found),
                "online": self._online(found), "self": found == pid}

    def _online(self, pid):
        entry = self._presence.get(pid)
        return entry is not None and self.clock() - entry[0] <= ONLINE_WINDOW

    # -- the heartbeat ----------------------------------------------------------------------
    def heartbeat(self, token, mode, friends):
        """Records this player's presence and returns the MUTUAL friends who are online."""
        pid = self.pid_of(token)
        if pid is None:
            raise SocialError(401, "session expired")
        self._banned(pid)
        if not self.presence_rate.allow(pid):
            raise SocialError(429, "too many updates")
        mode = str(mode or "")
        if mode and not MODE_RE.match(mode):
            raise SocialError(400, "invalid game mode")
        wanted = set()
        for value in list(friends or [])[:MAX_FRIENDS]:
            try:
                number = int(str(value), 16)
            except ValueError:
                continue
            if 0 < number <= 0x7FFFFFFF and number != pid:
                wanted.add(number)
        now = self.clock()
        with self._lock:
            self._presence[pid] = (now, mode, frozenset(wanted))
            self._prune(now)
            visible = []
            for friend in wanted:
                entry = self._presence.get(friend)
                if entry is None or now - entry[0] > ONLINE_WINDOW or pid not in entry[2]:
                    continue                  # offline, or the friend did not add this player back
                visible.append((friend, entry[1]))
        names = self.accounts.pseudos_for([friend for friend, _ in visible])
        return [{"pid": f"{friend:08x}", "name": names.get(friend, "Ami"), "mode": mode_hex}
                for friend, mode_hex in visible]

    def _prune(self, now, force=False):
        if not force and now < self._next_prune:
            return
        self._next_prune = now + 30.0
        for pid in [p for p, (seen, _, _) in self._presence.items() if now - seen > ONLINE_WINDOW * 4]:
            self._presence.pop(pid, None)
        for token in [t for t, (_, expires) in self._tokens.items() if expires < now]:
            self._forget_token(token)
        while len(self._tokens) >= MAX_TOKENS:
            self._forget_token(next(iter(self._tokens)))

    def _forget_token(self, token):
        entry = self._tokens.pop(token, None)
        if entry is not None and self._token_of.get(entry[0]) == token:
            del self._token_of[entry[0]]

    def online_count(self):
        now = self.clock()
        return sum(1 for seen, _, _ in self._presence.values() if now - seen <= ONLINE_WINDOW)
