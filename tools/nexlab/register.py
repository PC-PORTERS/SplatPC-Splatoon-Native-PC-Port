"""Register this PC's NEX identity on a public server.

The game never sends its NEX password, so a player registers once before playing: this tool
reads online_identity.ini (the file the port created next to the executable), sends the pid
and the password to the server's registration endpoint, and stores nothing else.

    py -3.12 tools\\nexlab\\register.py --server https://splatoon.example.net --token s3cret
    py -3.12 tools\\nexlab\\register.py --server http://127.0.0.1:60002 --identity Game\\online_identity.ini

The password is never printed or logged. Outside the loopback, plain http:// is refused: the
password would cross the network in clear (S-01). Put the endpoint behind TLS and prefer
registration "token" mode (server_config.registration).
"""
import argparse
import ipaddress
import json
import os
import pathlib
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request

LOOPBACK_NAMES = {"localhost"}


def _is_loopback(host):
    if host in LOOPBACK_NAMES:
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        pass
    try:                                   # B-12: "127.1" and other inet_aton shorthands
        return socket.inet_aton(host)[0] == 127
    except OSError:
        return False


def check_server_url(url, allow_http=False):
    """Refuse a plaintext endpoint outside the loopback (S-01): the password travels in it.

    Returns the URL as given. Raises ValueError with a player-readable message.
    """
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme not in ("http", "https"):
        raise ValueError("the server URL must start with https:// (or http:// for a local test)")
    if parsed.scheme == "https":
        return url
    host = (parsed.hostname or "").lower()
    local = _is_loopback(host)
    if local or allow_http:
        return url
    raise ValueError("refusing plain http:// to a remote host: your NEX password would cross "
                     "the network in clear. Use https://, or --allow-http for a trusted LAN.")


def read_identity(path):
    fields = {}
    for line in pathlib.Path(path).read_text(encoding="utf-8-sig").splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            key, value = line.split("=", 1)
            fields[key.strip()] = value.strip()
    pid, password = fields.get("pid", ""), fields.get("password", "")
    if not pid or not password:
        raise ValueError(f"{path}: no pid/password pair")
    number = int(pid, 16)
    if not 0 < number <= 0x7FFFFFFF:
        raise ValueError(f"{path}: pid out of range")
    if len(password) != 16 or not password.isalnum() or not password.isascii():
        raise ValueError(f"{path}: password must be 16 letters or digits")
    return number, password


def main(argv=None):
    parser = argparse.ArgumentParser(description="Register a player's NEX identity on a public server")
    parser.add_argument("--server", required=True, help="https://host[:port] of the registration endpoint")
    parser.add_argument("--identity", default="Game/online_identity.ini",
                        help="identity file written by the port (default: Game/online_identity.ini)")
    parser.add_argument("--token", help="registration token (or set SPLATOON_REGISTER_TOKEN)")
    parser.add_argument("--allow-http", action="store_true",
                        help="accept http:// to a non-local host (trusted LAN only)")
    args = parser.parse_args(argv)

    try:
        server = check_server_url(args.server, args.allow_http)
    except ValueError as error:
        print(f"refused: {error}", file=sys.stderr)
        return 1
    try:
        pid, password = read_identity(args.identity)
    except (OSError, ValueError) as error:
        print(f"cannot read the identity: {error}", file=sys.stderr)
        return 1

    token = args.token or os.environ.get("SPLATOON_REGISTER_TOKEN", "")
    body = json.dumps({"pid": f"0x{pid:08x}", "password": password}).encode("utf-8")
    request = urllib.request.Request(server.rstrip("/") + "/register", data=body,
                                     headers={"Content-Type": "application/json"})
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            answer = json.loads(response.read())
    except urllib.error.HTTPError as error:
        try:
            detail = json.loads(error.read()).get("error", error.reason)
        except Exception:
            detail = error.reason
        print(f"registration refused ({error.code}): {detail}", file=sys.stderr)
        return 1
    except urllib.error.URLError as error:
        print(f"cannot reach {server}: {error.reason}", file=sys.stderr)
        return 1
    print(f"{answer.get('status', 'accepted')}: pid {pid:08x} on {server}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
