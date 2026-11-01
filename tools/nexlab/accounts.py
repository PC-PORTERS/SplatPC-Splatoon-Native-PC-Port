"""Account store for the public NEX server.

A public player is identified by the 31-bit PID its port generated. The credential is the
16-character NEX password, but the game never transmits it: the ACT token it sends in
LoginEx is only 'rbrew1:<pid>:<game server>' (measured in build/diagnostics), the password
stays in the token buffer that the game keeps for Kerberos. A stranger therefore registers
once, through the small HTTP endpoint (register_server.py) or this module's CLI, and the
server keeps only the Kerberos key derived from that password.

sqlite3 from the standard library, no plaintext password, no secret in any log line.
"""
import getpass
import hmac
import ipaddress
import os
import pathlib

import sqlite3
import sys
import threading
import time

from nintendo.nex import kerberos


class AccountError(Exception):
    """Base class: the message is safe to show to a player."""


class InvalidIdentity(AccountError):
    pass


class AlreadyRegistered(AccountError):
    pass


class TooManyAttempts(AccountError):
    pass


def derive_key(password, pid):
    """The Kerberos key the NEX ticket is encrypted with (same derivation as the game)."""
    return kerberos.KeyDerivationOld().derive_key(password.encode("ascii"), pid)


def parse_pid(value):
    """Decimal or 0x-prefixed hexadecimal, 1..0x7fffffff (the port's own range)."""
    text = str(value).strip().lower()
    try:
        if text.startswith("0x"):
            number = int(text[2:], 16)
        else:
            number = int(text)
    except ValueError:
        try:
            number = int(text, 16)          # '5eea2491', as online_identity.ini writes it
        except ValueError:
            raise InvalidIdentity("pid must be decimal or hexadecimal") from None
    if not 0 < number <= 0x7FFFFFFF:
        raise InvalidIdentity("pid must be in 1..0x7fffffff")
    return number


def check_password(password):
    if len(password) != 16 or not all(c.isascii() and c.isalnum() for c in password):
        raise InvalidIdentity("the NEX password must be 16 letters or digits")
    return password


def check_ip(value):
    """Canonical key of a source address. IPv6 is grouped by /64: one home or phone gets a whole /64,
    so a per-address limit on single IPv6 addresses would never bind."""
    try:
        ip = ipaddress.ip_address(str(value).strip())
    except ValueError:
        return "unknown"
    if ip.version == 6:
        if ip.ipv4_mapped is not None:
            return str(ip.ipv4_mapped)
        return str(ipaddress.ip_network(f"{ip}/64", strict=False).network_address) + "/64"
    return str(ip)


class Accounts:
    """SQLite store: one row per pid, holding the derived key, never the password."""

    def __init__(self, path="nexlab-accounts.sqlite3", max_attempts=10, window=300.0,
                 clock=time.time):
        self.path = pathlib.Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.max_attempts = int(max_attempts)
        self.window = float(window)
        self.clock = clock
        self._lock = threading.Lock()
        self._db = sqlite3.connect(str(self.path), check_same_thread=False)
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA synchronous=NORMAL")      # WAL + NORMAL: no fsync per login, still no corruption
        self._db.execute("""CREATE TABLE IF NOT EXISTS accounts (
                                pid INTEGER PRIMARY KEY,
                                key BLOB NOT NULL,
                                created REAL NOT NULL,
                                last_seen REAL)""")
        self._db.execute("""CREATE TABLE IF NOT EXISTS failures (
                                ip TEXT NOT NULL,
                                at REAL NOT NULL)""")
        self._db.execute("CREATE INDEX IF NOT EXISTS failures_ip_at ON failures(ip, at)")
        # One pseudo per account, unique ignoring case (friends are found by pseudo).
        self._db.execute("""CREATE TABLE IF NOT EXISTS pseudos (
                                pid INTEGER PRIMARY KEY,
                                pseudo TEXT NOT NULL,
                                pkey TEXT NOT NULL UNIQUE)""")
        # Operator bans (chantier 4): either a pid, an IP, or both, with a reason.
        self._db.execute("""CREATE TABLE IF NOT EXISTS bans (
                                pid INTEGER,
                                ip TEXT,
                                reason TEXT,
                                created REAL NOT NULL,
                                UNIQUE(pid, ip))""")
        self._db.commit()
        self.restrict_permissions()

    def restrict_permissions(self):
        """The file holds Kerberos keys, i.e. credentials (S-08): keep it private."""
        targets = [self.path,
                   pathlib.Path(str(self.path) + "-wal"),
                   pathlib.Path(str(self.path) + "-shm")]
        for path in targets:
            try:
                if path.exists():
                    os.chmod(path, 0o600)
            except OSError:
                pass               # Windows, or a filesystem without POSIX modes

    def close(self):
        with self._lock:
            self._db.close()

    # -- operator backups (chantier 4) -------------------------------------
    def backup(self, destination):
        target = sqlite3.connect(str(destination))
        try:
            with self._lock:
                self._db.backup(target)
        finally:
            target.close()
        try:
            os.chmod(destination, 0o600)
        except OSError:
            pass
        return pathlib.Path(destination)

    def restore(self, source):
        """Replace this database with a backup (stop the server first). Reopens on exit."""
        source_path = pathlib.Path(source)
        if not source_path.is_file():
            raise AccountError(f"no such backup: {source_path}")
        with self._lock:
            self._db.close()
            try:
                for suffix in ("-wal", "-shm"):
                    pathlib.Path(str(self.path) + suffix).unlink(missing_ok=True)
                backup = sqlite3.connect(str(source_path))
                try:
                    target = sqlite3.connect(str(self.path))
                    try:
                        backup.backup(target)
                    finally:
                        target.close()
                finally:
                    backup.close()
            finally:
                self._db = sqlite3.connect(str(self.path), check_same_thread=False)
                self._db.execute("PRAGMA journal_mode=WAL")
        self.restrict_permissions()
        return self

    def count(self):
        with self._lock:
            return self._db.execute("SELECT COUNT(*) FROM accounts").fetchone()[0]

    def pids(self):
        with self._lock:
            return [row[0] for row in self._db.execute("SELECT pid FROM accounts ORDER BY pid")]

    def key(self, pid):
        with self._lock:
            row = self._db.execute("SELECT key FROM accounts WHERE pid = ?", (pid,)).fetchone()
        return bytes(row[0]) if row else None

    def touch(self, pid):
        with self._lock:
            self._db.execute("UPDATE accounts SET last_seen = ? WHERE pid = ?", (time.time(), pid))
            self._db.commit()

    def delete(self, pid):
        """Erase one account (a player's request to be forgotten); True when it existed."""
        pid = parse_pid(pid)
        with self._lock:
            removed = self._db.execute("DELETE FROM accounts WHERE pid = ?", (pid,)).rowcount
            self._db.commit()
        return removed > 0

    def claim_pseudo(self, pid, pseudo):
        """True when this account now owns `pseudo` (also when it already did); False when another account has it."""
        pid = parse_pid(pid)
        pkey = str(pseudo).strip().lower()
        with self._lock:
            row = self._db.execute("SELECT pid FROM pseudos WHERE pkey = ?", (pkey,)).fetchone()
            if row is not None and row[0] != pid:
                return False
            try:
                self._db.execute("INSERT INTO pseudos (pid, pseudo, pkey) VALUES (?, ?, ?) "
                                 "ON CONFLICT(pid) DO UPDATE SET pseudo = excluded.pseudo, pkey = excluded.pkey",
                                 (pid, str(pseudo).strip(), pkey))
                self._db.commit()
            except sqlite3.IntegrityError:          # somebody else took it between the check and the write
                self._db.rollback()
                return False
        return True

    def pseudo_of(self, pid):
        with self._lock:
            row = self._db.execute("SELECT pseudo FROM pseudos WHERE pid = ?", (parse_pid(pid),)).fetchone()
        return row[0] if row else None

    def pid_for_pseudo(self, pseudo):
        with self._lock:
            row = self._db.execute("SELECT pid FROM pseudos WHERE pkey = ?", (str(pseudo).strip().lower(),)).fetchone()
        return row[0] if row else None

    def pseudos_for(self, pids):
        pids = [int(p) for p in pids]
        if not pids:
            return {}
        marks = ",".join("?" * len(pids))
        with self._lock:
            rows = self._db.execute(f"SELECT pid, pseudo FROM pseudos WHERE pid IN ({marks})", pids).fetchall()
        return {pid: pseudo for pid, pseudo in rows}

    def register(self, pid, password):
        """"created" or "exists"; AlreadyRegistered when the pid belongs to another key."""
        pid = parse_pid(pid)
        key = derive_key(check_password(password), pid)
        with self._lock:
            row = self._db.execute("SELECT key FROM accounts WHERE pid = ?", (pid,)).fetchone()
            if row is not None:
                if hmac.compare_digest(bytes(row[0]), key):
                    return "exists"
                raise AlreadyRegistered("this pid is already registered with another password")
            self._db.execute("INSERT INTO accounts (pid, key, created) VALUES (?, ?, ?)",
                             (pid, key, time.time()))
            self._db.commit()
        self.restrict_permissions()
        return "created"

    def verify(self, pid, password):
        stored = self.key(pid)
        if stored is None:
            return False
        try:
            candidate = derive_key(password, pid)
        except (UnicodeEncodeError, AttributeError):
            return False
        return hmac.compare_digest(stored, candidate)

    # -- rate limiting, per source IP --------------------------------------
    def allow(self, ip):
        """False when this IP tried too many unknown pids inside the window."""
        ip = check_ip(ip)
        since = self.clock() - self.window
        with self._lock:
            rows = self._db.execute("SELECT COUNT(*) FROM failures WHERE ip = ? AND at >= ?",
                                    (ip, since)).fetchone()[0]
        return rows < self.max_attempts

    def failure(self, ip):
        ip = check_ip(ip)
        now = self.clock()
        with self._lock:
            self._db.execute("DELETE FROM failures WHERE at < ?", (now - self.window,))
            self._db.execute("INSERT INTO failures (ip, at) VALUES (?, ?)", (ip, now))
            self._db.commit()

    def attempt(self, ip):
        """Count a registration attempt even when it succeeds (S-07: bounded account creation)."""
        self.failure(ip)

    # -- operator bans (chantier 4) ----------------------------------------
    def ban(self, pid=None, ip=None, reason=""):
        if pid is None and ip is None:
            raise InvalidIdentity("a ban needs a pid, an ip, or both")
        pid = parse_pid(pid) if pid is not None else None
        ip = check_ip(ip) if ip is not None else None
        with self._lock:
            self._db.execute("INSERT OR REPLACE INTO bans (pid, ip, reason, created) "
                             "VALUES (?, ?, ?, ?)", (pid, ip, str(reason)[:200], time.time()))
            self._db.commit()
        return pid, ip

    def unban(self, pid=None, ip=None):
        if pid is None and ip is None:
            raise InvalidIdentity("an unban needs a pid, an ip, or both")
        pid = parse_pid(pid) if pid is not None else None
        ip = check_ip(ip) if ip is not None else None
        with self._lock:
            if pid is not None and ip is not None:
                self._db.execute("DELETE FROM bans WHERE pid = ? AND ip = ?", (pid, ip))
            elif pid is not None:
                self._db.execute("DELETE FROM bans WHERE pid = ? AND ip IS NULL", (pid,))
            else:
                self._db.execute("DELETE FROM bans WHERE ip = ? AND pid IS NULL", (ip,))
            self._db.commit()

    def bans(self):
        with self._lock:
            rows = self._db.execute("SELECT pid, ip, reason, created FROM bans "
                                    "ORDER BY created DESC").fetchall()
        return [{"pid": row[0], "ip": row[1], "reason": row[2], "created": row[3]} for row in rows]

    def is_banned(self, pid=None, ip=None):
        """A pid-only ban blocks the pid anywhere; an ip-only ban blocks the address; a pair
        blocks only that pair."""
        with self._lock:
            row = self._db.execute(
                "SELECT 1 FROM bans WHERE (pid = ? AND ip IS NULL) "
                "OR (ip = ? AND pid IS NULL) OR (pid = ? AND ip = ?) LIMIT 1",
                (pid, ip, pid, ip)).fetchone()
        return row is not None

    def forget_failures(self, ip):
        with self._lock:
            self._db.execute("DELETE FROM failures WHERE ip = ?", (check_ip(ip),))
            self._db.commit()


def main(argv=None):
    """Operator CLI: list | add <pid> | delete <pid> | ban | unban | bans | backup | restore"""
    import argparse
    parser = argparse.ArgumentParser(description="Manage public NEX accounts (no password echoed)")
    parser.add_argument("--db", default="nexlab-accounts.sqlite3", help="SQLite file")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="list registered pids")
    add = sub.add_parser("add", help="register one player")
    add.add_argument("pid", help="decimal or 0x-hexadecimal principal id")
    add.add_argument("--password-stdin", action="store_true",
                     help="read the 16-character password from standard input (scripts)")
    for name, help_text in (("ban", "ban a pid and/or an ip (persistent)"),
                            ("unban", "remove a ban")):
        command = sub.add_parser(name, help=help_text)
        command.add_argument("--pid", help="principal id (decimal or 0x-hex)")
        command.add_argument("--ip", help="source IPv4 address")
        command.add_argument("--reason", default="", help="short reason kept in the database")
    sub.add_parser("bans", help="list bans")
    erase = sub.add_parser("delete", help="erase one account (right to erasure)")
    erase.add_argument("pid", help="decimal or 0x-hexadecimal principal id")
    backup = sub.add_parser("backup", help="copy the account database to a file")
    backup.add_argument("destination")
    restore = sub.add_parser("restore", help="replace the account database from a backup")
    restore.add_argument("source")
    args = parser.parse_args(argv)

    accounts = Accounts(args.db)
    try:
        if args.command == "list":
            for pid in accounts.pids():
                print(f"{pid:08x}")
            print(f"{accounts.count()} account(s) in {accounts.path}")
            return 0
        if args.command == "bans":
            for entry in accounts.bans():
                pid = f"{entry['pid']:08x}" if entry["pid"] is not None else "-"
                print(f"pid={pid:<10} ip={entry['ip'] or '-':<16} {entry['reason']}")
            return 0
        if args.command in ("ban", "unban"):
            if not args.pid and not args.ip:
                parser.error(f"{args.command}: give --pid and/or --ip")
            if args.command == "ban":
                accounts.ban(args.pid, args.ip, args.reason)
                print("banned")
            else:
                accounts.unban(args.pid, args.ip)
                print("unbanned")
            return 0
        if args.command == "delete":
            print("deleted" if accounts.delete(args.pid) else "no such account")
            return 0
        if args.command == "backup":
            accounts.backup(args.destination)
            print(f"backup written to {args.destination}")
            return 0
        if args.command == "restore":
            accounts.restore(args.source)
            print(f"restored from {args.source} ({accounts.count()} accounts)")
            return 0
        if args.password_stdin:
            password = sys.stdin.readline().rstrip("\r\n")
        else:
            password = getpass.getpass("NEX password (16 letters or digits): ")
        try:
            status = accounts.register(args.pid, password)
        except AccountError as error:
            print(f"refused: {error}", file=sys.stderr)
            return 1
        print(f"{status}: pid {parse_pid(args.pid):08x}")
        return 0
    finally:
        accounts.close()


if __name__ == "__main__":
    raise SystemExit(main())
