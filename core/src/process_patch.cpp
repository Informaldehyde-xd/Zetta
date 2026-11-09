#include <sys/mman.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "zb/guest_memory.h"
#include "zb/import_patch.h"
#include "zb/log.h"
#include "zb/options.h"
#include "zb/process.h"
#include "zb/runtime_report.h"

namespace zb {

// The guest's libzbcompat offers faster versions of some libc and zlib functions (native memcpy, memset and
// zlib). Asking the linker to prefer them does not work on every phone, so the host walks every game
// library that is mapped and writes those functions into the libraries' own import tables. A library is
// walked in full on its first three passes (its relocations may still be in progress), after that its
// slots are only re-checked. Switch: patch_imports=0 in zb-options.txt.
void Process::patch_imports() {
    // Off by default: the first phone test of this patching came with a silent game and a lower frame rate, so
    // it is opt-in until it is shown to be safe. patch_imports=1 turns it on; patch_mem=0 / patch_zlib=0 then
    // leave out the memcpy-family or the zlib functions, so a fault can be narrowed down.
    static const bool enabled = option_enabled("patch_imports", false);
    if (!enabled) return;
    static const bool with_mem = option_enabled("patch_mem", true);
    static const bool with_zlib = option_enabled("patch_zlib", true);
    std::unordered_map<std::string, std::uint32_t> targets = patch_targets();
    for (auto it = targets.begin(); it != targets.end();) {
        const bool is_mem = it->first.compare(0, 3, "mem") == 0 || it->first.compare(0, 11, "__aeabi_mem") == 0;
        if ((is_mem && !with_mem) || (!is_mem && !with_zlib)) it = targets.erase(it);
        else ++it;
    }
    if (targets.empty()) return;

    struct LibraryState {
        std::string path;
        std::uint32_t length = 0;
        unsigned passes = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> slots;
    };
    static std::unordered_map<std::uint32_t, LibraryState> libraries;  // by load address
    static unsigned long long total_slots = 0, total_libraries = 0, total_rewritten = 0, opened_pages = 0;
    bool changed = false;

    std::lock_guard<std::mutex> mm(mm_mutex_);
    ImportPatchMemory memory;
    memory.read = [this](std::uint32_t addr, std::uint32_t len) -> const std::uint8_t* {
        return mem_.host_ptr(addr, len, kPageRead);
    };
    memory.write32 = [this](std::uint32_t addr, std::uint32_t value) -> bool {
        auto store = [&]() {
            std::uint8_t* where = mem_.host_ptr(addr, 4, kPageWrite);
            if (where == nullptr) return false;
            __atomic_store_n(reinterpret_cast<std::uint32_t*>(where), value, __ATOMIC_SEQ_CST);
            return true;
        };
        if (!mem_.accessible(addr, 4, kPageRead)) return false;
        if (mem_.accessible(addr, 4, kPageWrite)) return store();
        // a read-only page (a fully protected GOT): open it for this one word, then close it again
        const std::uint32_t page = addr & ~0xFFFu;
        const int restore = PROT_READ | (mem_.accessible(addr, 1, kPageExec) ? PROT_EXEC : 0);
        if (!mem_.protect(page, 4096, PROT_READ | PROT_WRITE)) return false;
        const bool ok = store();
        mem_.protect(page, 4096, restore);
        ++opened_pages;
        return ok;
    };

    for (const FileMapping& m : file_mappings_) {
        if (m.offset != 0 || m.length < 4096) continue;
        // not the system libraries, our own helper libraries, or the guest service program
        if (m.path.find("/sysroot/") != std::string::npos || m.path.find("zbhost") != std::string::npos ||
            m.path.find("libzbcompat") != std::string::npos || m.path.find("libzbjni") != std::string::npos)
            continue;
        LibraryState& state = libraries[m.start];
        if (state.path != m.path || state.length != m.length) state = LibraryState{m.path, m.length, 0, {}};
        if (state.passes >= 3) {
            const unsigned rewritten = recheck_import_slots(state.slots, memory);
            if (rewritten != 0) {
                total_rewritten += rewritten;
                changed = true;
            }
            continue;
        }
        ++state.passes;
        const ImportPatchStats stats = patch_library_imports(m.start, memory, targets);
        if (state.passes == 1 && !stats.slots.empty()) ++total_libraries;
        if (stats.patched != 0) {
            total_slots += stats.patched;
            changed = true;
        }
        state.slots = stats.slots;
    }
    if (changed) {
        char text[160];
        std::snprintf(text, sizeof text, "%llu slots patched in %llu libraries (%llu re-written, %llu read-only pages opened)", total_slots,
                      total_libraries, total_rewritten, opened_pages);
        runtime_report().note_jni_detail("import-patch", text, true);
    }
}

}  // namespace zb
