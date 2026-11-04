#pragma once

#include <cstdint>
#include <string>

namespace zb {

class GuestThread;
class Process;

// Executes the arm EABI syscall described by the thread's registers (r7 = number,
// r0-r5 = arguments) and stores the result in r0 (negative errno on failure).
// Returns false when the guest process must stop (exit, exit_group, fatal signal).
bool handle_syscall(Process& proc, GuestThread& thread);

// Name of an arm EABI syscall number, or "?" if unknown.
const char* syscall_name(std::uint32_t nr);

// The last signals the guest sent to threads (who, to whom, which signal, and when), for the
// freeze report. A garbage collector that stops the world sends one to every thread.
std::string recent_guest_signals();

}  // namespace zb
