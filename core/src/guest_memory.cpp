#include "zb/guest_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr std::uint64_t kGuardSize = 64 * 1024;
constexpr std::uint32_t kLowestAllocPage = 0x10000 >> 12;

bool range_valid(std::uint32_t addr, std::uint64_t len) {
    return len != 0 && (addr & kPageMask) == 0 && static_cast<std::uint64_t>(addr) + len <= kGuestSpaceSize;
}

std::uint8_t flags_from_prot(int prot) {
    std::uint8_t f = kPageMapped;
    if (prot & PROT_READ) f |= kPageRead;
    if (prot & PROT_WRITE) f |= kPageWrite;
    if (prot & PROT_EXEC) f |= kPageExec;
    return f;
}

std::uint32_t query_host_page() {
#ifdef ZB_TEST_HOST_PAGE
    return ZB_TEST_HOST_PAGE;  // tests pretend to run on a phone with bigger pages
#else
    const long value = sysconf(_SC_PAGESIZE);
    return value >= static_cast<long>(kPageSize) ? static_cast<std::uint32_t>(value) : kPageSize;
#endif
}

}  // namespace

int host_prot(int guest_prot) {
    int p = PROT_NONE;
    if (guest_prot & (PROT_READ | PROT_EXEC)) p |= PROT_READ;
    if (guest_prot & PROT_WRITE) p |= PROT_READ | PROT_WRITE;
    return p;
}

GuestMemory::GuestMemory() : pages_(static_cast<std::size_t>(kGuestSpaceSize >> 12), 0) {
    host_page_ = query_host_page();
    void* p = mmap(nullptr, kGuestSpaceSize + kGuardSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        log("cannot reserve guest address space: %s", std::strerror(errno));
        // Visible in the run report, so a phone that limits address space is told apart from a game bug.
        runtime_report().note_jni_detail("guest-memory", std::string("cannot reserve 4 GiB of address space: ") + std::strerror(errno), true);
        return;
    }
    if (coarse()) {
        runtime_report().note_jni_detail("host-page-size", std::to_string(host_page_) + " bytes (guest pages share host pages)", true);
    }
    base_ = static_cast<std::uint8_t*>(p);
}

GuestMemory::~GuestMemory() {
    if (base_) munmap(base_, kGuestSpaceSize + kGuardSize);
}

void GuestMemory::set_flags(std::uint32_t addr, std::uint64_t len, std::uint8_t flags) {
    std::memset(pages_.data() + (addr >> 12), flags, static_cast<std::size_t>(page_round_up(len) >> 12));
}

bool GuestMemory::map_anon(std::uint32_t addr, std::uint64_t len, int prot) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len)) return false;
    if (coarse()) return map_anon_coarse(addr, len, prot);
    void* p = mmap(base_ + addr, len, host_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return false;
    set_flags(addr, len, flags_from_prot(prot));
    return true;
}

bool GuestMemory::map_file(std::uint32_t addr, std::uint64_t len, int prot, int share_flags, int fd, std::uint64_t offset) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len)) return false;
    if (coarse()) return map_file_coarse(addr, len, prot, share_flags, fd, offset);
    int flags = (share_flags & (MAP_SHARED | MAP_PRIVATE)) | MAP_FIXED;
    void* p = mmap(base_ + addr, len, host_prot(prot), flags, fd, static_cast<off_t>(offset));
    if (p == MAP_FAILED) return false;
    set_flags(addr, len, flags_from_prot(prot));
    return true;
}

bool GuestMemory::protect(std::uint32_t addr, std::uint64_t len, int prot) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len) || !accessible(addr, len, 0)) return false;
    if (coarse()) return protect_coarse(addr, len, prot);
    if (mprotect(base_ + addr, len, host_prot(prot)) != 0) return false;
    set_flags(addr, len, flags_from_prot(prot));
    return true;
}

bool GuestMemory::unmap(std::uint32_t addr, std::uint64_t len) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len)) return false;
    if (coarse()) return unmap_coarse(addr, len);
    void* p = mmap(base_ + addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    set_flags(addr, len, 0);
    return true;
}

std::uint32_t GuestMemory::find_free(std::uint64_t len, std::uint32_t limit) const {
    const std::uint64_t pages = page_round_up(len) >> 12;
    const std::uint64_t top = limit >> 12;
    if (pages == 0 || top < kLowestAllocPage || top - kLowestAllocPage < pages) return 0;
    std::uint64_t run = 0;
    for (std::uint64_t i = top; i-- > kLowestAllocPage;) {
        if (pages_[i] == 0) {
            if (++run == pages) return static_cast<std::uint32_t>(i << 12);
        } else {
            run = 0;
        }
    }
    return 0;
}

bool GuestMemory::range_free(std::uint32_t addr, std::uint64_t len) const {
    len = page_round_up(len);
    if (!range_valid(addr, len)) return false;
    for (std::uint64_t i = addr >> 12, end = (addr + len) >> 12; i < end; ++i) {
        if (pages_[i] != 0) return false;
    }
    return true;
}

bool GuestMemory::accessible(std::uint32_t addr, std::uint64_t len, std::uint8_t need) const {
    if (len == 0) return true;
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + len;
    if (end > kGuestSpaceSize) return false;
    const std::uint8_t mask = need | kPageMapped;
    for (std::uint64_t i = addr >> 12, last = (end - 1) >> 12; i <= last; ++i) {
        if ((pages_[i] & mask) != mask) return false;
    }
    return true;
}

std::uint8_t* GuestMemory::host_ptr(std::uint32_t addr, std::uint64_t len, std::uint8_t need) const {
    return accessible(addr, len, need) ? base_ + addr : nullptr;
}


// ---- phones with host pages bigger than the guest's 4 KiB pages --------------------------------

bool GuestMemory::group_empty(std::uint64_t host_addr) const {
    for (std::uint64_t i = host_addr >> 12, end = (host_addr + host_page_) >> 12; i < end; ++i) {
        if (pages_[i] != 0) return false;
    }
    return true;
}

// Makes every host page that overlaps the guest range readable and writable memory, keeping the
// contents of guest pages that share a host page with the range.
bool GuestMemory::ensure_host_pages(std::uint32_t addr, std::uint64_t len) {
    const std::uint64_t end = host_ceil(static_cast<std::uint64_t>(addr) + len);
    for (std::uint64_t hp = host_floor(addr); hp < end;) {
        if (group_empty(hp)) {
            // Fresh host pages: one mapping for the whole run of them.
            std::uint64_t run_end = hp + host_page_;
            while (run_end < end && group_empty(run_end)) run_end += host_page_;
            void* p = mmap(base_ + hp, run_end - hp, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (p == MAP_FAILED) return false;
            hp = run_end;
        } else {
            if (mprotect(base_ + hp, host_page_, PROT_READ | PROT_WRITE) != 0) return false;
            hp += host_page_;
        }
    }
    return true;
}

// Host protection of a host page = the most permissive of its guest pages (the guest's own
// protection is still enforced from the page flags by the syscall layer).
bool GuestMemory::refresh_host_prot(std::uint32_t addr, std::uint64_t len) {
    const std::uint64_t end = host_ceil(static_cast<std::uint64_t>(addr) + len);
    std::uint64_t run_start = host_floor(addr);
    int run_prot = -1;
    for (std::uint64_t hp = run_start; hp <= end; hp += host_page_) {
        int prot = -1;  // -1 marks the end of the range
        if (hp < end) {
            prot = PROT_NONE;
            for (std::uint64_t i = hp >> 12, last = (hp + host_page_) >> 12; i < last; ++i) {
                const std::uint8_t f = pages_[i];
                if (!(f & kPageMapped)) continue;
                if (f & (kPageRead | kPageExec)) prot |= PROT_READ;
                if (f & kPageWrite) prot |= PROT_READ | PROT_WRITE;
            }
        }
        if (prot != run_prot) {
            if (run_prot >= 0 && mprotect(base_ + run_start, hp - run_start, run_prot) != 0) return false;
            run_start = hp;
            run_prot = prot;
        }
    }
    return true;
}

bool GuestMemory::map_anon_coarse(std::uint32_t addr, std::uint64_t len, int prot) {
    const bool replaces = !range_free(addr, len);  // a replaced mapping must come back zero-filled
    if (!ensure_host_pages(addr, len)) return false;
    if (replaces) std::memset(base_ + addr, 0, static_cast<std::size_t>(len));
    set_flags(addr, len, flags_from_prot(prot));
    return refresh_host_prot(addr, len);
}

bool GuestMemory::map_file_coarse(std::uint32_t addr, std::uint64_t len, int prot, int share_flags, int fd, std::uint64_t offset) {
    const bool aligned = (addr & (host_page_ - 1)) == 0 && (offset & (host_page_ - 1)) == 0 && (len & (host_page_ - 1)) == 0;
    if (aligned && (share_flags & MAP_SHARED)) {
        // A whole-host-page shared mapping can stay a real mapping (writes must reach the file).
        void* p = mmap(base_ + addr, len, host_prot(prot), MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(offset));
        if (p == MAP_FAILED) return false;
        set_flags(addr, len, flags_from_prot(prot));
        return true;
    }
    // Anything else is read into private memory: file offsets that are not host-page aligned cannot
    // be mapped. Private mappings never write back, so a copy behaves the same.
    const bool replaces = !range_free(addr, len);
    if (!ensure_host_pages(addr, len)) return false;
    if (replaces) std::memset(base_ + addr, 0, static_cast<std::size_t>(len));
    std::uint64_t done = 0;
    while (done < len) {
        const ssize_t got = pread(fd, base_ + addr + done, static_cast<std::size_t>(len - done), static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) break;  // past the end of the file: the rest stays zero
        done += static_cast<std::uint64_t>(got);
    }
    set_flags(addr, len, flags_from_prot(prot));
    return refresh_host_prot(addr, len);
}

bool GuestMemory::protect_coarse(std::uint32_t addr, std::uint64_t len, int prot) {
    set_flags(addr, len, flags_from_prot(prot));
    return refresh_host_prot(addr, len);
}

bool GuestMemory::unmap_coarse(std::uint32_t addr, std::uint64_t len) {
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + len;
    set_flags(addr, len, 0);
    for (std::uint64_t hp = host_floor(addr); hp < host_ceil(end); hp += host_page_) {
        if (group_empty(hp)) {
            // Nothing of the guest lives here any more: give the memory back.
            void* p = mmap(base_ + hp, host_page_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
            if (p == MAP_FAILED) return false;
        } else {
            // Neighbouring guest pages stay: zero only the freed part so a later mapping starts clean.
            if (mprotect(base_ + hp, host_page_, PROT_READ | PROT_WRITE) != 0) return false;
            const std::uint64_t from = hp > addr ? hp : addr;
            const std::uint64_t to = hp + host_page_ < end ? hp + host_page_ : end;
            std::memset(base_ + from, 0, static_cast<std::size_t>(to - from));
            if (!refresh_host_prot(static_cast<std::uint32_t>(hp), host_page_)) return false;
        }
    }
    return true;
}

}  // namespace zb
