// The date and time std::format("{0:%F} {0:%T}", time) gives with MSVC's
// library for a time of the system clock, which Norbyte's code shows to
// scripts (Lua/Libs/Timer.inl): "2026-11-11 16:04:27.1234567", in UTC.
//
// The seconds carry the fraction in the clock's ticks, and MSVC's system
// clock counts 100 nanoseconds (GetSystemTimePreciseAsFileTime): seven
// digits, the last one as live as the others. This system's clock and its
// std::chrono::system_clock count microseconds, so the time is taken with
// PreciseSystemTime below.
//
// std::format itself is not used: in the C++ library of macOS it needs
// macOS 13.3, and the game runs on macOS 11.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <time.h>

namespace bg3se::mac {

// MSVC's system_clock::duration.
using ClockTicks = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;

inline std::string ClockText(ClockTicks sinceEpoch)
{
    // Every name with its namespace: Norbyte's code, which includes this, has names of its own like these.
    auto day = std::chrono::floor<std::chrono::days>(sinceEpoch);
    std::chrono::year_month_day date{ std::chrono::sys_days{ day } };
    std::chrono::hh_mm_ss<ClockTicks> ofDay{ sinceEpoch - day };
    char text[48];
    snprintf(text, sizeof(text), "%04d-%02u-%02u %02d:%02d:%02d.%07lld", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()), static_cast<int>(ofDay.hours().count()), static_cast<int>(ofDay.minutes().count()),
        static_cast<int>(ofDay.seconds().count()), static_cast<long long>(ofDay.subseconds().count()));
    return text;
}

inline std::string ClockText(std::chrono::system_clock::time_point time)
{
    return ClockText(std::chrono::duration_cast<ClockTicks>(time.time_since_epoch()));
}

// The system clock in 100 ns ticks. The system's clock gives the time to
// the microsecond. Where in its microsecond a reading was taken is found
// from the moment the system clock next goes on to the following one, seen
// on the monotonic clock: the reading was taken that long before the start
// of that microsecond. Each reading waits for that change, less than a
// microsecond.

// The time of a reading `read` of the system clock, from the change of the
// system clock that was seen after it: to `edge`, the start of a
// microsecond, `elapsedNs` after the reading. Always within the microsecond
// read: the system clock may have been set in between.
inline ClockTicks TimeBeforeChange(ClockTicks read, ClockTicks edge, uint64_t elapsedNs)
{
    constexpr ClockTicks kLastTick{ 9 };     // the last 100 ns tick of the microsecond read
    ClockTicks time = edge - ClockTicks{ static_cast<long long>((elapsedNs + 99) / 100) };
    if (time < read) return read;
    if (time > read + kLastTick) return read + kLastTick;
    return time;
}

// What MSVC's std::chrono::system_clock::now() gives, for a reading `now`
// of this system's taken just before: the same time, in 100 ns ticks. When
// no change is seen closely enough within some microseconds (the thread
// preempted at every one), the microsecond read.
inline ClockTicks PreciseSystemTime(std::chrono::system_clock::time_point now)
{
    constexpr uint64_t kCloseNs = 250;          // the most two readings of the system clock may be apart around the change
    constexpr uint64_t kGiveUpNs = 20'000;
    auto system = [] {
        return std::chrono::duration_cast<ClockTicks>(std::chrono::system_clock::now().time_since_epoch());
    };
    ClockTicks read = std::chrono::duration_cast<ClockTicks>(now.time_since_epoch());
    uint64_t called = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    // The system clock's last reading, and the monotonic clock's just
    // before and just after it.
    ClockTicks last = read;
    uint64_t lastBefore = called, lastAfter = called;
    while (true) {
        uint64_t before = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        ClockTicks seen = system();
        uint64_t after = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        if (seen != last) {
            // The change was between the two readings: taken as half way.
            if (after - lastBefore <= kCloseNs) {
                uint64_t changeNs = (lastBefore + lastAfter + before + after) / 4;
                return TimeBeforeChange(read, seen, changeNs - called);
            }
            last = seen;
        }
        if (after - called > kGiveUpNs) return read;
        lastBefore = before;
        lastAfter = after;
    }
}

}
