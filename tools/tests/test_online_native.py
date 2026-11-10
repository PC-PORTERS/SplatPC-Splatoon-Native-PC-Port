"""The game's own online code (recompiler/port/online, Windows) must give exactly what the Python tools give:
the same integrity proof as scripts/integrity.py and the same Game/boss bytes as tools/boss/make_*.py.
    py -3.12 -m unittest tools.tests.test_online_native -v
Needs gambit_online_tests.exe (ninja -C build/game-v288-release gambit_online_tests); GAMBIT_ONLINE_TESTS may
point to another copy. Skipped when it is absent.
"""
import datetime
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
sys.path.insert(0, str(ROOT / "tools" / "boss"))
import byaml  # noqa: E402
import integrity  # noqa: E402
import make_festival  # noqa: E402
import make_vssetting  # noqa: E402

TOOL = pathlib.Path(os.environ.get("GAMBIT_ONLINE_TESTS") or ROOT / "build" / "game-v288-release" / "gambit_online_tests.exe")
UTC = datetime.timezone.utc


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
            (root / "VERSION").write_bytes(b"v0.1.5\r\n")
            (root / "scripts" / "a.py").write_bytes(b"print(1)\r\nprint(2)\r\n")
            (root / "recompiler" / "été.cpp").write_bytes(b"int x;\n" * 50000)
            listed = ["scripts/a.py", "recompiler/été.cpp", "scripts/missing.txt"]
            text = "".join(f"{'1' * 64} {name}\n" for name in listed)
            (root / "scripts" / "integrity-manifest.txt").write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
            self.assertEqual(self.native_attestation(root), integrity.attestation(str(root)))

    def test_attestation_of_this_checkout(self):
        self.assertEqual(self.native_attestation(ROOT), integrity.attestation(str(ROOT)))

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

    # -- SpotPass files -------------------------------------------------------------------------------
    def compare_boss(self, schedule, festival, now):
        with tempfile.TemporaryDirectory() as tmp:
            boss = pathlib.Path(tmp)
            if schedule is not None:
                (boss / "planning.json").write_text(json.dumps(schedule), encoding="utf-8")
            if festival is not None:
                (boss / "festival.json").write_text(json.dumps(festival, ensure_ascii=False), encoding="utf-8-sig")
            self.run_tool("boss", boss, now.strftime("%Y-%m-%dT%H:%M:%SZ"))
            # A missing plan is created with the defaults, the same ones as the Python tools.
            plan = json.loads((boss / "planning.json").read_text(encoding="utf-8-sig"))
            fest = json.loads((boss / "festival.json").read_text(encoding="utf-8-sig"))
            if schedule is None:
                self.assertEqual(plan, make_vssetting.DEFAULT_PLAN)
            if festival is None:
                self.assertEqual(fest, make_festival.DEFAULT_PLAN)
            expected = byaml.dump(make_vssetting.build(plan, now=now))
            self.assertEqual((boss / "schdat2" / "VSSetting.byaml").read_bytes(), expected, f"VSSetting at {now}")
            data = make_festival.build(fest)
            fid = "%04d" % data["FestivalId"]
            optdat = boss / "optdat2"
            self.assertEqual((optdat / "Festival.byaml").read_bytes(), byaml.dump(data))
            self.assertEqual((optdat / "HapTexture.bfres").read_bytes(), make_festival.empty_bfres("HapTexture_" + fid))
            self.assertEqual((optdat / "PanelTexture.bfres").read_bytes(), make_festival.empty_bfres("PanelTexture_" + fid))

    def test_default_plans_over_many_dates(self):
        dates = [datetime.datetime(2015, 5, 28, tzinfo=UTC), datetime.datetime(2026, 11, 10, 13, 7, 5, tzinfo=UTC),
                 datetime.datetime(2026, 12, 31, 23, 59, 59, tzinfo=UTC), datetime.datetime(2031, 2, 3, 4, 0, 0, tzinfo=UTC)]
        dates.append(datetime.datetime.now(UTC).replace(microsecond=0))
        for now in dates:
            with self.subTest(now=now):
                self.compare_boss(None, None, now)

    def test_custom_plans(self):
        schedule = {
            "start": "2026-11-01T10:30:00+01:00", "phase_hours": 2, "days": 3,
            "rotation": [{"regular": ["Fld_Crank00_Vss", 4], "ranked_rule": "cVgl", "ranked": [5, "Fld_Maze00"]},
                         {"regular": [1, 2, 3]}],
            "weapon_unlock": {"5021": "2026-11-02T00:00:00Z", "1000": "2026-11-01T00:00:00-05:00"},
            "maps_open": [0, 3, "Fld_Office00"], "available_since": "2026-10-01T00:00:00Z",
        }
        festival = {
            "id": 7, "announce": "2026-11-01T10:00:00Z", "start": "2026-11-08T10:00:00Z", "end": "2026-11-09T10:00:00Z",
            "result": "2026-11-09T16:00:00Z", "stages": [2, "Fld_Pivot00"], "countries": ["FR", "DE"],
            "teams": [{"name": {"fr": "Équipe Mer", "en": "Sea"}, "color": "1,0.5,0.25,1"},
                      {"name": {"de": "Berg"}, "short": {"de": "B"}, "color": "0.333333,0.1,0,1"}],
            "neutral_color": "0.5, 0.5, 0.5, 1",
        }
        self.compare_boss(schedule, festival, datetime.datetime(2026, 11, 10, 9, 0, 0, tzinfo=UTC))
        generated = {"map_pool": ["Fld_Crank00", 1, 2, 3, 4, 5, 6, 7, 8], "ranked_rules": ["cVlf", "cVar"], "days": 30, "phase_hours": 3}
        self.compare_boss(generated, None, datetime.datetime(2026, 11, 10, 9, 0, 0, tzinfo=UTC))

    def test_wrong_plan_keeps_the_previous_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            boss = pathlib.Path(tmp)
            (boss / "planning.json").write_text('{"phase_hours": 0}', encoding="utf-8")
            (boss / "schdat2").mkdir()
            (boss / "schdat2" / "VSSetting.byaml").write_bytes(b"old")
            result = subprocess.run([str(TOOL), "boss", str(boss), "2026-11-10T00:00:00Z"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual((boss / "schdat2" / "VSSetting.byaml").read_bytes(), b"old")
            self.assertTrue((boss / "optdat2" / "Festival.byaml").exists())  # the festival does not depend on it


if __name__ == "__main__":
    unittest.main()
