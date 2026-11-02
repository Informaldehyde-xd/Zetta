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

// The first guest memory faults (where, which address, which registers). Kept apart from the run report,
// whose detail list is full long before a fault happens, and written to zb-faults.txt as they come.
void guest_fault_note(const std::string& line);
std::string guest_faults();

}  // namespace zb
