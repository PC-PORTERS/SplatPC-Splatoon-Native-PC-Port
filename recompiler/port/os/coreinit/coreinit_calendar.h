#pragma once
// Seconds since 2000-01-01 00:00:00 UTC -> Cafe OSCalendarTime fields.
// OSCalendarTime: sec, min, hour, mday (1-31), mon (0-11), year, wday (0 = Sunday), yday (0-365),
// msec, usec; ten big-endian int32 in guest memory.
#include <array>
#include <cstdint>

inline std::array<int32_t, 10> rbrew_calendar_from_seconds(int64_t seconds_since_2000, int32_t usec = 0) {
    constexpr int64_t kDay = 86400;
    int64_t days = seconds_since_2000 / kDay, rest = seconds_since_2000 % kDay;
    if (rest < 0) { rest += kDay; --days; }
    // Civil date from days since 1970 (H. Hinnant's days_from_civil inverse).
    const int64_t z = days + 10957 + 719468;           // 2000-01-01 is day 10957 after 1970
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t mday = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;      // 1-12
    const int64_t year = yoe + era * 400 + (month <= 2);
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static constexpr int kBefore[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int64_t yday = kBefore[month - 1] + mday - 1 + (leap && month > 2);
    const int64_t wday = ((days + 6) % 7 + 7) % 7;     // 2000-01-01 was a Saturday
    return {int32_t(rest % 60), int32_t(rest / 60 % 60), int32_t(rest / 3600), int32_t(mday), int32_t(month - 1),
            int32_t(year), int32_t(wday), int32_t(yday), usec / 1000, usec % 1000};
}
