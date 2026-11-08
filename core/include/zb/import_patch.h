#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zb {

// Guest memory as the patcher needs it, so the ELF walk can be tested on a plain buffer.
struct ImportPatchMemory {
    // a pointer to `len` readable bytes at the guest address, or nullptr
    std::function<const std::uint8_t*(std::uint32_t addr, std::uint32_t len)> read;
    // store one 32-bit word (opening a read-only page for the write if needed); false when it could not
    std::function<bool(std::uint32_t addr, std::uint32_t value)> write32;
};

struct ImportPatchStats {
    unsigned imports_seen = 0;  // undefined function symbols of the library that are on the target list
    unsigned patched = 0;       // table slots changed in this pass
    unsigned already = 0;       // slots that already held the target
    unsigned failed = 0;        // slots that could not be written
    std::vector<std::pair<std::uint32_t, std::uint32_t>> slots;  // (slot address, target) for every slot found
};

// Walks one 32-bit ARM shared object loaded at `base` and points each of its imports (GOT and PLT slots)
// that is named in `targets` at the target address. Imports are what the game's own code calls
// (memcpy, inflate, ...), so this works whatever order the linker looks symbols up in.
ImportPatchStats patch_library_imports(std::uint32_t base, const ImportPatchMemory& memory,
                                       const std::unordered_map<std::string, std::uint32_t>& targets);

// Re-checks the slots found earlier for one library (a lazy binding or a late relocation may have
// overwritten one): cheap, no ELF walk. Returns how many slots had to be written again.
unsigned recheck_import_slots(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& slots, const ImportPatchMemory& memory);

// The functions the guest asks to have patched in (name -> guest address). Set by the guest's
// libzbcompat through a host call; read by Process::patch_imports.
void register_patch_targets(const std::unordered_map<std::string, std::uint32_t>& targets);
std::unordered_map<std::string, std::uint32_t> patch_targets();

}  // namespace zb
