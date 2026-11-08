#include "zb/import_patch.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace zb {

namespace {

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

constexpr std::uint32_t kPtLoad = 1, kPtDynamic = 2;
constexpr std::uint32_t kDtNull = 0, kDtPltRelSz = 2, kDtStrTab = 5, kDtSymTab = 6, kDtRel = 17, kDtRelSz = 18, kDtJmpRel = 23;
constexpr std::uint32_t kRArmGlobDat = 21, kRArmJumpSlot = 22;

// the C string at a guest address, at most 40 characters (every name we look for is shorter)
bool read_name(const ImportPatchMemory& memory, std::uint32_t address, std::string& out) {
    out.clear();
    for (std::uint32_t i = 0; i < 40; ++i) {
        const std::uint8_t* c = memory.read(address + i, 1);
        if (c == nullptr) return false;
        if (*c == 0) return true;
        out.push_back(static_cast<char>(*c));
    }
    return false;  // too long to be one of ours
}

std::mutex g_targets_mutex;
std::unordered_map<std::string, std::uint32_t> g_targets;

}  // namespace

ImportPatchStats patch_library_imports(std::uint32_t base, const ImportPatchMemory& memory,
                                       const std::unordered_map<std::string, std::uint32_t>& targets) {
    ImportPatchStats stats;
    const std::uint8_t* eh = memory.read(base, 52);
    if (eh == nullptr || std::memcmp(eh, "\x7f" "ELF", 4) != 0 || eh[4] != 1 || eh[5] != 1) return stats;  // 32-bit little endian
    if (rd16(eh + 18) != 40) return stats;                                                                // ARM
    const std::uint32_t phoff = rd32(eh + 28);
    const std::uint32_t phentsize = rd16(eh + 42);
    const std::uint32_t phnum = rd16(eh + 44);
    if (phentsize < 32 || phnum == 0 || phnum > 64) return stats;

    std::uint32_t min_vaddr = 0xFFFFFFFFu, dynamic_vaddr = 0, dynamic_size = 0;
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::uint8_t* ph = memory.read(base + phoff + i * phentsize, 32);
        if (ph == nullptr) return stats;
        const std::uint32_t type = rd32(ph), vaddr = rd32(ph + 8), memsz = rd32(ph + 20);
        if (type == kPtLoad && vaddr < min_vaddr) min_vaddr = vaddr;
        if (type == kPtDynamic) {
            dynamic_vaddr = vaddr;
            dynamic_size = memsz;
        }
    }
    if (min_vaddr == 0xFFFFFFFFu || dynamic_size == 0) return stats;
    const std::uint32_t bias = base - min_vaddr;

    std::uint32_t strtab = 0, symtab = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    for (std::uint32_t offset = 0; offset + 8 <= dynamic_size && offset < 0x4000; offset += 8) {
        const std::uint8_t* d = memory.read(bias + dynamic_vaddr + offset, 8);
        if (d == nullptr) return stats;
        const std::uint32_t tag = rd32(d), value = rd32(d + 4);
        if (tag == kDtNull) break;
        // address entries hold a link-time offset; if a linker already added the load address keep it
        const std::uint32_t absolute = value >= base ? value : bias + value;
        switch (tag) {
            case kDtStrTab: strtab = absolute; break;
            case kDtSymTab: symtab = absolute; break;
            case kDtRel: rel = absolute; break;
            case kDtRelSz: relsz = value; break;
            case kDtJmpRel: jmprel = absolute; break;
            case kDtPltRelSz: pltrelsz = value; break;
            default: break;
        }
    }
    if (strtab == 0 || symtab == 0) return stats;

    struct Table {
        std::uint32_t address, size;
    };
    const Table tables[2] = {{rel, relsz}, {jmprel, pltrelsz}};
    std::string name;
    for (const Table& table : tables) {
        if (table.address == 0 || table.size == 0) continue;
        const std::uint32_t count = std::min<std::uint32_t>(table.size / 8, 200000);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* entry = memory.read(table.address + 8 * i, 8);
            if (entry == nullptr) break;
            const std::uint32_t offset = rd32(entry), info = rd32(entry + 4);
            const std::uint32_t type = info & 0xFF, symbol = info >> 8;
            if ((type != kRArmGlobDat && type != kRArmJumpSlot) || symbol == 0) continue;
            const std::uint8_t* sym = memory.read(symtab + 16 * symbol, 16);
            if (sym == nullptr) continue;
            if (rd16(sym + 14) != 0) continue;  // defined in this library: not an import
            if (!read_name(memory, strtab + rd32(sym), name)) continue;
            const auto target = targets.find(name);
            if (target == targets.end()) continue;
            ++stats.imports_seen;
            const std::uint32_t slot = bias + offset;
            const std::uint8_t* current = memory.read(slot, 4);
            if (current == nullptr) {
                ++stats.failed;
                continue;
            }
            stats.slots.emplace_back(slot, target->second);
            if (rd32(current) == target->second) {
                ++stats.already;
            } else if (memory.write32(slot, target->second)) {
                ++stats.patched;
            } else {
                ++stats.failed;
            }
        }
    }
    return stats;
}

unsigned recheck_import_slots(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& slots, const ImportPatchMemory& memory) {
    unsigned rewritten = 0;
    for (const auto& [slot, target] : slots) {
        const std::uint8_t* current = memory.read(slot, 4);
        if (current == nullptr || rd32(current) == target) continue;
        if (memory.write32(slot, target)) ++rewritten;
    }
    return rewritten;
}

void register_patch_targets(const std::unordered_map<std::string, std::uint32_t>& targets) {
    std::lock_guard<std::mutex> lock(g_targets_mutex);
    for (const auto& [name, address] : targets) g_targets[name] = address;
}

std::unordered_map<std::string, std::uint32_t> patch_targets() {
    std::lock_guard<std::mutex> lock(g_targets_mutex);
    return g_targets;
}

}  // namespace zb
