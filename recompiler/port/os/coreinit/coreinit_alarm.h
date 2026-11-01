#pragma once
// Testable pieces of coreinit alarms and the GHS C library locks.
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <limits>

// OSAlarm (0x58 bytes; Cemu coreinit_Alarm.h): tag 'aLrM' at +0, handler
// +0x0C, group +0x10, next fire +0x18, period +0x28, user data +0x38.
constexpr uint32_t kOSAlarmSize = 0x58, kOSAlarmTag = 0x614C724Du;
constexpr uint32_t kOSAlarmHandler = 0x0C, kOSAlarmGroup = 0x10, kOSAlarmNext = 0x18,
                   kOSAlarmPeriod = 0x28, kOSAlarmUserData = 0x38;

// First fire of a periodic alarm started at `start` (OS ticks): `start` itself
// when still ahead, else the next period boundary after `now`.
constexpr uint64_t rbrew_alarm_add(uint64_t a,uint64_t b) {return b>UINT64_MAX-a?UINT64_MAX:a+b;}
constexpr uint64_t rbrew_alarm_first_periodic(uint64_t start,uint64_t period,uint64_t now) {
    if(start>=now || !period)return start;
    const uint64_t skipped=(now-start)/period;
    if(skipped==UINT64_MAX || skipped+1>(UINT64_MAX-start)/period)return UINT64_MAX;
    return start+(skipped+1)*period;
}
constexpr uint64_t rbrew_alarm_next_periodic(uint64_t due,uint64_t period,uint64_t now) {
    if(!period)return UINT64_MAX;
    const uint64_t next=rbrew_alarm_add(due,period);
    return next>now || next==UINT64_MAX?next:rbrew_alarm_first_periodic(due,period,now);
}
// Recursive lock for guest code (OSMutex semantics: the owner may relock).
// Unlocking from a thread that does not own it is ignored and reported,
// where std::recursive_mutex would be undefined behaviour.
class GuestRecursiveLock {
    std::mutex mutex_;
    std::condition_variable released_;
    std::thread::id owner_{};
    unsigned depth_ = 0;
public:
    void lock() {
        const auto me = std::this_thread::get_id();
        std::unique_lock guard(mutex_);
        if (depth_ && owner_ == me) { ++depth_; return; }
        released_.wait(guard, [&] { return depth_ == 0; });
        owner_ = me; depth_ = 1;
    }
    bool unlock() {
        const auto me = std::this_thread::get_id();
        std::lock_guard guard(mutex_);
        if (!depth_ || owner_ != me) return false;
        if (--depth_ == 0) { owner_ = {}; released_.notify_one(); }
        return true;
    }
    unsigned depth() { std::lock_guard guard(mutex_); return depth_; }
};
