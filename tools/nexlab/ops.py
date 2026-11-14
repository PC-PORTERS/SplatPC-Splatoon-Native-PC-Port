"""Operator client for a running public NEX server.

    py -3.12 tools/nexlab/ops.py --url http://127.0.0.1:60002 metrics
    py -3.12 tools/nexlab/ops.py --url http://127.0.0.1:60002 --token SECRET sessions
    py -3.12 tools/nexlab/ops.py --url http://127.0.0.1:60002 --token SECRET kick 5eea2491

    py -3.12 tools/nexlab/ops.py --url ... --token SECRET ban --pid 5eea2491 --hours 24 --reason "insults"
    py -3.12 tools/nexlab/ops.py --url ... --token SECRET unban --pid 5eea2491
    py -3.12 tools/nexlab/ops.py --url ... --token SECRET bans | log

`metrics` needs no token and shows counts only (players, sessions, errors, uptime). The other commands use
the admin token (`admin_token`, else `registration_token`); start the endpoint behind TLS. `ban` takes effect
at once (the player is disconnected) and is written to the moderation log. Backups are offline:
`py -3.12 -m tools.nexlab.accounts backup|restore` (the same tool also bans and lists offline).
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request


def request(url, token, path, body=None):
    full = url.rstrip("/") + path
    req = urllib.request.Request(full)
    if body is not None:
        req = urllib.request.Request(full, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
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
    for name, text in (("ban", "ban a player and/or an address now, and disconnect the player (token)"),
                       ("unban", "lift a ban (token)")):
        command = sub.add_parser(name, help=text)
        command.add_argument("--pid", help="principal id (decimal or 0x-hex)")
        command.add_argument("--ip", help="source address")
        if name == "ban":
            command.add_argument("--hours", type=float, help="ban for this many hours (default: for good)")
            command.add_argument("--reason", default="", help="short reason, kept in the moderation log")
    sub.add_parser("bans", help="active bans (token)")
    sub.add_parser("log", help="the latest moderation actions (token)")
    args = parser.parse_args(argv)

    if args.command in ("ban", "unban"):
        if not args.pid and not args.ip:
            parser.error(f"{args.command}: give --pid and/or --ip")
        body = {"pid": args.pid, "ip": args.ip}
        if args.command == "ban":
            body.update(hours=args.hours, reason=args.reason)
        status, payload = request(args.url, args.token, f"/admin/{args.command}", body)
    elif args.command in ("bans", "log"):
        status, payload = request(args.url, args.token, f"/admin/{args.command}")
    elif args.command == "metrics":
        status, payload = request(args.url, None, "/metrics")
    elif args.command == "sessions":
        status, payload = request(args.url, args.token, "/admin/sessions")
    else:
        status, payload = request(args.url, args.token, f"/admin/kick?pid={args.pid}")
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if status == 200 else 1


if __name__ == "__main__":
    raise SystemExit(main())
