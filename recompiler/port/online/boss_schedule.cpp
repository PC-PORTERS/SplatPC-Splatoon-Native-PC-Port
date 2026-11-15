#include "boss_schedule.h"
#include "byaml.h"
#include "py_random.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <process.h>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rbrew_native {
namespace {

constexpr int64_t kSecond = 1000000;
constexpr int64_t kEpoch = 1432771200 * kSecond; // 2015-05-28T00:00:00Z: phases count from here on every PC

const std::vector<std::pair<std::string, int>> kMaps = {
    {"Fld_Crank00", 0},     {"Fld_Warehouse00", 1}, {"Fld_SeaPlant00", 2}, {"Fld_UpDown00", 3},
    {"Fld_SkatePark00", 4}, {"Fld_Athletic00", 5},  {"Fld_Amida00", 6},    {"Fld_Maze00", 7},
    {"Fld_Tuzura00", 8},    {"Fld_Ruins00", 9},     {"Fld_Kaisou00", 10},  {"Fld_Jyoheki00", 11},
    {"Fld_Pivot00", 12},    {"Fld_Office00", 13},   {"Fld_Quarry00", 14},  {"Fld_Hiagari00", 15},
};
const std::vector<std::string> kRules = {"cPnt", "cVar", "cVlf", "cVgl"};
const std::vector<int> kSpotPassWeaponSets = {
    1000, 1001, 1002, 1020, 1021, 1042, 1060, 1061, 1062, 1072, 1081, 1091, 1110, 1111, 1130, 1131, 1132, 1150, 1151,
    1160, 1161, 1170, 1171, 1172, 2000, 2001, 2012, 2030, 2031, 2032, 2040, 2041, 3000, 3001, 3002, 3010, 3011, 3020,
    3021, 4002, 4012, 4022, 4031, 4040, 4041, 4050, 4051, 4052, 5000, 5001, 5002, 5010, 5011, 5012, 5020, 5021,
};
const char* const kRegions[] = {"None", "JP", "US", "EU", "CN", "KR", "TW"};
const char* const kLangs[] = {"None", "ja", "en", "fr", "de", "it", "es", "ch", "ko", "nl", "pt", "ru", "ta"};

[[noreturn]] void fail(const std::string& reason) {
    throw std::invalid_argument(reason);
}

int64_t floor_div(int64_t a, int64_t b) {
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

bool truthy(const Plan& value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value.get<double>() != 0.0;
    if (value.is_string() || value.is_array() || value.is_object()) return !value.empty();
    return true;
}

// Python int(): an int, a float (truncated), a bool or a decimal string.
int64_t to_int(const Plan& value, const char* what) {
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_number_integer()) return value.get<int64_t>();
    if (value.is_number_float() && std::isfinite(value.get<double>())) return int64_t(value.get<double>());
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        text.erase(0, text.find_first_not_of(" \t\r\n"));
        text.erase(text.find_last_not_of(" \t\r\n") + 1);
        char* end = nullptr;
        const long long n = std::strtoll(text.c_str(), &end, 10);
        if (!text.empty() && end && !*end) return n;
    }
    fail(std::string(what) + ": not a whole number");
}

Plan map_id(const Plan& m) {
    if (m.is_boolean()) return m; // Python: True/False are the ints 1/0
    if (m.is_number_integer() && m.get<int64_t>() >= 0 && m.get<int64_t>() <= 15) return m;
    if (m.is_string()) {
        std::string name = m.get<std::string>();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, "_Vss") == 0) name.resize(name.size() - 4);
        for (const auto& [map, id] : kMaps)
            if (map == name) return id;
    }
    fail("unknown map " + m.dump() + " (MapID 0-15 or a Fld_ name)");
}

std::string rule(const Plan& r) {
    if (r.is_string() && std::find(kRules.begin(), kRules.end(), r.get<std::string>()) != kRules.end())
        return r.get<std::string>();
    fail("unknown rule " + r.dump() + " (one of cPnt, cVar, cVlf, cVgl)");
}

std::string date_text(const Plan& value) {
    if (!value.is_string()) fail("dates must be ISO text");
    return value.get<std::string>();
}

Plan merged(const Plan& defaults, const Plan& plan) {
    Plan out = defaults;
    for (const auto& [key, value] : plan.items()) out[key] = value;
    return out;
}

// make_vssetting._deck: one shuffled deck per cycle; maps of the previous cycle's last phase go last.
class Decks {
public:
    explicit Decks(std::vector<Plan> pool) : pool_(std::move(pool)) {}
    const std::vector<Plan>& deck(int64_t cycle) {
        const size_t per_cycle = pool_.size() / 4;
        while (int64_t(decks_.size()) <= cycle) {
            const int64_t c = int64_t(decks_.size());
            std::vector<Plan> cards = pool_;
            PyRandom("splatoon-rotation-" + std::to_string(c)).shuffle(cards);
            if (!decks_.empty()) {
                const auto& last = decks_.back();
                const std::set<std::string> previous = [&] {
                    std::set<std::string> s;
                    for (size_t i = 4 * per_cycle - 4; i < 4 * per_cycle && i < last.size(); ++i)
                        s.insert(last[i].dump());
                    return s;
                }();
                std::stable_partition(cards.begin(), cards.end(),
                                      [&](const Plan& m) { return !previous.count(m.dump()); });
            }
            decks_.push_back(std::move(cards));
        }
        return decks_[size_t(cycle)];
    }

private:
    std::vector<Plan> pool_;
    std::vector<std::vector<Plan>> decks_;
};

Plan localized(const Plan& texts) {
    Plan fallback = texts.contains("en") ? texts["en"] : Plan();
    if (!truthy(fallback)) fallback = texts.begin().value();
    Plan out = Plan::object();
    for (const char* region : kRegions)
        for (const char* lang : kLangs) out[std::string(region) + lang] = texts.contains(lang) ? texts[lang] : fallback;
    return out;
}

std::string color(const Plan& text) {
    if (!text.is_string()) fail("color " + text.dump() + ": 4 numeric values 0-1, r,g,b,a");
    const std::string all = text.get<std::string>();
    std::vector<double> parts;
    size_t start = 0;
    while (true) {
        const size_t comma = all.find(',', start);
        std::string part = all.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        part.erase(0, part.find_first_not_of(" \t\r\n"));
        part.erase(part.find_last_not_of(" \t\r\n") + 1);
        char* end = nullptr;
        const double v = std::strtod(part.c_str(), &end);
        if (part.empty() || !end || *end || part.find_first_of("xX") != std::string::npos)
            fail("color " + text.dump() + ": 4 numeric values 0-1, r,g,b,a");
        parts.push_back(v);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() != 4 || !std::all_of(parts.begin(), parts.end(), [](double p) { return p >= 0.0 && p <= 1.0; }))
        fail("color " + text.dump() + ": 4 values 0-1, 'r,g,b,a'");
    std::string out;
    for (double p : parts) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", p);
        if (!out.empty()) out += ',';
        out += buf;
    }
    return out;
}

// The temporary name carries the process id: two games started together (test launchers) share Game/boss.
bool write_file(const fs::path& path, fs::path temporary, const std::string& content) {
    temporary += "." + std::to_string(_getpid());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(content.data(), std::streamsize(content.size())) || !out.flush()) return false;
    }
    fs::rename(temporary, path, ec);
    return !ec;
}

// The plan file, created with the defaults when missing (as the Python tools do).
Plan read_plan(const fs::path& path, const Plan& defaults) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        fs::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << defaults.dump(2);
    }
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Plan plan;
    try {
        plan = Plan::parse(text); // skips a UTF-8 BOM, like utf-8-sig
    } catch (const nlohmann::json::parse_error& error) {
        fail(path.filename().string() + ": invalid JSON (" + error.what() + ")");
    }
    if (!plan.is_object()) fail(path.filename().string() + ": the JSON must hold an object");
    return plan;
}

} // namespace

int64_t unix_micros_now() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t parse_date(const std::string& raw) {
    // Python 3.11 datetime.fromisoformat after "Z" -> "+00:00": YYYY-MM-DD[(T| )HH[:MM[:SS[.ffffff]]]][Z|±HH[:MM]]
    const char* s = raw.c_str();
    const auto digits = [&](int count, int& value) {
        value = 0;
        for (int i = 0; i < count; ++i) {
            if (*s < '0' || *s > '9') return false;
            value = value * 10 + (*s++ - '0');
        }
        return true;
    };
    int year, month, day, hour = 0, minute = 0, second = 0;
    int64_t micros = 0;
    if (!digits(4, year) || *s++ != '-' || !digits(2, month) || *s++ != '-' || !digits(2, day)) fail("bad date " + raw);
    bool has_zone = false;
    int64_t offset = 0;
    if (*s) {
        ++s; // any separator, as fromisoformat allows
        if (!digits(2, hour)) fail("bad date " + raw);
        if (*s == ':') {
            ++s;
            if (!digits(2, minute)) fail("bad date " + raw);
            if (*s == ':') {
                ++s;
                if (!digits(2, second)) fail("bad date " + raw);
                if (*s == '.' || *s == ',') {
                    ++s;
                    int count = 0;
                    int64_t fraction = 0;
                    while (*s >= '0' && *s <= '9' && count < 6) fraction = fraction * 10 + (*s++ - '0'), ++count;
                    if (count != 3 && count != 6) fail("bad date " + raw);
                    while (count++ < 6) fraction *= 10;
                    micros = fraction;
                }
            }
        }
        if (*s == 'Z' || *s == '+' || *s == '-') {
            has_zone = true;
            const int sign = *s == '-' ? -1 : 1;
            if (*s++ != 'Z') {
                int zh, zm = 0;
                if (!digits(2, zh)) fail("bad date " + raw);
                if (*s == ':') ++s;
                if (*s && !digits(2, zm)) fail("bad date " + raw);
                offset = sign * (int64_t(zh) * 3600 + zm * 60) * kSecond;
            }
        }
        if (*s) fail("bad date " + raw);
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        fail("bad date " + raw);
    static const int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day > kDays[month - 1] || (month == 2 && day == 29 && !leap)) fail("bad date " + raw);
    if (!has_zone) { // a naive date is local time (datetime.astimezone)
        std::tm local{};
        local.tm_year = year - 1900;
        local.tm_mon = month - 1;
        local.tm_mday = day;
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = second;
        local.tm_isdst = -1;
        const std::time_t t = std::mktime(&local);
        if (t == -1) fail("bad date " + raw);
        return int64_t(t) * kSecond + micros;
    }
    // days_from_civil (H. Hinnant)
    const int y = year - (month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
    return ((days * 24 + hour) * 3600 + minute * 60 + second) * kSecond + micros - offset;
}

std::string format_date(int64_t micros) {
    const int64_t seconds = floor_div(micros, kSecond);
    const int64_t days = floor_div(seconds, 86400), rest = seconds - days * 86400;
    // civil_from_days (H. Hinnant)
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = int64_t(yoe) + era * 400 + (m <= 2);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02dZ", (long long)y, m, d, int(rest / 3600),
                  int(rest / 60 % 60), int(rest % 60));
    return buf;
}

Plan default_schedule_plan() {
    Plan maps = Plan::array();
    for (const auto& [name, id] : kMaps) maps.push_back(name);
    Plan plan = Plan::object();
    plan["start"] = "now";
    plan["phase_hours"] = 4;
    plan["days"] = 14;
    plan["rotation"] = Plan::array();
    plan["map_pool"] = maps;
    plan["ranked_rules"] = {"cVar", "cVlf", "cVgl"};
    plan["weapon_unlock"] = "start";
    plan["maps_open"] = maps;
    plan["available_since"] = "2015-05-28T00:00:00Z";
    return plan;
}

Plan build_schedule(const Plan& input, int64_t now) {
    const Plan plan = merged(default_schedule_plan(), input);
    const int64_t hours = to_int(plan["phase_hours"], "phase_hours");
    if (hours < 1 || hours > 24) fail("phase_hours must be 1..24");
    const int64_t phase = hours * 3600 * kSecond;
    int64_t start, first;
    if (plan["start"] == "now") { // the current phase of the shared clock
        first = floor_div(now - kEpoch, phase);
        start = kEpoch + first * phase;
    } else {
        start = parse_date(date_text(plan["start"]));
        first = 0;
    }
    const int64_t count =
        std::min<int64_t>(256, std::max<int64_t>(1, floor_div(to_int(plan["days"], "days") * 24, hours)));
    std::vector<std::string> rules;
    for (const auto& r : plan["ranked_rules"]) rules.push_back(rule(r));
    const Plan& rotation = plan["rotation"];
    const bool fixed = truthy(rotation);
    if (fixed && !rotation.is_array()) fail("rotation must be a list");

    std::vector<Plan> pool;
    if (!fixed) {
        for (const auto& m : plan["map_pool"]) pool.push_back(map_id(m));
        if (pool.size() < 4) fail("map_pool needs at least 4 maps");
        if (rules.empty()) fail("ranked_rules is empty");
    }
    Decks decks(pool);
    Plan phases = Plan::array();
    for (int64_t i = 0; i < count; ++i) {
        const int64_t g = first + i;
        Plan p;
        if (fixed) {
            p = rotation[size_t(((g % int64_t(rotation.size())) + int64_t(rotation.size())) %
                                int64_t(rotation.size()))];
            if (!p.is_object()) fail("rotation entries must be objects");
        } else {
            const int64_t per_cycle = int64_t(pool.size() / 4);
            const int64_t cycle = floor_div(g, per_cycle), slot = g - cycle * per_cycle;
            const auto& deck = decks.deck(cycle);
            p = {{"regular", {deck[size_t(4 * slot)], deck[size_t(4 * slot + 1)]}},
                 {"ranked_rule",
                  rules[size_t(((g % int64_t(rules.size())) + int64_t(rules.size())) % int64_t(rules.size()))]},
                 {"ranked", {deck[size_t(4 * slot + 2)], deck[size_t(4 * slot + 3)]}}};
        }
        Plan regular = Plan::array(), ranked = Plan::array();
        for (const auto& m : p.value("regular", Plan::array())) regular.push_back({{"MapID", map_id(m)}});
        for (const auto& m : p.value("ranked", Plan::array())) ranked.push_back({{"MapID", map_id(m)}});
        if (regular.empty() || regular.size() > 3 || ranked.size() > 3)
            fail("phase " + std::to_string(i) + ": 1-3 regular maps, 0-3 ranked maps");
        Plan entry = Plan::object();
        entry["Time"] = hours;
        entry["RegularRule"] = "cPnt";
        entry["RegularStages"] = regular;
        entry["GachiRule"] = rule(p.contains("ranked_rule") ? p["ranked_rule"] : Plan("cVar"));
        entry["GachiStages"] = ranked;
        phases.push_back(entry);
    }
    const std::string past =
        format_date(std::min(parse_date(date_text(plan["available_since"])), start - 86400 * kSecond));
    Plan unlocks = Plan::array();
    const Plan& unlock = plan["weapon_unlock"];
    if (unlock == "start") {
        for (int set : kSpotPassWeaponSets) unlocks.push_back({{"WeaponSetID", set}, {"Date", past}});
    } else {
        if (!unlock.is_object()) fail("weapon_unlock must be \"start\" or {set id: date}");
        for (const auto& [set, date] : unlock.items())
            unlocks.push_back({{"WeaponSetID", to_int(Plan(set), "weapon set id")},
                               {"Date", format_date(parse_date(date_text(date)))}});
    }
    Plan open = Plan::array();
    for (const auto& m : plan["maps_open"]) open.push_back({{"MapID", map_id(m)}, {"Date", past}});
    Plan first_rules = Plan::array();
    for (const char* r : {"cVar", "cVlf", "cVgl"}) first_rules.push_back({{"GachiRule", r}, {"Date", past}});
    Plan data = Plan::object();
    data["Version"] = 3;
    data["DateTime"] = format_date(start);
    data["Phases"] = phases;
    data["WeaponUnlock"] = unlocks;
    data["MapFirstAppear"] = open;
    data["RuleFirstAppear"] = first_rules;
    return data;
}

Plan default_festival_plan() {
    Plan plan = Plan::object();
    plan["id"] = 1000;
    plan["announce"] = "2099-01-01T00:00:00Z";
    plan["start"] = "2099-01-02T00:00:00Z";
    plan["end"] = "2099-01-03T00:00:00Z";
    plan["result"] = "2099-01-03T06:00:00Z";
    plan["rule"] = "cPnt";
    plan["stages"] = {"Fld_Crank00", "Fld_Warehouse00", "Fld_SeaPlant00"};
    Plan cats = Plan::object(), dogs = Plan::object();
    cats["name"] = {{"fr", "Chats"}, {"en", "Cats"}};
    cats["short"] = {{"fr", "Chats"}, {"en", "Cats"}};
    cats["color"] = "0.85,0.2,0.55,1";
    dogs["name"] = {{"fr", "Chiens"}, {"en", "Dogs"}};
    dogs["short"] = {{"fr", "Chiens"}, {"en", "Dogs"}};
    dogs["color"] = "0.2,0.75,0.25,1";
    plan["teams"] = {cats, dogs};
    plan["neutral_color"] = "0.5,0.5,0.5,1";
    plan["countries"] = Plan::array();
    return plan;
}

Plan build_festival(const Plan& input) {
    const Plan plan = merged(default_festival_plan(), input);
    std::vector<int64_t> dates;
    for (const char* key : {"announce", "start", "end", "result"}) {
        if (!plan[key].is_string()) fail("announce/start/end/result must be ISO dates");
        try {
            dates.push_back(parse_date(plan[key].get<std::string>()));
        } catch (const std::invalid_argument&) {
            fail("announce/start/end/result must be ISO dates");
        }
    }
    for (size_t i = 0; i + 1 < dates.size(); ++i)
        if (!(dates[i] < dates[i + 1])) fail("dates must satisfy announce < start < end < result");
    Plan stages = Plan::array();
    for (const auto& m : plan["stages"]) stages.push_back({{"MapID", map_id(m)}});
    if (stages.empty() || stages.size() > 3) fail("1-3 stages");
    const Plan& teams_in = plan["teams"];
    if (!teams_in.is_array() || teams_in.size() != 2)
        fail("exactly 2 named teams are required; the third neutral team is generated");
    Plan teams = Plan::array();
    for (size_t index = 0; index < 2; ++index) {
        const Plan& team = teams_in[index];
        const std::string label = "team " + std::to_string(index + 1);
        if (!team.is_object()) fail(label + ": expected an object");
        for (const char* field : {"name", "short"}) {
            const Plan texts = team.contains(field) ? team[field] : (team.contains("name") ? team["name"] : Plan());
            bool ok = texts.is_object() && !texts.empty();
            if (ok)
                for (const auto& text : texts)
                    ok = ok && text.is_string() &&
                         text.get<std::string>().find_first_not_of(" \t\r\n\v\f") != std::string::npos;
            if (!ok) fail(label + ": " + field + " needs nonempty localized text");
        }
        if (!team.contains("color")) fail(label + ": missing color");
    }
    for (const auto& team : teams_in) {
        Plan entry = Plan::object();
        entry["Name"] = localized(team["name"]);
        entry["ShortName"] = localized(team.contains("short") ? team["short"] : team["name"]);
        entry["Color"] = color(team["color"]);
        teams.push_back(entry);
    }
    teams.push_back({{"Color", color(plan["neutral_color"])}});
    Plan news = Plan::array();
    for (const char* type : {"Announce", "Start", "ResultA", "ResultB"}) {
        Plan item = Plan::object();
        item["NewsType"] = type;
        for (const char* region : kRegions)
            for (const char* lang : kLangs) item[std::string(region) + lang] = Plan::array();
        news.push_back(item);
    }
    Plan data = Plan::object();
    data["Version"] = 0;
    data["FestivalId"] = to_int(plan["id"], "id");
    data["Teams"] = teams;
    data["Rule"] = rule(plan["rule"]);
    data["Stages"] = stages;
    data["Time"] = {{"Announce", format_date(dates[0])},
                    {"Start", format_date(dates[1])},
                    {"End", format_date(dates[2])},
                    {"Result", format_date(dates[3])}};
    data["News"] = news;
    if (truthy(plan["countries"])) {
        Plan countries = Plan::array();
        for (const auto& c : plan["countries"]) countries.push_back(c.is_string() ? c.get<std::string>() : c.dump());
        data["Country"] = countries;
    }
    return data;
}

std::string empty_bfres(const std::string& name) {
    std::string blob(0x6C, '\0');
    const auto put = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) blob[at + i] = char(v >> (24 - 8 * i));
    };
    blob.replace(0, 4, "FRES");
    put(4, 0x03050003);
    blob[8] = char(0xFE), blob[9] = char(0xFF), blob[10] = 0, blob[11] = 0x10;
    std::string strings(4, '\0');
    for (int i = 0; i < 4; ++i) strings[i] = char(uint32_t(name.size()) >> (24 - 8 * i));
    strings += name;
    strings += '\0';
    strings.append((4 - strings.size() % 4) % 4, '\0');
    put(0x10, 0x2000);                   // file alignment
    put(0x14, 0x6C + 4 - 0x14);          // name, relative to the field
    put(0x18, uint32_t(strings.size())); // string table length / offset
    put(0x1C, 0x6C - 0x1C);
    blob += strings;
    put(0x0C, uint32_t(blob.size()));
    return blob;
}

std::vector<std::string> write_boss_files(const fs::path& boss_dir, int64_t now) {
    std::vector<std::string> problems;
    try {
        const std::string blob =
            byaml_dump(build_schedule(read_plan(boss_dir / "planning.json", default_schedule_plan()), now));
        const fs::path out = boss_dir / "schdat2" / "VSSetting.byaml";
        if (!write_file(out, boss_dir / "schdat2" / "VSSetting.tmp", blob))
            problems.push_back("cannot write VSSetting.byaml");
    } catch (const std::exception& error) {
        problems.push_back(std::string("planning.json: ") + error.what());
    }
    try {
        const Plan data = build_festival(read_plan(boss_dir / "festival.json", default_festival_plan()));
        char id[24];
        std::snprintf(id, sizeof id, "%04lld", (long long)data["FestivalId"].get<int64_t>());
        const fs::path dir = boss_dir / "optdat2";
        const std::pair<const char*, std::string> files[] = {
            {"Festival.byaml", byaml_dump(data)},
            {"HapTexture.bfres", empty_bfres(std::string("HapTexture_") + id)},
            {"PanelTexture.bfres", empty_bfres(std::string("PanelTexture_") + id)}};
        for (const auto& [name, content] : files)
            if (!write_file(dir / name, dir / (std::string(name) + ".tmp"), content))
                problems.push_back(std::string("cannot write ") + name);
    } catch (const std::exception& error) {
        problems.push_back(std::string("festival.json: ") + error.what());
    }
    return problems;
}

} // namespace rbrew_native
