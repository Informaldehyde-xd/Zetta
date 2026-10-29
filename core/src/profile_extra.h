#pragma once

#include <chrono>
#include <cstdint>

namespace zb {

inline std::uint64_t profile_ticks() {
#if defined(__aarch64__)
    std::uint64_t value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Counts one guest system call and the time it took (waiting included).
void account_syscall(std::uint32_t nr, std::uint64_t ticks);

// Notes a guest system call that failed (errno `err`, positive). `subject` is the path or the
// arguments it was called with, when there are any. The first failures of each kind, and their
// running count, go to the run report as "syscall-fail-N" lines: a phone that refuses something an
// ordinary phone allows (a permission, a missing file, a blocked call) shows up there, in every build.
void account_syscall_failure(std::uint32_t nr, const char* name, int err, const char* subject);

// Starts, once, a background thread that every 5 seconds writes zb-syscalls.txt (which system
// calls the guest spends its time in) and zb-memory.txt (where the memory of the process is).
void start_extra_profiler();

}  // namespace zb
