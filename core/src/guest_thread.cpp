#include "zb/external_dir.h"
#include "zb/guest_thread.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <bit>
#include <climits>
#include <cstring>
#include <mutex>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/halt_reason.h>

#include "zb/cp15.h"
#include <cstdio>

#include "zb/log.h"
#include "zb/options.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr Dynarmic::HaltReason kStopHalt = Dynarmic::HaltReason::UserDefined1;
constexpr Dynarmic::HaltReason kInterruptHalt = Dynarmic::HaltReason::UserDefined2;
// With check_halt_on_memory_access, code emitted after each memory access tests this bit and
// returns with PC set to the accessing instruction, so faults are precise.
constexpr Dynarmic::HaltReason kMemoryAbortHalt = Dynarmic::HaltReason::MemoryAbort;

// Speed switches, set from a text file so that each one can be tried without a rebuild:
// <external files dir>/zb-tuning.txt, one "name=1" or "name=0" per line. A name that is not in the
// file has its default, given here.
//   little_endian  (default ON)   the guest never uses SETEND, so every load and store can skip the
//                                 byte-order check
//   fast_fp        (default ON)   fp_standard + fp_estimate + fp_fma below. A Unity 5 game ran at 15 fps
//                                 with little_endian and fast_fp and at 6-9 fps without them.
//                                 Results can differ in the last bits, which games rarely notice.
//   fp_standard    (default off)  NEON arithmetic runs with the host's floating-point state instead of
//                                 switching to the ARM "standard FPSCR" for every operation
//   fp_estimate    (default off)  reciprocal and square-root estimates may be more exact than ARM's
//   fp_fma         (default off)  fused multiply-add is split into a multiply and an add
//   fp_nan         (default off)  results may not have the exact NaN ARM would produce (broke loading)
//   fast_exclusive (default off)  ldrex/strex (atomics, locks) use the direct memory pointer
// To switch a default off for a game that misbehaves: "fast_fp=0" and "little_endian=0".
// The file is read once; a change needs a new start of the game.
// zb-tuning.txt lives in the launcher's external files folder (zb::external_file_path)

bool tuning(const char* name) {
    static const std::string text = [] {
        std::string content;
        if (std::FILE* file = std::fopen(external_file_path("zb-tuning.txt").c_str(), "re")) {
            char buffer[512];
            std::size_t got;
            while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) content.append(buffer, got);
            std::fclose(file);
        }
        return content;
    }();
    auto has_line = [&](const std::string& wanted) {
        for (std::size_t at = text.find(wanted); at != std::string::npos; at = text.find(wanted, at + 1)) {
            const bool line_start = at == 0 || text[at - 1] == '\n';
            const std::size_t end = at + wanted.size();
            const bool line_end = end == text.size() || text[end] == '\n' || text[end] == '\r' || text[end] == ' ';
            if (line_start && line_end) return true;
        }
        return false;
    };
    bool value;
    if (has_line(std::string(name) + "=0")) value = false;
    else if (has_line(std::string(name) + "=1")) value = true;
    else value = std::strcmp(name, "little_endian") == 0 || std::strcmp(name, "fast_fp") == 0;
    // show what is active in the runtime report (one entry per switch, in the order they are first asked for)
    static std::mutex note_mutex;
    static std::string active;
    {
        std::lock_guard<std::mutex> lock(note_mutex);
        if (active.find(std::string(name) + "=") == std::string::npos) {
            active += std::string(active.empty() ? "" : " ") + name + "=" + (value ? "1" : "0");
            runtime_report().note_jni_detail("tuning", active, true);
        }
    }
    return value;
}

constexpr std::uint32_t kCpsrThumb = 0x20;
constexpr std::uint32_t kCpsrEndian = 0x200;
constexpr std::uint32_t kCpsrItMask = 0x0600FC00;

template <typename T>
T load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T>
void store(std::uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof v);
}

}  // namespace

namespace {
constexpr std::size_t kAtomicSlots = 512;
std::atomic<std::uint32_t> g_atomic_address[kAtomicSlots];
std::atomic<std::uint32_t> g_atomic_count[kAtomicSlots];
std::atomic<std::int32_t> g_atomic_tid[kAtomicSlots];
std::atomic<std::uint32_t> g_atomic_value[kAtomicSlots];
std::atomic<std::uint32_t> g_atomic_ms[kAtomicSlots];
std::atomic<std::int32_t> g_atomic_acquire_tid[kAtomicSlots];
std::atomic<std::uint32_t> g_atomic_acquire_ms[kAtomicSlots];
thread_local std::uint32_t t_atomic_tick = 0;
const std::chrono::steady_clock::time_point g_uptime_start = std::chrono::steady_clock::now();
}  // namespace

std::uint32_t process_uptime_ms() {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_uptime_start).count());
}

// A word already in the table is followed on every store; a new word is only picked up on every 8th.
void note_atomic_store(std::uint32_t vaddr, std::int32_t tid, std::uint32_t value, std::uint32_t old_value) {
    const bool sampled = (++t_atomic_tick & 7u) == 0;
    std::size_t slot = (vaddr >> 2) * 2654435761u % kAtomicSlots;
    for (int probe = 0; probe < 8; ++probe, slot = (slot + 1) % kAtomicSlots) {
        std::uint32_t seen = g_atomic_address[slot].load(std::memory_order_relaxed);
        if (seen == 0) {
            if (!sampled) return;
            std::uint32_t empty = 0;
            if (g_atomic_address[slot].compare_exchange_strong(empty, vaddr)) seen = vaddr;
            else seen = empty;
        }
        if (seen == vaddr) {
            g_atomic_count[slot].fetch_add(1, std::memory_order_relaxed);
            g_atomic_tid[slot].store(tid, std::memory_order_relaxed);
            g_atomic_value[slot].store(value, std::memory_order_relaxed);
            const std::uint32_t now_ms = process_uptime_ms();
            g_atomic_ms[slot].store(now_ms, std::memory_order_relaxed);
            if (old_value == 0 && value != 0) {
                g_atomic_acquire_tid[slot].store(tid, std::memory_order_relaxed);
                g_atomic_acquire_ms[slot].store(now_ms, std::memory_order_relaxed);
            }
            return;
        }
    }
}

std::vector<AtomicWordInfo> hot_atomic_words(std::size_t most) {
    std::vector<AtomicWordInfo> all;
    for (std::size_t i = 0; i < kAtomicSlots; ++i) {
        const std::uint32_t address = g_atomic_address[i].load(std::memory_order_relaxed);
        if (address == 0) continue;
        all.push_back({address, g_atomic_count[i].load(std::memory_order_relaxed),
                       g_atomic_tid[i].load(std::memory_order_relaxed), g_atomic_value[i].load(std::memory_order_relaxed),
                       g_atomic_ms[i].load(std::memory_order_relaxed), g_atomic_acquire_tid[i].load(std::memory_order_relaxed),
                       g_atomic_acquire_ms[i].load(std::memory_order_relaxed)});
    }
    std::sort(all.begin(), all.end(), [](const AtomicWordInfo& x, const AtomicWordInfo& y) { return x.count > y.count; });
    if (all.size() > most) all.resize(most);
    return all;
}

namespace {
constexpr std::size_t kCodeLineWords = (1u << 26) / 64;  // one bit per 64-byte line of the 4 GB guest space (8 MB)
std::atomic<std::uint64_t> g_code_lines[kCodeLineWords];
std::atomic<std::uint64_t> g_fetched{0}, g_invalidations{0}, g_skipped{0};
}  // namespace

void note_code_page(std::uint32_t vaddr) {
    const std::uint32_t line = vaddr >> 6;
    // always a read-modify-write: a plain "already set" check could miss the order of a concurrent invalidation
    g_code_lines[line >> 6].fetch_or(1ull << (line & 63), std::memory_order_seq_cst);
}

bool code_in_range(std::uint32_t addr, std::uint32_t len) {
    if (len == 0) return false;
    const std::uint64_t end = std::min<std::uint64_t>(static_cast<std::uint64_t>(addr) + len, 1ull << 32);  // exclusive
    const std::uint32_t first = addr >> 6;
    const std::uint32_t last = static_cast<std::uint32_t>((end - 1) >> 6);
    for (std::uint32_t word = first >> 6; word <= (last >> 6); ++word) {
        std::uint64_t mask = ~0ull;
        if (word == (first >> 6)) mask &= ~0ull << (first & 63);
        if (word == (last >> 6)) mask &= ~0ull >> (63 - (last & 63));
        if ((g_code_lines[word].load(std::memory_order_seq_cst) & mask) != 0) return true;
    }
    return false;
}

TranslationCounters translation_counters() {
    return {g_fetched.load(std::memory_order_relaxed), g_invalidations.load(std::memory_order_relaxed),
            g_skipped.load(std::memory_order_relaxed)};
}

void note_invalidation(bool skipped) {
    g_invalidations.fetch_add(1, std::memory_order_relaxed);
    if (skipped) g_skipped.fetch_add(1, std::memory_order_relaxed);
}

std::size_t default_code_cache_size() {
    // 120 MB unless code_cache_mb=N is set in zb-options.txt. Above about 128 MB the translator's
    // branches may not reach, so values from 128 to 512 are for testing only: if a game does not
    // start with one, lower it.
    static const std::size_t size = [] {
        int mb = option_number("code_cache_mb", 120);
        if (mb < 8) mb = 8;
        if (mb > 512) mb = 512;
        return static_cast<std::size_t>(mb) * 1024 * 1024;
    }();
    return size;
}

GuestThread::GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id,
                         bool precise_faults, std::size_t code_cache_size)
    : mem_(mem), processor_id_(processor_id) {
    cp15_ = std::make_shared<Cp15>(&tpidruro_, &tpidrurw_);

    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = this;
    cfg.global_monitor = monitor;
    cfg.processor_id = processor_id;
    cfg.arch_version = Dynarmic::A32::ArchVersion::v8;
    cfg.fastmem_pointer = static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(mem.base()));
    cfg.coprocessors[15] = cp15_;
    cfg.define_unpredictable_behaviour = true;
    cfg.enable_cycle_counting = false;
    cfg.check_halt_on_memory_access = precise_faults;
    cfg.code_cache_size = code_cache_size;
    if (tuning("little_endian")) cfg.always_little_endian = true;
    {
        const bool fast_fp = tuning("fast_fp");
        const bool standard = fast_fp || tuning("fp_standard");
        const bool estimate = fast_fp || tuning("fp_estimate");
        const bool fma = fast_fp || tuning("fp_fma");
        const bool nan = tuning("fp_nan");
        if (standard || estimate || fma || nan) cfg.unsafe_optimizations = true;
        if (standard) cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
        if (estimate) cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
        if (fma) cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        if (nan) cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
    }
    if (tuning("fast_exclusive")) cfg.fastmem_exclusive_access = true;
    // The assembly dispatcher (third_party/patches/dynarmic-0003) is on by default; this turns it off.
    if (tuning("no_fast_dispatch")) cfg.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
    static std::once_flag logged;
    std::call_once(logged, [] {
        log("tuning: little_endian=%d fast_fp=%d fp_standard=%d fp_estimate=%d fp_fma=%d fp_nan=%d fast_exclusive=%d",
            tuning("little_endian") ? 1 : 0, tuning("fast_fp") ? 1 : 0, tuning("fp_standard") ? 1 : 0,
            tuning("fp_estimate") ? 1 : 0, tuning("fp_fma") ? 1 : 0, tuning("fp_nan") ? 1 : 0,
            tuning("fast_exclusive") ? 1 : 0);
    });
    jit_ = std::make_unique<Dynarmic::A32::Jit>(cfg);
}

GuestThread::~GuestThread() = default;

std::array<std::uint32_t, 16>& GuestThread::regs() {
    return jit_->Regs();
}

std::array<std::uint32_t, 64>& GuestThread::ext_regs() {
    return jit_->ExtRegs();
}

std::uint32_t GuestThread::cpsr() const {
    return jit_->Cpsr();
}

std::uint32_t GuestThread::fpscr() const {
    return jit_->Fpscr();
}

void GuestThread::set_fpscr(std::uint32_t value) {
    jit_->SetFpscr(value);
}

void GuestThread::set_cpsr(std::uint32_t value) {
    jit_->SetCpsr(value);
}

// The translator throws for an instruction its backend cannot emit, and that throw crosses every
// entry into translated code: the run loop, a Java-to-guest native call, a nested guest call. It
// is caught here, at the one place they all share, and turned into the same stop a guest fault
// produces, so the caller unwinds normally and the report names the program counter instead of
// the process dying with "terminating".
Stop GuestThread::translator_gave_up(const char* reason) {
    const std::uint32_t pc = regs()[15];
    char detail[128];
    std::snprintf(detail, sizeof detail, "pc 0x%08x: %s", pc, reason != nullptr ? reason : "unknown");
    log("the translator gave up at %s", detail);
    runtime_report().note_jni_detail("translator-gave-up", detail, true);
    Stop stop;
    stop.kind = StopKind::Exception;
    stop.exception = Dynarmic::A32::Exception::UndefinedInstruction;
    stop.pc = pc;
    return stop;
}

Stop GuestThread::run() {
    pending_ = Stop{};
    for (;;) {
        Dynarmic::HaltReason reason{};
        try {
            reason = jit_->Run();
        } catch (const std::exception& failure) {
            return translator_gave_up(failure.what());
        } catch (...) {
            return translator_gave_up(nullptr);
        }
        jit_->ClearHalt(kStopHalt | kMemoryAbortHalt);
        if (pending_.kind == StopKind::None && Dynarmic::Has(reason, kInterruptHalt)) {
            jit_->ClearHalt(kInterruptHalt);
            pending_.kind = StopKind::Interrupted;
            break;
        }
        // Another thread invalidated translated code while we were running; the JIT applies
        // the invalidation at the start of the next Run().
        if (pending_.kind == StopKind::None && Dynarmic::Has(reason, Dynarmic::HaltReason::CacheInvalidation)) continue;
        break;
    }
    Stop s = pending_;
    if (s.kind != StopKind::Exception) s.pc = jit_->Regs()[15];
    return s;
}

std::optional<GuestResult> GuestThread::call(std::uint32_t target, const GuestCall& args,
                                             const GuestStopHandler& handle_stop) {
    const auto saved_regs = regs();
    const auto saved_ext = ext_regs();
    const std::uint32_t saved_cpsr = cpsr();
    const std::uint32_t saved_fpscr = fpscr();

    const auto restore = [&] {
        regs() = saved_regs;
        ext_regs() = saved_ext;
        set_cpsr(saved_cpsr);
        set_fpscr(saved_fpscr);
    };

    const std::uint64_t bytes = static_cast<std::uint64_t>(args.stack.size()) * 4;
    if (bytes > saved_regs[13]) return std::nullopt;
    const std::uint32_t call_sp = static_cast<std::uint32_t>((saved_regs[13] - bytes) & ~7u);
    std::uint8_t* stack = mem_.host_ptr(call_sp, bytes, kPageWrite);
    if (bytes != 0 && stack == nullptr) return std::nullopt;
    if (bytes != 0) std::memcpy(stack, args.stack.data(), static_cast<std::size_t>(bytes));

    regs()[0] = args.regs[0];
    regs()[1] = args.regs[1];
    regs()[2] = args.regs[2];
    regs()[3] = args.regs[3];
    regs()[13] = call_sp;
    regs()[14] = kHostReturnAddress;
    regs()[15] = target & ~1u;
    // A fresh call starts outside any IT block with little-endian data; only T follows the target.
    set_cpsr((saved_cpsr & ~(kCpsrThumb | kCpsrItMask | kCpsrEndian)) | ((target & 1u) ? kCpsrThumb : 0));

    ++call_depth;
    std::optional<GuestResult> result;
    for (;;) {
        const Stop stop = run();
        if (stop.kind == StopKind::Svc && stop.swi == kHostReturnSwi) {
            if (regs()[13] == call_sp) {
                result = GuestResult{regs()[0], regs()[1]};
                break;
            }
            // Not this frame's return: the handler treats the svc as an illegal instruction.
            log("host-to-guest return with sp 0x%08x, frame sp 0x%08x: a longjmp or unwind crossed the "
                "host-to-guest call frame",
                regs()[13], call_sp);
        }
        if (!handle_stop(stop)) break;
    }
    --call_depth;
    restore();
    return result;
}

void GuestThread::invalidate(std::uint32_t addr, std::uint32_t len) {
    jit_->InvalidateCacheRange(addr, len);
}

void GuestThread::begin_blocking() {
    blocking_.store(1);
}

void GuestThread::end_blocking() {
    blocking_.store(0);
}

void GuestThread::interrupt() {
    // seq_cst: the poster sets the pending bit before reading this flag, and the waiter sets the
    // flag before it checks for pending signals, so at least one of the two sees the other.
    if (blocking_.load() == 0 || tid == 0) return;
    ::syscall(SYS_tgkill, ::getpid(), tid, host_interrupt_signal());
}

void GuestThread::post_signal(const g::siginfo32& info) {
    const int sig = info.si_signo;
    if (sig < 1 || sig > 64) return;
    pending_info_[static_cast<std::size_t>(sig)] = info;
    pending_signals_.fetch_or(1ULL << (sig - 1));
    jit_->HaltExecution(kInterruptHalt);
    wake();
    interrupt();
}

void GuestThread::park(std::uint32_t token) {
    ::syscall(SYS_futex, &park_word_, FUTEX_WAIT_PRIVATE, token, nullptr, nullptr, 0);
}

void GuestThread::wake() {
    park_word_.fetch_add(1);
    ::syscall(SYS_futex, &park_word_, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

bool GuestThread::take_signal(std::uint64_t blocked, g::siginfo32& out) {
    std::uint64_t pending = pending_signals_.load();
    for (;;) {
        const std::uint64_t deliverable = pending & ~blocked;
        if (deliverable == 0) return false;
        const int bit = std::countr_zero(deliverable);
        if (pending_signals_.compare_exchange_weak(pending, pending & ~(1ULL << bit))) {
            out = pending_info_[static_cast<std::size_t>(bit + 1)];
            return true;
        }
    }
}

void GuestThread::halt() {
    jit_->HaltExecution(kStopHalt);
}

bool GuestThread::check_access(std::uint32_t vaddr, std::uint32_t len, std::uint8_t need, bool write) {
    if (mem_.accessible(vaddr, len, need)) return true;
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::MemoryFault;
        pending_.fault_addr = vaddr;
        pending_.fault_write = write;
    }
    jit_->HaltExecution(kMemoryAbortHalt);
    return false;
}

std::uint8_t GuestThread::MemoryRead8(std::uint32_t vaddr) {
    return check_access(vaddr, 1, kPageRead, false) ? mem_.base()[vaddr] : 0;
}

std::uint16_t GuestThread::MemoryRead16(std::uint32_t vaddr) {
    return check_access(vaddr, 2, kPageRead, false) ? load<std::uint16_t>(mem_.base() + vaddr) : 0;
}

std::uint32_t GuestThread::MemoryRead32(std::uint32_t vaddr) {
    return check_access(vaddr, 4, kPageRead, false) ? load<std::uint32_t>(mem_.base() + vaddr) : 0;
}

std::uint64_t GuestThread::MemoryRead64(std::uint32_t vaddr) {
    return check_access(vaddr, 8, kPageRead, false) ? load<std::uint64_t>(mem_.base() + vaddr) : 0;
}

void GuestThread::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (check_access(vaddr, 1, kPageWrite, true)) mem_.base()[vaddr] = value;
}

void GuestThread::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (check_access(vaddr, 2, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (check_access(vaddr, 4, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (check_access(vaddr, 8, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

namespace {

// Store-exclusive as ONE host compare-and-swap. The ExclusiveMonitor only serializes exclusive
// operations against each other. The owner of a spin lock releases it with a plain store
// ("lock = 0"), and a waiter's retry loop stores 1 even when the lock is taken. With a separate
// compare and write, that release could land between the waiter's compare and its write; the
// waiter's write then put the 1 back, and the lock stayed set with nobody holding it. On real ARM
// a plain store clears the other thread's reservation, so that strex would fail. A compare-and-swap
// gives the same result: if the release came first the compare fails, if it came second it wins.
template <typename T>
bool compare_and_store(std::uint8_t* base, std::uint32_t vaddr, T value, T expected) {
    static const bool atomic_exclusive = option_enabled("atomic_exclusive", true);
    if (!atomic_exclusive || (vaddr & (sizeof(T) - 1)) != 0) {  // switch off, or not naturally aligned
        T current;
        std::memcpy(&current, base + vaddr, sizeof(T));
        if (current != expected) return false;
        std::memcpy(base + vaddr, &value, sizeof(T));
        return true;
    }
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(base + vaddr), &expected, value, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

}  // namespace

bool GuestThread::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) {
    if (!check_access(vaddr, 1, kPageRead | kPageWrite, true)) return false;
    return compare_and_store<std::uint8_t>(mem_.base(), vaddr, value, expected);
}

bool GuestThread::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    if (!check_access(vaddr, 2, kPageRead | kPageWrite, true)) return false;
    return compare_and_store<std::uint16_t>(mem_.base(), vaddr, value, expected);
}

bool GuestThread::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    if (!check_access(vaddr, 4, kPageRead | kPageWrite, true)) return false;
    if (!compare_and_store<std::uint32_t>(mem_.base(), vaddr, value, expected)) return false;
    note_atomic_store(vaddr, tid, value, expected);
    return true;
}

bool GuestThread::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    if (!check_access(vaddr, 8, kPageRead | kPageWrite, true)) return false;
    return compare_and_store<std::uint64_t>(mem_.base(), vaddr, value, expected);
}

std::optional<std::uint32_t> GuestThread::MemoryReadCode(std::uint32_t vaddr) {
    if (!mem_.accessible(vaddr, 4, kPageExec)) return std::nullopt;
    note_code_page(vaddr);  // before the bytes are read, so a flush that follows a rewrite is never skipped
    note_code_page(vaddr + 3);
    g_fetched.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t word = load<std::uint32_t>(mem_.base() + vaddr);
    // The old garbage collector in the game (libgc in libmono) takes its lock with the ARM "swp"
    // instruction, which must swap atomically. The translator does it as a read followed by a
    // write, so two threads can both "win", and a release can be overwritten: the lock then stays
    // set with nobody holding it and every thread sleeps in the lock's retry loop forever. A swp
    // is rewritten here into svc #(0xA50000 | b<<12 | Rn<<8 | Rt<<4 | Rt2), which keeps the same
    // condition code; Process::handle_stop does the swap with a host atomic exchange.
    // Switch: atomic_swp=0 in zb-options.txt. Only ARM mode has swp (Thumb-2 has none), so
    // Thumb code is never touched.
    static const bool atomic_swp = option_enabled("atomic_swp", true);
    if (atomic_swp && (vaddr & 3u) == 0 && (word & 0x0FB00FF0u) == 0x01000090u && (cpsr() & kCpsrThumb) == 0) {
        const std::uint32_t rn = (word >> 16) & 15u;
        const std::uint32_t rt = (word >> 12) & 15u;
        const std::uint32_t rt2 = word & 15u;
        if (rn != 15 && rt != 15 && rt2 != 15 && rn != rt && rn != rt2) {
            static std::atomic<bool> noted{false};
            if (!noted.exchange(true)) {
                runtime_report().note_jni_detail("atomic-swp", "first swp rewritten at pc " + std::to_string(vaddr), true);
            }
            word = (word & 0xF0000000u) | 0x0F000000u | 0xA50000u | (((word >> 22) & 1u) << 12) | (rn << 8) |
                   (rt << 4) | rt2;
        }
    }
    return word;
}

void GuestThread::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    log("interpreter fallback requested at pc 0x%08x (%zu instructions)", pc, num_instructions);
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = Dynarmic::A32::Exception::UndefinedInstruction;
        pending_.pc = pc;
    }
    halt();
}

void GuestThread::CallSVC(std::uint32_t swi) {
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Svc;
        pending_.swi = swi;
    }
    halt();
}

void GuestThread::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = exception;
        pending_.pc = pc;
    }
    halt();
}

}  // namespace zb
