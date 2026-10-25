#include "zb/external_dir.h"

#include <atomic>
#include <cstring>

#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr const char* kDefaultDirectory = "/storage/emulated/0/Android/data/com.zettabridge.launcher/files";

// Written once, before the guest runs, and read from many threads and from signal handlers, so it
// is a fixed buffer published through an atomic flag rather than a string behind a mutex.
char g_directory[512];
std::atomic<bool> g_ready{false};

}  // namespace

void set_external_files_dir(const std::string& directory) {
    std::string clean = directory;
    while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
    if (clean.empty() || clean.size() >= sizeof g_directory) return;
    if (g_ready.load(std::memory_order_acquire)) return;  // first value wins; it never changes
    std::memcpy(g_directory, clean.c_str(), clean.size() + 1);
    g_ready.store(true, std::memory_order_release);
    runtime_report().note_jni_detail("external-files-dir", clean, true);
}

std::string get_external_files_dir() {
    if (g_ready.load(std::memory_order_acquire)) return std::string(g_directory);
    return std::string(kDefaultDirectory);
}

std::string external_file_path(const char* name) {
    std::string path = get_external_files_dir();
    path += '/';
    path += name;
    return path;
}

}  // namespace zb
