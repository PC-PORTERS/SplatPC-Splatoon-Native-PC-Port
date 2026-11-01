"""Builds Splatoon's SpotPass schedule (VSSetting.byaml) from an editable plan.

The plan is a JSON file (default Game/boss/planning.json, created on first run). The port serves
Game/boss/<storage>/ to the game in online mode (recompiler/port/os/stubs/boss_local.cpp); the
schedule goes to storage "schdat2".

usage: py -3.12 make_vssetting.py [--plan FILE] [--out FILE] [--show]

Plan keys (all optional; defaults in DEFAULT_PLAN):
  start          "now" (current hour, UTC) or a date "2026-10-27T12:00:00Z"
  phase_hours    length of one rotation, in hours (the game's phase "Time")
  days           how long the schedule runs from start (phases = days*24/phase_hours, max 256)
  rotation       list of {"regular": [maps], "ranked_rule": rule, "ranked": [maps]}, cycled in order;
                 empty or missing = generated from "map_pool" and "ranked_rules"
  map_pool       maps used by the generated rotation
  ranked_rules   rules cycled by the generated rotation (cVar, cVlf, cVgl)
  weapon_unlock  "start" = the shop may sell every SpotPass weapon from the start date (each one
                 still needs its level and price), or {"<set id>": "date", ...} to stagger them
  maps_open      maps whose first appearance is before start (new maps get no "new" banner)
A map is its MapID or internal name (MAPS below). Rules: cPnt (Turf War), cVar, cVlf, cVgl.

Schema decoded from the game (VSSetting_parse 021BF5F0 and its helpers): Version <= 3,
DateTime, Phases[{Time int, RegularRule, RegularStages[{MapID}] (<=3), GachiRule, GachiStages}],
WeaponUnlock[{WeaponSetID, Date}], MapFirstAppear[{MapID, Date}], RuleFirstAppear[{GachiRule, Date}].
Dates: "YYYY-MM-DDTHH:MM:SS" + "Z" or "+hh:mm".
"""
import argparse, datetime, json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import byaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
# MapInfo.byaml (Static.pack): versus maps and their MapID.
MAPS = {
    "Fld_Crank00": 0, "Fld_Warehouse00": 1, "Fld_SeaPlant00": 2, "Fld_UpDown00": 3,
    "Fld_SkatePark00": 4, "Fld_Athletic00": 5, "Fld_Amida00": 6, "Fld_Maze00": 7,
    "Fld_Tuzura00": 8, "Fld_Ruins00": 9, "Fld_Kaisou00": 10, "Fld_Jyoheki00": 11,
    "Fld_Pivot00": 12, "Fld_Office00": 13, "Fld_Quarry00": 14, "Fld_Hiagari00": 15,
}
RULES = ("cPnt", "cVar", "cVlf", "cVgl")
# WeaponSet.byaml (Static.pack): sets with Lock "Boss", unlocked by WeaponUnlock dates.
SPOTPASS_WEAPON_SETS = [
    1000, 1001, 1002, 1020, 1021, 1042, 1060, 1061, 1062, 1072, 1081, 1091, 1110, 1111, 1130, 1131,
    1132, 1150, 1151, 1160, 1161, 1170, 1171, 1172, 2000, 2001, 2012, 2030, 2031, 2032, 2040, 2041,
    3000, 3001, 3002, 3010, 3011, 3020, 3021, 4002, 4012, 4022, 4031, 4040, 4041, 4050, 4051, 4052,
    5000, 5001, 5002, 5010, 5011, 5012, 5020, 5021,
]
DEFAULT_PLAN = {
    "start": "now",
    "phase_hours": 4,
    "days": 14,
    "rotation": [],
    "map_pool": list(MAPS),
    "ranked_rules": ["cVar", "cVlf", "cVgl"],
    "weapon_unlock": "start",
    "maps_open": list(MAPS),
    # Date given to weapons, maps and ranked rules already available. Dated "yesterday", Callie and
    # Marie announced every one of them as new at each online launch (27/10).
    "available_since": "2015-05-28T00:00:00Z",
}


def map_id(m):
    if isinstance(m, int) and m in MAPS.values(): return m
    if isinstance(m, str) and m.removesuffix("_Vss") in MAPS: return MAPS[m.removesuffix("_Vss")]
    raise ValueError(f"unknown map {m!r} (MapID 0-15 or a name of {list(MAPS)})")


def rule(r):
    if r not in RULES: raise ValueError(f"unknown rule {r!r} (one of {RULES})")
    return r


def parse_date(text):
    return datetime.datetime.fromisoformat(text.replace("Z", "+00:00")).astimezone(datetime.timezone.utc)


def fmt(d):
    return d.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def generated_rotation(pool, rules, count):
    """Two Turf War maps and two ranked maps per phase, walking the pool so each map comes back
    regularly and the four maps of a phase are distinct."""
    pool = [map_id(m) for m in pool]
    if len(pool) < 4: raise ValueError("map_pool needs at least 4 maps")
    out, cursor = [], 0
    for i in range(count):
        picks = [pool[(cursor + k) % len(pool)] for k in range(4)]
        cursor += 3   # odd shift: consecutive phases share at most one map
        out.append({"regular": picks[:2], "ranked_rule": rules[i % len(rules)], "ranked": picks[2:]})
    return out


def build(plan):
    plan = {**DEFAULT_PLAN, **plan}
    now = datetime.datetime.now(datetime.timezone.utc).replace(minute=0, second=0, microsecond=0)
    start = now if plan["start"] == "now" else parse_date(plan["start"])
    hours = int(plan["phase_hours"])
    if not 1 <= hours <= 24: raise ValueError("phase_hours must be 1..24")
    count = min(256, max(1, int(plan["days"]) * 24 // hours))
    rotation = plan["rotation"] or generated_rotation(plan["map_pool"], [rule(r) for r in plan["ranked_rules"]], count)
    phases = []
    for i in range(count):
        p = rotation[i % len(rotation)]
        regular, ranked = [map_id(m) for m in p.get("regular", [])], [map_id(m) for m in p.get("ranked", [])]
        if not 1 <= len(regular) <= 3 or len(ranked) > 3: raise ValueError(f"phase {i}: 1-3 regular maps, 0-3 ranked maps")
        phases.append({
            "Time": hours,
            "RegularRule": "cPnt",
            "RegularStages": [{"MapID": m} for m in regular],
            "GachiRule": rule(p.get("ranked_rule", "cVar")),
            "GachiStages": [{"MapID": m} for m in ranked],
        })
    past = fmt(min(parse_date(plan["available_since"]), start - datetime.timedelta(days=1)))
    unlock = plan["weapon_unlock"]
    if unlock == "start":
        unlocks = [{"WeaponSetID": s, "Date": past} for s in SPOTPASS_WEAPON_SETS]
    else:
        unlocks = [{"WeaponSetID": int(s), "Date": fmt(parse_date(d))} for s, d in unlock.items()]
    return {
        "Version": 3,
        "DateTime": fmt(start),
        "Phases": phases,
        "WeaponUnlock": unlocks,
        "MapFirstAppear": [{"MapID": map_id(m), "Date": past} for m in plan["maps_open"]],
        "RuleFirstAppear": [{"GachiRule": r, "Date": past} for r in ("cVar", "cVlf", "cVgl")],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", type=pathlib.Path, default=ROOT / "Game" / "boss" / "planning.json")
    # The game's NBDL tasks/storages are "schdat2" (schedule) and "optdat2" (seen in the net trace).
    ap.add_argument("--out", type=pathlib.Path, default=ROOT / "Game" / "boss" / "schdat2" / "VSSetting.byaml")
    ap.add_argument("--show", action="store_true", help="print the first phases")
    a = ap.parse_args()
    if not a.plan.exists():
        a.plan.parent.mkdir(parents=True, exist_ok=True)
        a.plan.write_text(json.dumps(DEFAULT_PLAN, indent=2), encoding="utf-8")
    try:
        plan = json.loads(a.plan.read_text(encoding="utf-8-sig"))
    except json.JSONDecodeError as error:
        ap.error(f"{a.plan}: JSON invalide, ligne {error.lineno}, colonne {error.colno}: {error.msg}")
    if not isinstance(plan, dict): ap.error(f"{a.plan}: le JSON doit contenir un objet")
    try:
        data = build(plan)
    except ValueError as error:
        ap.error(f"{a.plan}: {error}")
    blob = byaml.dump(data)
    assert byaml.load(blob) == data
    a.out.parent.mkdir(parents=True, exist_ok=True)
    tmp = a.out.with_suffix(".tmp")
    tmp.write_bytes(blob)
    tmp.replace(a.out)
    print(f"{a.out}: {len(data['Phases'])} phases from {data['DateTime']}, {len(blob)} bytes")
    if a.show:
        names = {v: k for k, v in MAPS.items()}
        for p in data["Phases"][:4]:
            print(" ", [names[s["MapID"]] for s in p["RegularStages"]], p["GachiRule"], [names[s["MapID"]] for s in p["GachiStages"]])


if __name__ == "__main__":
    main()


