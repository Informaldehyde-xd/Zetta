#include "zb/diagnostics.h"

#include <atomic>

#include "zb/runtime_report.h"

namespace zb {

namespace {
std::atomic<bool> g_enabled{true};
}

bool diagnostics_enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_diagnostics_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    runtime_report().note_jni_detail("diagnostics", enabled ? "on (debug build)" : "off (fast build)", true);
}

}  // namespace zb
