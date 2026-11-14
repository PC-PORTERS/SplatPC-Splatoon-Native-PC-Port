"""SpotPass schedule: the rotation is shared by every player and moves on with the clock."""
import datetime
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "boss"))
import make_vssetting as vs  # noqa: E402

UTC = datetime.timezone.utc


def maps(data, i):
    p = data["Phases"][i]
    return [s["MapID"] for s in p["RegularStages"]], p["GachiRule"], [s["MapID"] for s in p["GachiStages"]]


class Rotation(unittest.TestCase):
    def test_same_schedule_for_players_launched_in_the_same_phase(self):
        a = vs.build({}, now=datetime.datetime(2026, 11, 6, 20, 5, tzinfo=UTC))
        b = vs.build({}, now=datetime.datetime(2026, 11, 6, 23, 59, tzinfo=UTC))
        self.assertEqual(a, b)
        self.assertEqual(a["DateTime"], "2026-11-06T20:00:00Z")

    def test_later_launch_continues_the_same_rotation(self):
        a = vs.build({}, now=datetime.datetime(2026, 11, 6, 20, 0, tzinfo=UTC))
        b = vs.build({}, now=datetime.datetime(2026, 11, 7, 8, 0, tzinfo=UTC))   # 3 phases later
        self.assertEqual(b["DateTime"], "2026-11-07T08:00:00Z")
        for i in range(10):
            self.assertEqual(maps(b, i), maps(a, i + 3))

    def test_maps_change_and_all_come_back(self):
        data = vs.build({}, now=datetime.datetime(2026, 11, 6, 20, 0, tzinfo=UTC))
        firsts = {tuple(maps(data, i)[0]) for i in range(20)}
        self.assertGreater(len(firsts), 10)
        seen = set()
        for i in range(8):
            regular, _, ranked = maps(data, i)
            self.assertEqual(len(set(regular + ranked)), 4)
            seen.update(regular + ranked)
        self.assertEqual(seen, set(vs.MAPS.values()))
        self.assertEqual({maps(data, i)[1] for i in range(3)}, {"cVar", "cVlf", "cVgl"})
        for i in range(1, len(data["Phases"])):
            before, now = maps(data, i - 1), maps(data, i)
            self.assertFalse(set(before[0] + before[2]) & set(now[0] + now[2]), f"phase {i} repeats a map")

    def test_fixed_start_and_explicit_rotation_still_work(self):
        plan = {"start": "2026-10-27T12:00:00Z", "rotation": [{"regular": [0, 1], "ranked_rule": "cVgl", "ranked": [2, 3]}]}
        data = vs.build(plan)
        self.assertEqual(data["DateTime"], "2026-10-27T12:00:00Z")
        self.assertEqual(maps(data, 5), ([0, 1], "cVgl", [2, 3]))


if __name__ == "__main__":
    unittest.main()
