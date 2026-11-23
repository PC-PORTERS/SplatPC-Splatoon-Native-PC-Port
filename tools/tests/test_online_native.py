"""The game's own online code (recompiler/port/online, Windows) must give the same integrity proof
as scripts/integrity.py and pass the native C++ tests.
    py -3.12 -m unittest tools.tests.test_online_native -v
Needs gambit_online_tests.exe, built only with -DRBREW_BUILD_TESTS=ON (ninja -C <that build> gambit_online_tests);
GAMBIT_ONLINE_TESTS points to it when it is not in build/game-v288-release. Skipped when it is absent.
"""
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import integrity  # noqa: E402

TOOL = pathlib.Path(os.environ.get("GAMBIT_ONLINE_TESTS") or ROOT / "build" / "game-v288-release" / "gambit_online_tests.exe")


@unittest.skipUnless(TOOL.exists(), f"{TOOL} not built")
class NativeTests(unittest.TestCase):
    def run_tool(self, *args):
        result = subprocess.run([str(TOOL), *map(str, args)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unit_tests(self):
        self.run_tool()

    # -- integrity proof ----------------------------------------------------------------------------
    def native_attestation(self, root):
        with tempfile.TemporaryDirectory() as out:
            self.run_tool("attest", root, pathlib.Path(out) / "proof.json")
            return json.loads((pathlib.Path(out) / "proof.json").read_text(encoding="utf-8"))

    def test_attestation_of_a_crafted_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "scripts").mkdir()
            (root / "recompiler").mkdir()
            (root / "scripts" / "a.py").write_bytes(b"print(1)\r\nprint(2)\r\n")
            (root / "recompiler" / "été.cpp").write_bytes(b"int x;\n" * 50000)
            listed = ["scripts/a.py", "recompiler/été.cpp", "scripts/missing.txt"]
            text = "".join(f"{'1' * 64} {name}\n" for name in listed)
            (root / "scripts" / "integrity-manifest.txt").write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
            self.assertEqual(self.native_attestation(root)["code"], integrity.attestation(str(root))["code"])

    def test_attestation_of_this_checkout(self):
        self.assertEqual(self.native_attestation(ROOT)["code"], integrity.attestation(str(ROOT))["code"])

    # -- the relay over WinHTTP, against a local fake server --------------------------------------------
    def test_relay_cycle_over_http(self):
        received = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                received.append((self.path, self.headers["Content-Type"], body))
                answers = {"/social/session": {"token": "t1"}, "/social/pseudo": {},
                           "/social/presence": {"friends": [{"pid": "0000000a", "name": "Bob", "mode": ""}], "requests": []}}
                data = json.dumps(answers.get(self.path, {})).encode()
                self.send_response(200 if self.path in answers else 404)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as tmp:
                game = pathlib.Path(tmp)
                (game / "friends.txt").write_text("0000000a Bob\n", encoding="utf-8")
                self.run_tool("relay-once", f"http://127.0.0.1:{server.server_address[1]}", game, "12345678", "secret")
                self.assertEqual((game / "friends" / "state.res").read_text(encoding="utf-8"), "friend|0000000a|Bob|1\nmsg|\n")
                self.assertTrue((game / "friends" / "0000000a.txt").exists())
        finally:
            server.shutdown()
        routes = [path for path, _, _ in received]
        self.assertEqual(routes, ["/social/pseudo", "/social/session", "/social/presence"])
        self.assertTrue(all(kind == "application/json" for _, kind, _ in received))
        self.assertEqual(received[1][2], {"pid": "0x12345678", "password": "secret", "client": {"version": "0.1.5", "code": "ab"}})
        self.assertEqual(received[2][2], {"token": "t1", "mode": "", "friends": ["0000000a"]})

    def test_relay_follows_no_redirect(self):
        received = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                received.append((self.path, self.rfile.read(int(self.headers["Content-Length"]))))
                if self.path == "/social/session":      # a 307 would carry the password to the new address
                    self.send_response(307)
                    self.send_header("Location", "/social/elsewhere")
                else:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"{}")

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as tmp:
                result = subprocess.run([str(TOOL), "relay-once", f"http://127.0.0.1:{server.server_address[1]}", tmp,
                                         "12345678", "secret"], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 5, result.stderr)   # not signed in
        finally:
            server.shutdown()
        self.assertEqual([path for path, _ in received], ["/social/pseudo", "/social/session"])


if __name__ == "__main__":
    unittest.main()
