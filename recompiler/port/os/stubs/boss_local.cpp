// nn_boss (SpotPass) served from local files.
//
// At boot Splatoon runs its NBDL tasks "schdat2" and "optdat2", then reads every
// file of the storages of the same names and hands it to BossData_dispatch (02003D7C) by
// name: VSSetting.byaml (stage schedule, weapon unlock dates; the shops stay
// "offline" without it), Festival.byaml, HapTexture.bfres, PanelTexture.bfres.
// Nintendo's servers are gone, so in online mode the storage is the folder
// boss/<task id>/ next to the executable (tools/boss/make_vssetting.py writes
// it). Offline, nothing changes: the storage does not exist, as before.
//
// ABI (this = r3): Storage::Initialize(this, taskId, kind), Storage::Exist,
// Storage::GetDataList(this, DataName[32-byte] names, max, u32* count, offset),
// NsData::Initialize(this, const Storage&, name), NsData::Exist,
// NsData::GetSize -> u64 in r3:r4, NsData::Read(this, buffer, size) -> Result.
// nn::Result 0 is success.

#include "../os_common.h"
#include "stubs_addrs.h"
#include "../../runtime/online_identity.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
namespace fs = std::filesystem;

std::mutex s_lock;
std::unordered_map<uint32_t, std::string> s_storages;   // Storage* -> task id
std::unordered_map<uint32_t, fs::path> s_data;          // NsData* -> file

bool guest_range(CPUState* cpu,uint64_t address,uint64_t size){return address && address+size<=cpu->guest_memory_size && rbrew_memory_accessible(cpu->mem,uint32_t(address),size);}
std::string guest_string(CPUState* cpu, uint32_t address, size_t max) {
    std::string out;
    while (address && out.size() < max && guest_range(cpu, address, 1u) && MEM[address]) out += char(MEM[address++]);
    return address && guest_range(cpu,address,1) && !MEM[address] ? out : std::string();
}
// Task ids and data names are plain identifiers; anything else never maps to a path.
bool plain_name(const std::string& name) {
    return !name.empty() && name.size() < 32 && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.'; });
}
// RBREW_BOSS_DIR overrides the folder (benchmark runs use their own copy of the exe).
fs::path storage_dir(const std::string& task) {
    static const fs::path root = [] {
#if defined(_WIN32)
        const wchar_t* dir = _wgetenv(L"RBREW_BOSS_DIR");
#else
        const char* dir = std::getenv("RBREW_BOSS_DIR");
#endif
        return dir && *dir ? fs::path(dir) : rbrew_online_cfg::settings_path().parent_path() / "boss";
    }();
    return root / task;
}
bool enabled() { static const bool on = rbrew_online::config().online; return on; }

std::vector<std::string> data_names(const std::string& task) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(storage_dir(task),ec),end;
    if(ec)return {};
    while(it!=end) {
        const bool regular=it->is_regular_file(ec);
        if(ec)return {};
        if(regular) {
            // BOSS names are ASCII identifiers: avoid throwing Unicode conversion.
            const auto native=it->path().filename().native();
            if(std::all_of(native.begin(),native.end(),[](auto c){return uint32_t(c)<128;})) {
                const std::string name(native.begin(),native.end());
                if(plain_name(name))names.push_back(name);
            }
        }
        it.increment(ec);
        if(ec)return {};
    }
    std::sort(names.begin(), names.end());
    return names;
}

void StorageInitialize(CPUState* cpu) {
    const std::string task = guest_string(cpu, ARG1, 31);
    std::lock_guard lock(s_lock);
    if (plain_name(task)) s_storages[ARG0] = task; else s_storages.erase(ARG0);
    RET = 0;
}
void StorageExist(CPUState* cpu) {
    std::string task;
    { std::lock_guard lock(s_lock); auto it = s_storages.find(ARG0); if (it != s_storages.end()) task = it->second; }
    std::error_code ec;
    RET = enabled() && !task.empty() && fs::is_directory(storage_dir(task), ec) ? 1u : 0u;
    rbrew_net_note("boss storage", (task + (RET ? " present" : " absent")).c_str());
}
void StorageGetDataList(CPUState* cpu) {
    const uint32_t names = ARG1, max = ARG2, count_out = ARG3, offset = ARG4;
    std::string task;
    { std::lock_guard lock(s_lock); auto it = s_storages.find(ARG0); if (it != s_storages.end()) task = it->second; }
    uint32_t written = 0;
    if (enabled() && !task.empty()) {
        const auto list = data_names(task);
        for (size_t i = offset; i < list.size() && written < max; ++i, ++written) {
            const uint64_t slot = uint64_t(names) + uint64_t(written) * 32u;
            if (!guest_range(cpu, slot, 32u)) break;
            std::memset(MEM + slot, 0, 32);
            std::memcpy(MEM + slot, list[i].data(), list[i].size());
            rbrew_net_note("boss data", (task + "/" + list[i]).c_str());
        }
    }
    if (count_out && guest_range(cpu, count_out, 4u)) rbrew_write32(MEM, count_out, written);
    RET = 0;
}
// The game reads the data name back from the NsData object itself: the loop in
// BossTask_readStorage (0285ECF4) takes NsData+0 as a C string and matches it
// against "VSSetting.byaml"... Left empty, nothing was ever read. The object
// is 0x58 bytes on the game's stack; the name takes the first 32.
void NsDataInitialize(CPUState* cpu) {
    const std::string name = guest_string(cpu, ARG2, 31);
    if (ARG0 && guest_range(cpu, ARG0, 32u)) {
        std::memset(MEM + ARG0, 0, 32);
        std::memcpy(MEM + ARG0, name.data(), name.size());
    }
    std::lock_guard lock(s_lock);
    auto it = s_storages.find(ARG1);
    if (it != s_storages.end() && plain_name(name)) s_data[ARG0] = storage_dir(it->second) / name;
    else s_data.erase(ARG0);
    RET = 0;
}
fs::path data_file(uint32_t self) {
    std::lock_guard lock(s_lock);
    auto it = s_data.find(self);
    return it == s_data.end() ? fs::path() : it->second;
}
void NsDataExist(CPUState* cpu) {
    std::error_code ec;
    const fs::path file = data_file(ARG0);
    RET = enabled() && !file.empty() && fs::is_regular_file(file, ec) ? 1u : 0u;
}
void NsDataGetSize(CPUState* cpu) {
    std::error_code ec;
    const fs::path file = data_file(ARG0);
    const uint64_t size = enabled() && !file.empty() ? uint64_t(fs::file_size(file, ec)) : 0;
    cpu->r[3] = uint32_t((ec ? 0 : size) >> 32);
    cpu->r[4] = uint32_t(ec ? 0 : size);
}
void NsDataRead(CPUState* cpu) {
    const uint32_t buffer = ARG1, size = ARG2;
    const fs::path file = data_file(ARG0);
    std::ifstream in(file, std::ios::binary);
    if (!enabled() || file.empty() || !in || (size && !guest_range(cpu, buffer, size))) { RET = 0xC0000000u; return; }
    in.read(reinterpret_cast<char*>(MEM + buffer), size);
    RET = in && uint64_t(in.gcount())==size ? 0u : 0xC0000000u;
}
// The NBDL task itself "completes" at once: its data is already on disk.
void TaskWait(CPUState* cpu)           { RET = 1; }
void TaskGetTurnState(CPUState* cpu)   { if (ARG1 && guest_range(cpu, ARG1, 4u)) rbrew_write32(MEM, ARG1, 0); RET = 0x10; }   // done (0x11 = done with error)
void TaskGetHttpStatusCode(CPUState* cpu) { if (ARG1 && guest_range(cpu, ARG1, 4u)) rbrew_write32(MEM, ARG1, 0); RET = 200; }
}

void boss_local_register(CPUState* cpu) {
    rbrew_register_func(cpu, ADDR_Initialize__Q3_2nn4boss7StorageFPCcQ3_2nn4boss11StorageKind, StorageInitialize);
    rbrew_register_func(cpu, ADDR_Exist__Q3_2nn4boss7StorageCFv, StorageExist);
    rbrew_register_func(cpu, ADDR_GetDataList__Q3_2nn4boss7StorageCFPQ3_2nn4boss8DataNameUiPUiT2, StorageGetDataList);
    rbrew_register_func(cpu, ADDR_Initialize__Q3_2nn4boss6NsDataFRCQ3_2nn4boss7StoragePCc, NsDataInitialize);
    rbrew_register_func(cpu, ADDR_Exist__Q3_2nn4boss6NsDataCFv, NsDataExist);
    rbrew_register_func(cpu, ADDR_GetSize__Q3_2nn4boss6NsDataCFv, NsDataGetSize);
    rbrew_register_func(cpu, ADDR_Read__Q3_2nn4boss6NsDataFPvUi, NsDataRead);
    rbrew_register_func(cpu, ADDR_Wait__Q3_2nn4boss4TaskFUiQ3_2nn4boss13TaskWaitState, TaskWait);
    rbrew_register_func(cpu, ADDR_GetTurnState__Q3_2nn4boss4TaskCFPUi, TaskGetTurnState);
    rbrew_register_func(cpu, ADDR_GetHttpStatusCode__Q3_2nn4boss4TaskCFPUi, TaskGetHttpStatusCode);
}



