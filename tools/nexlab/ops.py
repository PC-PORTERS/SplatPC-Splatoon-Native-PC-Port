"""Operator client for a running public NEX server.

    py -3.12 tools/nexlab/ops.py --url http://127.0.0.1:60002 metrics
    py -3.12 tools/nexlab/ops.py --url http://127.0.0.1:60002 --token SECRET sessions
    py -3.12 tools/nexlab/ops.py --url http://127.0.0.1:60002 --token SECRET kick 5eea2491

`metrics` needs no token and shows counts only (players, sessions, errors, uptime). `sessions`
and `kick` use the admin token (`registration_token`), so they only exist when the registration
endpoint runs with `registration: "token"`; start it behind TLS. Bans and backups are offline:
`py -3.12 -m tools.nexlab.accounts ban|unban|bans|backup|restore`.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request


def request(url, token, path):
    full = url.rstrip("/") + path
    req = urllib.request.Request(full)
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        try:
            detail = json.loads(error.read())
        except Exception:
            detail = {"error": error.reason}
        return error.code, detail
    except urllib.error.URLError as error:
        return 0, {"error": str(error.reason)}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Query or control a running NEX server")
    parser.add_argument("--url", default=os.environ.get("SPLATOON_OPS_URL", "http://127.0.0.1:60002"),
                        help="registration endpoint base URL")
    parser.add_argument("--token", default=os.environ.get("SPLATOON_REGISTER_TOKEN", ""),
                        help="admin token (or SPLATOON_REGISTER_TOKEN)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("metrics", help="players, sessions, errors, uptime (no token)")
    sub.add_parser("sessions", help="list active sessions (token)")
    kick = sub.add_parser("kick", help="disconnect one player (token)")
    kick.add_argument("pid", help="decimal or 0x-hexadecimal principal id")
    args = parser.parse_args(argv)

    if args.command == "metrics":
        status, payload = request(args.url, None, "/metrics")
    elif args.command == "sessions":
        status, payload = request(args.url, args.token, "/admin/sessions")
    else:
        status, payload = request(args.url, args.token, f"/admin/kick?pid={args.pid}")
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if status == 200 else 1


if __name__ == "__main__":
    raise SystemExit(main())
