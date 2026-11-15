"""Builds Splatoon's SpotPass festival data (storage optdat2) from an editable file.

Writes Festival.byaml, HapTexture.bfres and PanelTexture.bfres into Game/boss/optdat2/, served to the
game in online mode by recompiler/port/os/stubs/boss_local.cpp. The game's "online" test
(Online_isAvailable 028C2248) needs all three loaded when the save holds a festival state, which is why
the shops said "offline" with only the schedule.

usage: py -3.12 make_festival.py [--plan FILE] [--out DIR]

Plan (default Game/boss/festival.json, created on first run):
  id            FestivalId (int)
  announce, start, end, result   ISO dates ("2026-11-01T10:00:00Z"); the default festival is announced
                in 2099, so nothing is shown until a real one is planned
  rule          cPnt (Turf War) is the festival rule
  stages        1-3 maps (MapID or name, see make_vssetting.MAPS)
  teams         [team A, team B]: {"name": {"fr": ..., "en": ...}, "short": {...}, "color": "r,g,b,a"}
  neutral_color "r,g,b,a" for the third, nameless entry
  countries     optional list of country codes taking part (empty = everyone)

Schema decoded from the game (Festival_parse 021BB834 and helpers): Version 0, FestivalId,
Country[str]?, HideTeamNameOnBoard?, BattleResultRate?, LowPopulation?, SeparateMatchingJP?,
Teams[3]{Name{<region><lang>}, ShortName{...}, Color "r,g,b,a"}, Rule, Stages[{MapID}] (<=3),
Time{Announce, Start, End, Result}, News[4]{NewsType: Announce|Start|ResultA|ResultB, <region><lang>:
[script commands]}.
Name keys are region+language ("EUfr", "USen"...); every combination is written.
Textures: BFRES 3.5.0.3 (the game checks "FRES", version 0x03050003, BOM FEFF) loaded generically;
an empty BFRES (no texture) is valid. Panel images are a later step.
"""
import argparse, datetime, json, pathlib, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import byaml
from make_vssetting import map_id, rule, parse_date, fmt

ROOT = pathlib.Path(__file__).resolve().parents[2]
# The game's tables (101E09D0, 101E09F4) start with "None": a console setting the port leaves unset
# gives keys such as "Nonefr" or "EUNone", so those are written too.
REGIONS = ("None", "JP", "US", "EU", "CN", "KR", "TW")
LANGS = ("None", "ja", "en", "fr", "de", "it", "es", "ch", "ko", "nl", "pt", "ru", "ta")
DEFAULT_PLAN = {
    "id": 1000,
    "announce": "2099-01-01T00:00:00Z",
    "start": "2099-01-02T00:00:00Z",
    "end": "2099-01-03T00:00:00Z",
    "result": "2099-01-03T06:00:00Z",
    "rule": "cPnt",
    "stages": ["Fld_Crank00", "Fld_Warehouse00", "Fld_SeaPlant00"],
    "teams": [
        {"name": {"fr": "Chats", "en": "Cats"}, "short": {"fr": "Chats", "en": "Cats"}, "color": "0.85,0.2,0.55,1"},
        {"name": {"fr": "Chiens", "en": "Dogs"}, "short": {"fr": "Chiens", "en": "Dogs"}, "color": "0.2,0.75,0.25,1"},
    ],
    "neutral_color": "0.5,0.5,0.5,1",
    "countries": [],
}


def localized(texts):
    """Every region+language key; a missing language falls back to English, then to any text."""
    fallback = texts.get("en") or next(iter(texts.values()))
    return {r + l: texts.get(l, fallback) for r in REGIONS for l in LANGS}


def color(text):
    try:
        parts = [float(x) for x in str(text).split(",")]
    except ValueError:
        raise ValueError(f"color {text!r}: 4 numeric values 0-1, r,g,b,a") from None
    if len(parts) != 4 or not all(0.0 <= p <= 1.0 for p in parts): raise ValueError(f"color {text!r}: 4 values 0-1, 'r,g,b,a'")
    return ",".join(f"{p:g}" for p in parts)


def build(plan):
    plan = {**DEFAULT_PLAN, **plan}
    try:
        dates = [parse_date(plan[k]) for k in ("announce", "start", "end", "result")]
    except (ValueError, TypeError, AttributeError):
        raise ValueError("announce/start/end/result must be ISO dates") from None
    if not all(a < b for a, b in zip(dates, dates[1:])): raise ValueError("dates must satisfy announce < start < end < result")
    stages = [map_id(m) for m in plan["stages"]]
    if not 1 <= len(stages) <= 3: raise ValueError("1-3 stages")
    if not isinstance(plan["teams"], list) or len(plan["teams"]) != 2: raise ValueError("exactly 2 named teams are required; the third neutral team is generated")
    for index, team in enumerate(plan["teams"]):
        if not isinstance(team, dict): raise ValueError(f"team {index+1}: expected an object")
        for field in ("name", "short"):
            texts = team.get(field, team.get("name"))
            if not isinstance(texts, dict) or not texts or not all(isinstance(v, str) and v.strip() for v in texts.values()):
                raise ValueError(f"team {index+1}: {field} needs nonempty localized text")
        if "color" not in team: raise ValueError(f"team {index+1}: missing color")
    teams = [{"Name": localized(t["name"]), "ShortName": localized(t.get("short", t["name"])), "Color": color(t["color"])}
             for t in plan["teams"]]
    teams.append({"Color": color(plan["neutral_color"])})
    data = {
        "Version": 0,
        "FestivalId": int(plan["id"]),
        "Teams": teams,
        "Rule": rule(plan["rule"]),
        "Stages": [{"MapID": m} for m in stages],
        "Time": {"Announce": fmt(dates[0]), "Start": fmt(dates[1]), "End": fmt(dates[2]), "Result": fmt(dates[3])},
        # Each news also carries, under every region+language key, the Callie & Marie script: a list of
        # commands (U_Command, 0217F028); an empty list is accepted. A missing key rejects the file.
        "News": [{"NewsType": n, **{k: [] for k in localized({"en": ""})}} for n in ("Announce", "Start", "ResultA", "ResultB")],
    }
    if plan["countries"]: data["Country"] = [str(c) for c in plan["countries"]]
    return data


def empty_bfres(name):
    """BFRES 3.5.0.3 header with no sub-files (0x6C bytes) plus its name in the string table."""
    header = bytearray(0x6C)
    struct.pack_into(">4sIHH", header, 0, b"FRES", 0x03050003, 0xFEFF, 0x10)
    strings = struct.pack(">I", len(name)) + name.encode() + b"\0"
    strings += b"\0" * (-len(strings) % 4)
    struct.pack_into(">I", header, 0x10, 0x2000)                      # file alignment
    struct.pack_into(">i", header, 0x14, 0x6C + 4 - 0x14)             # name, relative to the field
    struct.pack_into(">ii", header, 0x18, len(strings), 0x6C - 0x1C)  # string table length / offset
    blob = header + strings
    struct.pack_into(">I", blob, 0x0C, len(blob))
    return bytes(blob)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", type=pathlib.Path, default=ROOT / "Game" / "boss" / "festival.json")
    ap.add_argument("--out", type=pathlib.Path, default=ROOT / "Game" / "boss" / "optdat2")
    a = ap.parse_args()
    if not a.plan.exists():
        a.plan.parent.mkdir(parents=True, exist_ok=True)
        a.plan.write_text(json.dumps(DEFAULT_PLAN, indent=2, ensure_ascii=False), encoding="utf-8")
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
    a.out.mkdir(parents=True, exist_ok=True)
    # Festival_checkTextures (021B9FE4) clears everything unless each BFRES name contains the
    # FestivalId as "%04d" (021B9E34).
    fid = "%04d" % data["FestivalId"]
    for name, content in (("Festival.byaml", blob), ("HapTexture.bfres", empty_bfres("HapTexture_" + fid)),
                          ("PanelTexture.bfres", empty_bfres("PanelTexture_" + fid))):
        tmp = a.out / (name + ".tmp")
        tmp.write_bytes(content)
        tmp.replace(a.out / name)
    print(f"{a.out}: festival {data['FestivalId']} {data['Time']['Start']} -> {data['Time']['End']}, {len(blob)} bytes")


if __name__ == "__main__":
    main()


