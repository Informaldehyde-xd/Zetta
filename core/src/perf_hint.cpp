#include "zb/perf_hint.h"

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "zb/log.h"
#include "zb/options.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

using GetManagerFn = void* (*)();
using CreateSessionFn = void* (*)(void*, const std::int32_t*, std::size_t, std::int64_t);
using ReportFn = int (*)(void*, std::int64_t);
using CloseFn = void (*)(void*);

constexpr std::int64_t kTargetNanos = 16666667;  // one frame at 60 fps

bool read_thread(int tid, unsigned long& ticks, std::string& name) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    char buffer[1024];
    std::FILE* file = std::fopen(path, "re");
    if (file == nullptr) return false;
    const std::size_t got = std::fread(buffer, 1, sizeof buffer - 1, file);
    std::fclose(file);
    buffer[got] = 0;
    const char* open_paren = std::strchr(buffer, '(');
    const char* close_paren = std::strrchr(buffer, ')');
    if (open_paren == nullptr || close_paren == nullptr || close_paren < open_paren) return false;
    name.assign(open_paren + 1, close_paren);
    unsigned long user = 0, system = 0;
    if (std::sscanf(close_paren + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &user, &system) != 2) return false;
    ticks = user + system;
    return true;
}

void run() {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (lib == nullptr) return;
    auto get_manager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
    auto create = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession"));
    auto report = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    auto close_session = reinterpret_cast<CloseFn>(dlsym(lib, "APerformanceHint_closeSession"));
    if (get_manager == nullptr || create == nullptr || report == nullptr || close_session == nullptr) {
        log("perf hint: this Android has no PerformanceHint API (needs Android 13)");
        return;
    }
    void* manager = get_manager();
    if (manager == nullptr) {
        log("perf hint: no manager");
        return;
    }
    NativeCallCounter& frame_counter = runtime_report().native_call_counter("nativeRender");
    const long clock_ticks = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;

    std::unordered_map<int, unsigned long> last_ticks;
    std::vector<std::int32_t> session_tids;
    void* session = nullptr;
    auto last_time = std::chrono::steady_clock::now();
    std::uint64_t last_frames = frame_counter.count.load(std::memory_order_relaxed);
    log("perf hint: started");

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last_time).count();
        last_time = now;

        struct Busy {
            double percent;
            std::int32_t tid;
        };
        std::vector<Busy> busy;
        if (DIR* dir = opendir("/proc/self/task")) {
            while (dirent* entry = readdir(dir)) {
                const int tid = std::atoi(entry->d_name);
                if (tid <= 0) continue;
                unsigned long ticks = 0;
                std::string name;
                if (!read_thread(tid, ticks, name)) continue;
                const unsigned long before = last_ticks.count(tid) ? last_ticks[tid] : ticks;
                last_ticks[tid] = ticks;
                const double percent = 100.0 * (ticks - std::min(ticks, before)) / (static_cast<double>(clock_ticks) * seconds);
                // sound mixing and the Java collector do not need a faster core
                if (percent >= 40.0 && name.compare(0, 4, "FMOD") != 0 && name.compare(0, 4, "Heap") != 0)
                    busy.push_back({percent, tid});
            }
            closedir(dir);
        }
        std::sort(busy.begin(), busy.end(), [](const Busy& a, const Busy& b) { return a.percent > b.percent; });
        if (busy.size() > 3) busy.resize(3);
        std::vector<std::int32_t> wanted;
        for (const Busy& b : busy) wanted.push_back(b.tid);
        std::sort(wanted.begin(), wanted.end());

        if (wanted != session_tids) {
            if (session != nullptr) close_session(session);
            session = nullptr;
            if (!wanted.empty()) session = create(manager, wanted.data(), wanted.size(), kTargetNanos);
            session_tids = wanted;
            std::string list;
            for (std::int32_t t : wanted) list += (list.empty() ? "" : " ") + std::to_string(t);
            log("perf hint: hot threads now [%s]%s", list.c_str(), wanted.empty() ? "" : (session != nullptr ? "" : " (session refused)"));
        }
        const std::uint64_t frames = frame_counter.count.load(std::memory_order_relaxed);
        if (session != nullptr) {
            // Tell the system how long a frame really takes while a hot thread is working flat out. A frame
            // that takes longer than the target is the signal to boost; otherwise report "on time".
            std::int64_t actual = kTargetNanos;
            if (!busy.empty() && busy[0].percent >= 70.0 && frames > last_frames) {
                const double frame_ms = 1000.0 * seconds / static_cast<double>(frames - last_frames);
                actual = static_cast<std::int64_t>(std::min(100.0, std::max(frame_ms, 16.7)) * 1.0e6);
            } else if (!busy.empty() && busy[0].percent >= 70.0) {
                actual = kTargetNanos * 2;  // a hot thread and no frame at all
            }
            report(session, actual);
        }
        last_frames = frames;
    }
}

}  // namespace

void start_perf_hint() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!option_enabled("perf_hint", true)) return;
        std::thread(run).detach();
    });
}

}  // namespace zb
