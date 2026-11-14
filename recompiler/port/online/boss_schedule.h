// boss_schedule.h - the SpotPass files the game reads in online mode (boss_local.cpp), built in the game itself so
// a player without Python gets the same rotation as everybody else. Ports of tools/boss/make_vssetting.py and
// make_festival.py: same plans, same bytes (tools/tests/test_online_native.py compares them).
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rbrew_native {

using Plan = nlohmann::ordered_json;

int64_t unix_micros_now();
int64_t parse_date(const std::string& text); // ISO date as make_vssetting.parse_date reads it -> UTC microseconds
std::string format_date(int64_t micros);     // "YYYY-MM-DDTHH:MM:SSZ"

Plan default_schedule_plan();
Plan default_festival_plan();
// Both throw std::invalid_argument with the Python tool's reason when the plan is wrong.
Plan build_schedule(const Plan& plan, int64_t now_micros);
Plan build_festival(const Plan& plan);
std::string empty_bfres(const std::string& name);

// Game/boss: planning.json -> schdat2/VSSetting.byaml, festival.json -> optdat2/ (created with the defaults when
// missing). Returns one message per file that could not be made; the previous file then stays.
std::vector<std::string> write_boss_files(const std::filesystem::path& boss_dir, int64_t now_micros);

} // namespace rbrew_native
