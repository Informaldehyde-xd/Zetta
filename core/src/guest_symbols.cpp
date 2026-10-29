#include "guest_symbols.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace zb {

namespace {

struct Symbol {
    std::uint32_t value;
    std::uint32_t size;
    std::string name;
};

struct Segment {
    std::uint32_t offset, vaddr, filesz;
};

struct Table {
    std::vector<Symbol> symbols;  // sorted by value
    std::vector<Segment> segments;
};

std::uint32_t u32(const std::vector<std::uint8_t>& d, std::size_t o) {
    std::uint32_t v;
    std::memcpy(&v, d.data() + o, sizeof v);
    return v;
}

std::uint16_t u16(const std::vector<std::uint8_t>& d, std::size_t o) {
    std::uint16_t v;
    std::memcpy(&v, d.data() + o, sizeof v);
    return v;
}

bool read_at(std::FILE* f, std::uint64_t offset, std::size_t size, std::vector<std::uint8_t>& out) {
    out.assign(size, 0);
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return false;
    return std::fread(out.data(), 1, size, f) == size;
}

std::shared_ptr<Table> load(const std::string& path) {
    auto table = std::make_shared<Table>();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return table;
    std::vector<std::uint8_t> head;
    if (read_at(f, 0, 52, head) && std::memcmp(head.data(), "\x7f" "ELF", 4) == 0 && head[4] == 1 && head[5] == 1) {
        const std::uint32_t phoff = u32(head, 0x1C), shoff = u32(head, 0x20);
        const std::uint16_t phentsize = u16(head, 0x2A), phnum = u16(head, 0x2C), shentsize = u16(head, 0x2E), shnum = u16(head, 0x30);
        std::vector<std::uint8_t> ph;
        if (phentsize >= 32 && phnum > 0 && phnum < 64 && read_at(f, phoff, static_cast<std::size_t>(phentsize) * phnum, ph)) {
            for (std::uint16_t i = 0; i < phnum; ++i) {
                const std::size_t o = static_cast<std::size_t>(i) * phentsize;
                if (u32(ph, o) == 1) table->segments.push_back({u32(ph, o + 4), u32(ph, o + 8), u32(ph, o + 16)});  // PT_LOAD
            }
        }
        std::vector<std::uint8_t> sh;
        if (shentsize >= 40 && shnum > 0 && shnum < 200 && read_at(f, shoff, static_cast<std::size_t>(shentsize) * shnum, sh)) {
            for (std::uint16_t i = 0; i < shnum; ++i) {
                const std::size_t o = static_cast<std::size_t>(i) * shentsize;
                const std::uint32_t type = u32(sh, o + 4);
                if (type != 2 && type != 11) continue;  // SHT_SYMTAB, SHT_DYNSYM
                const std::uint32_t off = u32(sh, o + 16), size = u32(sh, o + 20), link = u32(sh, o + 24);
                if (size == 0 || size > (8u << 20) || link >= shnum) continue;
                const std::size_t lo = static_cast<std::size_t>(link) * shentsize;
                const std::uint32_t str_off = u32(sh, lo + 16), str_size = u32(sh, lo + 20);
                if (str_size == 0 || str_size > (8u << 20)) continue;
                std::vector<std::uint8_t> syms, strs;
                if (!read_at(f, off, size, syms) || !read_at(f, str_off, str_size, strs)) continue;
                for (std::size_t s = 0; s + 16 <= syms.size(); s += 16) {
                    const std::uint32_t name = u32(syms, s), value = u32(syms, s + 4), sz = u32(syms, s + 8);
                    const std::uint8_t info = syms[s + 12];
                    const std::uint16_t shndx = u16(syms, s + 14);
                    const std::uint8_t kind = info & 0xF;
                    if (shndx == 0 || value == 0 || (kind != 0 && kind != 2) || name >= strs.size()) continue;
                    const char* text = reinterpret_cast<const char*>(strs.data()) + name;
                    if (*text == '\0' || *text == '$') continue;  // ARM mapping symbols
                    table->symbols.push_back({value & ~1u, sz, std::string(text, strnlen(text, strs.size() - name))});
                }
            }
            std::sort(table->symbols.begin(), table->symbols.end(),
                      [](const Symbol& a, const Symbol& b) { return a.value < b.value; });
        }
    }
    std::fclose(f);
    return table;
}

}  // namespace

std::string guest_symbol_for(const std::string& path, std::uint64_t position, bool is_vaddr) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<Table>> cache;
    std::shared_ptr<Table> table;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(path);
        if (it == cache.end()) it = cache.emplace(path, load(path)).first;
        table = it->second;
    }
    std::uint64_t vaddr = position;
    if (!is_vaddr) {
        bool found = false;
        for (const Segment& s : table->segments) {
            if (position >= s.offset && position < static_cast<std::uint64_t>(s.offset) + s.filesz) {
                vaddr = position - s.offset + s.vaddr;
                found = true;
                break;
            }
        }
        if (!found && !table->segments.empty()) return "";
    }
    // The last symbol that starts at or before the address.
    auto it = std::upper_bound(table->symbols.begin(), table->symbols.end(), vaddr,
                               [](std::uint64_t value, const Symbol& s) { return value < s.value; });
    if (it == table->symbols.begin()) return "";
    --it;
    const std::uint64_t distance = vaddr - it->value;
    if (it->size > 0 ? distance >= it->size : distance > 0x400) return "";
    char text[40];
    std::snprintf(text, sizeof text, "+0x%llx", static_cast<unsigned long long>(distance));
    return it->name + text;
}

}  // namespace zb
