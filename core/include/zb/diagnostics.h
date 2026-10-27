#pragma once

namespace zb {

// The runtime's profiling and dump tools (host-call and syscall profilers, the PC sampler, the freeze
// dump) cost a little time on every call. The "fast" launcher build turns them off at start-up;
// the "debug" build keeps them on. On unless the Java side says otherwise.
bool diagnostics_enabled();
void set_diagnostics_enabled(bool enabled);

}  // namespace zb
