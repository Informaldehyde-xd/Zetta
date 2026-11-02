#include "guest_log.h"

#include <sys/uio.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "zb/external_dir.h"

namespace zb {

namespace {

std::mutex g_mutex;
std::deque<std::string> g_lines;
std::vector<int> g_logd_fds;
constexpr std::size_t kMaxLines = 60;
constexpr std::size_t kMaxLine = 240;

bool is_logd(int fd) {
    for (int known : g_logd_fds) {
        if (known == fd) return true;
    }
    return false;
}

void push(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == '\0')) line.pop_back();
    if (line.empty()) return;
    if (line.size() > kMaxLine) line.resize(kMaxLine);
    for (char& c : line) {
        if (c == '\n') c = ' ';
    }
    g_lines.push_back(std::move(line));
    if (g_lines.size() > kMaxLines) g_lines.pop_front();
}

// A logd datagram: id(1) tid(2) seconds(4) nanoseconds(4), then priority(1), tag, NUL, message, NUL.
void note_logd(const std::string& data) {
    if (data.size() < 14) return;
    const char priority_chars[] = "??VDIWEF";
    const unsigned char priority = static_cast<unsigned char>(data[11]);
    const char* tag = data.c_str() + 12;
    const std::size_t tag_length = strnlen(tag, data.size() - 12);
    if (12 + tag_length + 1 >= data.size()) return;
    const char* message = tag + tag_length + 1;
    const std::size_t message_length = strnlen(message, data.size() - (12 + tag_length + 1));
    char head[96];
    std::snprintf(head, sizeof head, "%c/%.60s: ", priority < 8 ? priority_chars[priority] : '?', tag);
    push(std::string(head) + std::string(message, message_length));
}

}  // namespace

void guest_log_remember_logd_fd(int fd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!is_logd(fd) && g_logd_fds.size() < 16) g_logd_fds.push_back(fd);
}

void guest_log_note_write(int fd, const void* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd == 1 || fd == 2) {
        push(std::string(fd == 2 ? "stderr: " : "stdout: ") + std::string(static_cast<const char*>(data), size > kMaxLine ? kMaxLine : size));
    } else if (is_logd(fd)) {
        note_logd(std::string(static_cast<const char*>(data), size > 2048 ? 2048 : size));
    }
}

void guest_log_note_write(int fd, const iovec* vectors, int count) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (fd != 1 && fd != 2 && !is_logd(fd)) return;
    }
    std::string joined;
    for (int i = 0; i < count && joined.size() < 2048; ++i) {
        joined.append(static_cast<const char*>(vectors[i].iov_base), vectors[i].iov_len);
    }
    guest_log_note_write(fd, joined.data(), joined.size());
}

namespace {
std::vector<std::string> g_faults;
constexpr std::size_t kMaxFaults = 8;
}  // namespace

void guest_fault_note(const std::string& line) {
    std::string all;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_faults.size() >= kMaxFaults) return;
        g_faults.push_back(line);
        for (const std::string& fault : g_faults) all += fault + "\n";
    }
    if (std::FILE* file = std::fopen(external_file_path("zb-faults.txt").c_str(), "we")) {
        std::fwrite(all.data(), 1, all.size(), file);
        std::fclose(file);
    }
}

std::string guest_faults() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    for (const std::string& fault : g_faults) out += fault + "\n";
    return out;
}

std::string guest_log_recent() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    for (const std::string& line : g_lines) {
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace zb
