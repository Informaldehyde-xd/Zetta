#pragma once

#include <cstdint>
#include <string>

namespace zb {

// "function_name+0x1c" for an address inside a 32-bit ARM library, from its symbol tables (.dynsym and,
// when the file still has it, .symtab); empty when no symbol fits. `position` is a file offset, or a
// virtual address when `is_vaddr` is true. Used to turn "libmono.so offset 0xed428" in a crash report
// into the name of the function.
std::string guest_symbol_for(const std::string& path, std::uint64_t position, bool is_vaddr);

}  // namespace zb
