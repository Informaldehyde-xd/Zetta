#pragma once

#include <cstddef>
#include <string>

struct iovec;

namespace zb {

// The last lines the guest wrote to the Android log (through the logd socket) and to stdout/stderr.
// A game that aborts usually prints the reason first (Mono and its garbage collector do), and on a
// phone without root that line cannot be read with logcat, so the crash report carries them.
void guest_log_remember_logd_fd(int fd);
void guest_log_note_write(int fd, const iovec* vectors, int count);
void guest_log_note_write(int fd, const void* data, std::size_t size);
std::string guest_log_recent();

}  // namespace zb
