#pragma once

#include <cstdint>

#include "zb/guest_thread.h"

namespace zb {

class LibraryRuntime;

// Native (arm64) zlib for the guest's inflate family, uncompress, crc32 and adler32.
// Switch: native_zlib=0 in zb-options.txt makes the guest use its own translated libz again.
class HostZlib {
public:
    explicit HostZlib(LibraryRuntime& runtime) : runtime_(runtime) {}
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    LibraryRuntime& runtime_;
};

}  // namespace zb
