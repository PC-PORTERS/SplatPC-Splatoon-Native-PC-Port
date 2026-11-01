// nn::acp network time.
//
// Splatoon reads it once at boot (0202E78C): only when ACPGetNetworkTime
// returns 0 with its "valid" flag set does it keep the time (+0x15D = 1), then
// it converts it with ACPConvertNetworkTimeToOSCalendarTime. Both were empty
// PLT stubs, so the game never had a network time. Online mode answers with
// the host clock (UTC, the same as OSGetTime); offline keeps the old answer, as
// for a console that never synchronised. Network time here is seconds since
// 2000-01-01 00:00:00 UTC (Cemu's convention); both ends are ours.

#include "../os_common.h"
#include "stubs_addrs.h"
#include "../coreinit/coreinit_calendar.h"
#include "../../runtime/online_identity.h"
#include <chrono>

namespace {
// ACPGetNetworkTime(s64* time, u32* valid) -> ACPResult (0 = success)
void ACPGetNetworkTime(CPUState* cpu) {
    if (!rbrew_online::config().online) { RET = uint32_t(-1); return; }
    if(!ARG0 || !ARG1 || uint64_t(ARG0)+8>cpu->guest_memory_size || uint64_t(ARG1)+4>cpu->guest_memory_size || !rbrew_memory_accessible(MEM,ARG0,8) || !rbrew_memory_accessible(MEM,ARG1,4)){RET=uint32_t(-1);return;}
    using namespace std::chrono;
    const int64_t seconds = duration_cast<std::chrono::seconds>(system_clock::now().time_since_epoch()).count() - 946684800;
    if (ARG0 && rbrew_memory_accessible(MEM, ARG0, 8u)) {
        rbrew_write32(MEM, ARG0, uint32_t(uint64_t(seconds) >> 32));
        rbrew_write32(MEM, ARG0 + 4, uint32_t(seconds));
    }
    if (ARG1 && rbrew_memory_accessible(MEM, ARG1, 4u)) rbrew_write32(MEM, ARG1, 1);
    RET = 0;
}
// ACPConvertNetworkTimeToOSCalendarTime(s64 time in r3:r4, OSCalendarTime* r5)
void ACPConvertNetworkTimeToOSCalendarTime(CPUState* cpu) {
    const int64_t seconds = int64_t(uint64_t(cpu->r[3]) << 32 | cpu->r[4]);
    const uint32_t out = cpu->r[5];
    if (!out || uint64_t(out)+40>cpu->guest_memory_size || !rbrew_memory_accessible(MEM, out, 40u)) { RET = uint32_t(-1); return; }
    const auto fields = rbrew_calendar_from_seconds(seconds);
    for (unsigned i = 0; i < fields.size(); ++i) rbrew_write32(MEM, out + 4 * i, uint32_t(fields[i]));
    RET = 0;
}
}

void acp_network_time_register(CPUState* cpu) {
    rbrew_register_func(cpu, ADDR_ACPGetNetworkTime, ACPGetNetworkTime);
    rbrew_register_func(cpu, ADDR_ACPConvertNetworkTimeToOSCalendarTime, ACPConvertNetworkTimeToOSCalendarTime);
}

