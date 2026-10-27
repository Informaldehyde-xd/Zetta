#include "profile_extra.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "zb/external_dir.h"

namespace zb {

namespace {

constexpr std::size_t kSlots = 1024;
std::atomic<std::uint64_t> g_calls[kSlots];
std::atomic<std::uint64_t> g_ticks[kSlots];

struct Named {
    std::uint32_t nr;
    const char* name;
};

// ARM EABI system call numbers the games use most; others print as a number.
constexpr Named kNames[] = {
    {3, "read"},           {4, "write"},          {5, "open"},           {6, "close"},
    {19, "lseek"},         {45, "brk"},           {54, "ioctl"},         {55, "fcntl"},
    {78, "gettimeofday"},  {85, "readlink"},      {91, "munmap"},        {122, "uname"},
    {125, "mprotect"},     {158, "sched_yield"},  {162, "nanosleep"},    {163, "mremap"},
    {168, "poll"},         {172, "prctl"},        {174, "rt_sigaction"}, {175, "rt_sigprocmask"},
    {180, "pread64"},      {181, "pwrite64"},     {186, "sigaltstack"},  {192, "mmap2"},
    {195, "stat64"},       {197, "fstat64"},      {217, "getdents64"},   {220, "madvise"},
    {224, "gettid"},       {240, "futex"},        {251, "epoll_wait"},   {256, "set_tid_address"},
    {263, "clock_gettime"},{265, "clock_nanosleep"}, {268, "tgkill"},    {322, "openat"},
    {336, "ppoll"},        {346, "epoll_pwait"},
};

const char* syscall_name(std::uint32_t nr) {
    for (const Named& n : kNames) {
        if (n.nr == nr) return n.name;
    }
    return nullptr;
}

void write_file(const std::string& path, const std::string& text) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    [[maybe_unused]] const ssize_t written = ::write(fd, text.data(), text.size());
    ::close(fd);
}

std::uint64_t frequency() {
#if defined(__aarch64__)
    std::uint64_t value;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
#else
    return 1000000000ull;
#endif
}

// "VmRSS: 123 kB" lines of /proc/self/status, in the order worth reading.
std::string memory_text() {
    std::string out;
    std::FILE* file = std::fopen("/proc/self/status", "re");
    if (file == nullptr) return "cannot read /proc/self/status\n";
    char line[256];
    static const char* const wanted[] = {"VmPeak", "VmSize", "VmRSS", "RssAnon", "RssFile", "RssShmem", "VmSwap", "Threads"};
    while (std::fgets(line, sizeof line, file) != nullptr) {
        for (const char* key : wanted) {
            const std::size_t length = std::strlen(key);
            if (std::strncmp(line, key, length) == 0 && line[length] == ':') out += line;
        }
    }
    std::fclose(file);
    // Total of the big anonymous mappings (game heap, JIT code, graphics driver): the 12 largest by resident size.
    file = std::fopen("/proc/self/smaps", "re");
    if (file != nullptr) {
        struct Region {
            std::string name;
            std::uint64_t rss_kb;
        };
        std::vector<Region> regions;
        std::string current;
        while (std::fgets(line, sizeof line, file) != nullptr) {
            unsigned long long a, b;
            char perms[8], name[200];
            name[0] = '\0';
            if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %199[^\n]", &a, &b, perms, name) >= 3 && std::strchr(line, '-') != nullptr &&
                std::strlen(line) > 20 && line[0] != 'R' && ((line[0] >= '0' && line[0] <= '9') || (line[0] >= 'a' && line[0] <= 'f'))) {
                char label[300];
                std::snprintf(label, sizeof label, "%llx-%llx %s %s (%llu MB mapped)", a, b, perms, name[0] ? name : "[anonymous]",
                              (b - a) >> 20);
                current = label;
            } else {
                unsigned long long kb;
                if (std::sscanf(line, "Rss: %llu kB", &kb) == 1 && kb >= 4096) regions.push_back({current, kb});
            }
        }
        std::fclose(file);
        std::sort(regions.begin(), regions.end(), [](const Region& x, const Region& y) { return x.rss_kb > y.rss_kb; });
        out += "\nlargest resident regions:\n";
        for (std::size_t i = 0; i < regions.size() && i < 12; ++i) {
            char row[400];
            std::snprintf(row, sizeof row, "  %6llu MB  %s\n", static_cast<unsigned long long>(regions[i].rss_kb >> 10), regions[i].name.c_str());
            out += row;
        }
    }
    return out;
}

}  // namespace

void account_syscall(std::uint32_t nr, std::uint64_t ticks) {
    const std::size_t slot = nr < kSlots ? nr : kSlots - 1;
    g_calls[slot].fetch_add(1, std::memory_order_relaxed);
    g_ticks[slot].fetch_add(ticks, std::memory_order_relaxed);
}

void start_extra_profiler() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            const double hz = static_cast<double>(frequency());
            std::vector<std::uint64_t> seen_calls(kSlots, 0), seen_ticks(kSlots, 0);
            auto window_start = std::chrono::steady_clock::now();
            std::uint64_t busiest = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                const auto now = std::chrono::steady_clock::now();
                const double window_ms = std::chrono::duration<double, std::milli>(now - window_start).count();
                window_start = now;

                struct Row {
                    std::uint32_t nr;
                    std::uint64_t calls, ticks;
                };
                std::vector<Row> rows;
                std::uint64_t total_calls = 0, total_ticks = 0;
                for (std::size_t i = 0; i < kSlots; ++i) {
                    const std::uint64_t calls = g_calls[i].load(std::memory_order_relaxed);
                    const std::uint64_t ticks = g_ticks[i].load(std::memory_order_relaxed);
                    const std::uint64_t dc = calls - seen_calls[i], dt = ticks - seen_ticks[i];
                    seen_calls[i] = calls;
                    seen_ticks[i] = ticks;
                    if (dc == 0) continue;
                    rows.push_back({static_cast<std::uint32_t>(i), dc, dt});
                    total_calls += dc;
                    total_ticks += dt;
                }
                write_file(external_file_path("zb-memory.txt"), memory_text());
                if (total_calls < 100) continue;
                std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.ticks > y.ticks; });
                std::string text;
                char line[300];
                std::snprintf(line, sizeof line,
                              "window %.2f s: %llu guest system calls (%.0f per second), %.1f ms inside them\n"
                              "(waiting counts: a thread sleeping in futex or poll shows its whole sleep, which is not CPU time)\n\n",
                              window_ms / 1000.0, static_cast<unsigned long long>(total_calls),
                              1000.0 * static_cast<double>(total_calls) / window_ms,
                              1000.0 * static_cast<double>(total_ticks) / hz);
                text += line;
                std::snprintf(line, sizeof line, "%-4s %-18s %10s %10s %9s\n", "#", "system call", "calls", "ms", "avg us");
                text += line;
                for (std::size_t r = 0; r < rows.size() && r < 20; ++r) {
                    char label[48];
                    if (const char* name = syscall_name(rows[r].nr)) std::snprintf(label, sizeof label, "%s", name);
                    else std::snprintf(label, sizeof label, "nr %u", rows[r].nr);
                    const double ms = 1000.0 * static_cast<double>(rows[r].ticks) / hz;
                    std::snprintf(line, sizeof line, "%-4zu %-18s %10llu %10.1f %9.2f\n", r + 1, label,
                                  static_cast<unsigned long long>(rows[r].calls), ms,
                                  1000.0 * ms / static_cast<double>(rows[r].calls));
                    text += line;
                }
                write_file(external_file_path("zb-syscalls.txt"), text);
                if (total_calls > busiest) {
                    busiest = total_calls;
                    write_file(external_file_path("zb-syscalls-busiest.txt"), text);
                }
            }
        }).detach();
    });
}

}  // namespace zb
