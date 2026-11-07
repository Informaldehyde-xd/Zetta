#include "zb/host_zlib.h"

#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "zb/guest_memory.h"
#include "zb/library_runtime.h"
#include "zb/log.h"
#include "zb/options.h"
#include "zb/runtime_report.h"
#include "zb/zlib_protocol.h"

namespace zb {

namespace {

std::mutex g_mutex;
std::unordered_map<std::uint32_t, std::unique_ptr<z_stream>> g_streams;
std::uint32_t g_next_handle = 1;

bool native_enabled() {
    static const bool enabled = option_enabled("native_zlib", true);
    return enabled;
}

z_stream* find_stream(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(handle);
    return it == g_streams.end() ? nullptr : it->second.get();
}

}  // namespace

bool HostZlib::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_ZLIB_HC_INFLATE_INIT || index > ZB_ZLIB_HC_LAST) return false;
    GuestMemory& memory = runtime_.memory();
    auto& regs = thread.regs();

    if (index == ZB_ZLIB_HC_PROBE) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            log("native zlib is %s", native_enabled() ? "ON" : "OFF (native_zlib=0)");
        }
        regs[0] = native_enabled() ? 1u : 0u;
        return true;
    }

    if (index == ZB_MEM_HC_PROBE) {
        // Copies and fills of at least this many bytes run as native code. Below it the translated libc is as
        // fast as the host call. 0 switches it off (native_mem_min=0 in zb-options.txt).
        static const int minimum = option_number("native_mem_min", 16384);
        regs[0] = minimum > 0 ? static_cast<std::uint32_t>(minimum) : 0u;
        return true;
    }
    if (index == ZB_MEM_HC_COPY || index == ZB_MEM_HC_SET) {
        static std::atomic<std::uint64_t> copies{0}, sets{0}, bytes{0};
        const std::uint32_t destination = regs[0];
        const std::uint32_t length = regs[2];
        std::uint8_t* to = memory.host_ptr(destination, length, kPageWrite);
        if (index == ZB_MEM_HC_COPY) {
            const std::uint8_t* from = memory.host_ptr(regs[1], length, kPageRead);
            if (to == nullptr || from == nullptr) {  // a bad pointer: the guest's own copy will fault in the right place
                regs[0] = 0;
                return true;
            }
            std::memmove(to, from, length);
            copies.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (to == nullptr) {
                regs[0] = 0;
                return true;
            }
            std::memset(to, static_cast<int>(regs[1] & 0xFF), length);
            sets.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t total = bytes.fetch_add(length, std::memory_order_relaxed) + length;
        const std::uint64_t calls = copies.load(std::memory_order_relaxed) + sets.load(std::memory_order_relaxed);
        if ((calls & 1023) == 1) {  // shows in the runtime report, and proves the guest really uses it
            char text[160];
            std::snprintf(text, sizeof text, "%llu copies, %llu fills, %llu MB done natively", static_cast<unsigned long long>(copies.load()),
                          static_cast<unsigned long long>(sets.load()), static_cast<unsigned long long>(total >> 20));
            runtime_report().note_jni_detail("native-mem", text, true);
        }
        regs[0] = 1;
        return true;
    }

    if (index == ZB_ZLIB_HC_MALLOC_PROBE) {
        static const bool keep = option_enabled("malloc_keep_memory", false);
        static bool announced = false;
        if (!announced) {
            announced = true;
            log("guest allocator keeps freed memory: %s", keep ? "ON" : "OFF (malloc_keep_memory=0)");
        }
        regs[0] = keep ? 1u : 0u;
        return true;
    }

    if (index == ZB_ZLIB_HC_CRC32 || index == ZB_ZLIB_HC_ADLER32) {
        const std::uint32_t start = regs[0];
        const std::uint32_t address = regs[1];
        const std::uint32_t length = regs[2];
        const std::uint8_t* data = nullptr;
        if (address != 0 && length != 0) {
            data = memory.host_ptr(address, length, kPageRead);
            if (data == nullptr) {  // the guest passed a bad buffer: leave the value unchanged
                regs[0] = start;
                return true;
            }
        } else if (address != 0) {
            regs[0] = start;
            return true;
        }
        const uLong value = index == ZB_ZLIB_HC_CRC32 ? crc32(start, data, length) : adler32(start, data, length);
        regs[0] = static_cast<std::uint32_t>(value);
        return true;
    }

    if (index == ZB_ZLIB_HC_UNCOMPRESS) {
        const std::uint32_t dest = regs[0];
        const std::uint32_t length_address = regs[1];
        const std::uint32_t source = regs[2];
        const std::uint32_t source_length = regs[3];
        std::uint8_t* length_ptr = memory.host_ptr(length_address, 4, kPageRead | kPageWrite);
        if (length_ptr == nullptr) {
            regs[0] = static_cast<std::uint32_t>(Z_STREAM_ERROR);
            return true;
        }
        std::uint32_t dest_length;
        std::memcpy(&dest_length, length_ptr, 4);
        static std::uint8_t dummy_out[1];
        static const std::uint8_t dummy_in[1] = {0};
        std::uint8_t* out = dummy_out;
        const std::uint8_t* in = dummy_in;
        if (dest_length != 0) {
            out = memory.host_ptr(dest, dest_length, kPageWrite);
            if (out == nullptr) {
                regs[0] = static_cast<std::uint32_t>(Z_STREAM_ERROR);
                return true;
            }
        }
        if (source_length != 0) {
            in = memory.host_ptr(source, source_length, kPageRead);
            if (in == nullptr) {
                regs[0] = static_cast<std::uint32_t>(Z_STREAM_ERROR);
                return true;
            }
        }
        uLongf produced = dest_length;
        const int result = uncompress(out, &produced, in, source_length);
        const std::uint32_t produced32 = static_cast<std::uint32_t>(produced);
        std::memcpy(length_ptr, &produced32, 4);
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }

    // Stream calls: r0 points at a struct zb_zcall in guest memory.
    std::uint8_t* raw = memory.host_ptr(regs[0], sizeof(zb_zcall), kPageRead | kPageWrite);
    if (raw == nullptr) {
        regs[0] = static_cast<std::uint32_t>(Z_STREAM_ERROR);
        return true;
    }
    zb_zcall call;
    std::memcpy(&call, raw, sizeof call);
    auto finish = [&](int result) {
        call.ret = result;
        std::memcpy(raw, &call, sizeof call);
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    };
    auto fill_out = [&](z_stream* zs) {
        call.total_in = static_cast<std::uint32_t>(zs->total_in);
        call.total_out = static_cast<std::uint32_t>(zs->total_out);
        call.adler = static_cast<std::uint32_t>(zs->adler);
        call.data_type = static_cast<std::uint32_t>(zs->data_type);
        std::memset(call.msg, 0, sizeof call.msg);
        if (zs->msg != nullptr) std::strncpy(call.msg, zs->msg, sizeof call.msg - 1);
    };

    if (index == ZB_ZLIB_HC_INFLATE_INIT) {
        auto zs = std::make_unique<z_stream>();
        std::memset(zs.get(), 0, sizeof(z_stream));
        const int result = inflateInit2(zs.get(), static_cast<int>(call.arg));
        if (result != Z_OK) return finish(result);
        fill_out(zs.get());
        std::lock_guard<std::mutex> lock(g_mutex);
        while (g_next_handle == 0 || g_streams.count(g_next_handle) != 0) ++g_next_handle;
        call.handle = g_next_handle++;
        g_streams.emplace(call.handle, std::move(zs));
        return finish(Z_OK);
    }

    z_stream* zs = find_stream(call.handle);
    if (zs == nullptr) return finish(Z_STREAM_ERROR);

    if (index == ZB_ZLIB_HC_INFLATE_END) {
        std::unique_ptr<z_stream> owned;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_streams.find(call.handle);
            if (it != g_streams.end()) {
                owned = std::move(it->second);
                g_streams.erase(it);
            }
        }
        return finish(owned ? inflateEnd(owned.get()) : Z_STREAM_ERROR);
    }

    if (index == ZB_ZLIB_HC_INFLATE_RESET) {
        const int result = inflateReset(zs);
        fill_out(zs);
        return finish(result);
    }

    if (index == ZB_ZLIB_HC_INFLATE_DICT) {
        static const std::uint8_t dummy[1] = {0};
        const std::uint8_t* dictionary = dummy;
        if (call.avail_in != 0) {
            dictionary = memory.host_ptr(call.next_in, call.avail_in, kPageRead);
            if (dictionary == nullptr) return finish(Z_STREAM_ERROR);
        }
        const int result = inflateSetDictionary(zs, dictionary, call.avail_in);
        fill_out(zs);
        return finish(result);
    }

    // ZB_ZLIB_HC_INFLATE
    {
        static std::atomic<std::uint64_t> inflate_calls{0};
        if ((inflate_calls.fetch_add(1, std::memory_order_relaxed) & 4095) == 0) {
            char text[80];
            std::snprintf(text, sizeof text, "%llu inflate calls so far", static_cast<unsigned long long>(inflate_calls.load()));
            runtime_report().note_jni_detail("native-zlib", text, true);
        }
    }
    std::uint8_t* in = nullptr;
    std::uint8_t* out = nullptr;
    if (call.avail_in != 0) {
        in = memory.host_ptr(call.next_in, call.avail_in, kPageRead);
        if (in == nullptr) return finish(Z_STREAM_ERROR);
    }
    if (call.avail_out != 0) {
        out = memory.host_ptr(call.next_out, call.avail_out, kPageWrite);
        if (out == nullptr) return finish(Z_STREAM_ERROR);
    }
    zs->next_in = in;
    zs->avail_in = call.avail_in;
    zs->next_out = out;
    zs->avail_out = call.avail_out;
    const int result = inflate(zs, static_cast<int>(call.arg));
    call.next_in += call.avail_in - zs->avail_in;
    call.next_out += call.avail_out - zs->avail_out;
    call.avail_in = zs->avail_in;
    call.avail_out = zs->avail_out;
    zs->next_in = nullptr;
    zs->next_out = nullptr;
    fill_out(zs);
    return finish(result);
}

}  // namespace zb
