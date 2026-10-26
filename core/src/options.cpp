#include "zb/options.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "zb/external_dir.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

struct Entry {
    std::string name;
    bool value;
};

std::mutex g_mutex;
std::vector<Entry> g_entries;
bool g_loaded = false;
std::vector<std::string> g_used;  // "name=value" for the run report

void load_locked() {
    g_loaded = true;
    std::FILE* file = std::fopen(external_file_path("zb-options.txt").c_str(), "re");
    if (file == nullptr) return;
    char line[256];
    while (std::fgets(line, sizeof line, file) != nullptr) {
        char* text = line;
        while (*text == ' ' || *text == '\t') ++text;
        if (*text == '#' || *text == '\n' || *text == '\0' || *text == '\r') continue;
        char* equals = std::strchr(text, '=');
        if (equals == nullptr) continue;
        std::string name(text, equals);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        const char* value = equals + 1;
        while (*value == ' ' || *value == '\t') ++value;
        g_entries.push_back({name, *value != '0' && *value != 'n' && *value != 'f'});
    }
    std::fclose(file);
}

}  // namespace

bool option_enabled(const char* name, bool default_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_loaded) load_locked();
    bool value = default_value;
    for (const Entry& entry : g_entries) {
        if (entry.name == name) value = entry.value;  // the last line wins
    }
    g_used.push_back(std::string(name) + "=" + (value ? "1" : "0"));
    std::string all;
    for (const std::string& item : g_used) {
        if (!all.empty()) all += ' ';
        all += item;
    }
    runtime_report().note_jni_detail("options", all, true);
    return value;
}

}  // namespace zb
