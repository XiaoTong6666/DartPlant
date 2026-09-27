// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include "android_logging.h"
#include "core/internal.h"
#include "vm/abi/resolver.h"

#if defined(__aarch64__) && !defined(MAP_FIXED_NOREPLACE)
#define MAP_FIXED_NOREPLACE 0x100000
#endif

extern "C" void dartplant_arm64_callback_entry();
extern "C" void dartplant_arm64_native_callback_entry();
extern "C" void dartplant_arm64_return_entry();
extern "C" void dartplant_arm64_generated_publication_gate_entry();
extern "C" void dartplant_arm64_native_publication_gate_entry();
extern "C" void dartplant_arm64_dispatch_exception_unwind(uintptr_t target_spreg,
                                                          uintptr_t target_fp);

namespace dartplant {

namespace {

#if defined(__aarch64__)
std::mutex& PublishedReturnPayloadsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<DartCodePayload>>& PublishedReturnPayloads() {
    // A published payload-return veneer can be reached by an unhooked sibling
    // that never contributes to HookRecord::in_flight. Without a process-wide
    // instruction-fetch grace period there is no proof that it is safe to
    // munmap the veneer or destroy its DartCodePayload cookie after restoring
    // the final RET patch. Retain both for process lifetime instead.
    static auto* payloads = new std::vector<std::shared_ptr<DartCodePayload>>();
    return *payloads;
}

bool RetainPublishedReturnPayload(const std::shared_ptr<DartCodePayload>& payload) {
    if (payload == nullptr) return false;
    if (payload->return_entry_published) return true;
    std::lock_guard lock(PublishedReturnPayloadsMutex());
    if (payload->return_entry_published) return true;
    try {
        // Retain before publishing the first branch. If the following code
        // write fails, keeping this payload is conservative but safe; doing
        // the allocation after a successful code write would create an OOM
        // window where a live branch targets an unretained cookie/veneer.
        PublishedReturnPayloads().push_back(payload);
    } catch (...) {
        return false;
    }
    payload->return_entry_published = true;
    return true;
}

void DestroyUnpublishedReturnEntry(DartCodePayload* payload) {
    if (payload == nullptr || payload->return_entry == nullptr || payload->return_entry_published)
        return;
    DestroyArm64CallbackStub(payload->return_entry, payload->return_entry_size);
    payload->return_entry = nullptr;
    payload->return_entry_size = 0;
}
#endif

bool IsDartReturn(uint32_t instruction) { return instruction == 0xd65f03c0U; }

bool IsTerminalTrap(uint32_t instruction) {
    // Dart emits BRK after calls that are semantically noreturn (for example a
    // throwing runtime stub). If such a call unexpectedly returns, BRK ends
    // control flow rather than falling into adjacent Code. Treat the trap as a
    // terminal edge; a separate reachable normal path must still provide RET.
    return (instruction & 0xffe0001fU) == 0xd4200000U;  // BRK #imm16.
}

int64_t SignExtend(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

bool IsIndirectBranch(uint32_t instruction) {
    // A64 unconditional branch-register family. BLR/BLRA* are returning calls
    // (bit 21) and therefore fall through; plain RET x30 is handled before
    // this check. BR/BRA*, RETAA/RETAB, ERET-like or otherwise unsupported
    // register transfers fail closed instead of being mistaken for fallthrough.
    if ((instruction & 0xfe000000U) != 0xd6000000U) return false;
    const bool returning_call = (instruction & (1U << 21)) != 0 && (instruction & (1U << 22)) == 0;
    return !returning_call;
}

bool DecodeDirectBranch(uint32_t instruction, int64_t* out_delta, bool* out_conditional) {
    if (out_delta == nullptr || out_conditional == nullptr) return false;
    if ((instruction & 0xfc000000U) == 0x14000000U) {  // B imm26 (not BL).
        *out_delta = SignExtend(instruction & 0x03ffffffU, 26) << 2;
        *out_conditional = false;
        return true;
    }
    if ((instruction & 0xff000010U) == 0x54000000U) {  // B.cond imm19.
        *out_delta = SignExtend((instruction >> 5) & 0x7ffffU, 19) << 2;
        *out_conditional = true;
        return true;
    }
    if ((instruction & 0x7e000000U) == 0x34000000U) {  // CBZ/CBNZ imm19.
        *out_delta = SignExtend((instruction >> 5) & 0x7ffffU, 19) << 2;
        *out_conditional = true;
        return true;
    }
    if ((instruction & 0x7e000000U) == 0x36000000U) {  // TBZ/TBNZ imm14.
        *out_delta = SignExtend((instruction >> 5) & 0x3fffU, 14) << 2;
        *out_conditional = true;
        return true;
    }
    return false;
}

#if defined(__aarch64__)
constexpr uintptr_t kArm64AdrpReach = uintptr_t{1} << 32;
constexpr uintptr_t kArm64BranchReach = uintptr_t{1} << 27;

using JumpToFrameFn = void (*)(uintptr_t program_counter, uintptr_t stack_pointer,
                               uintptr_t frame_pointer, void* thread);

struct ExceptionBridgeState {
    std::mutex mutex;
    uintptr_t target = 0;
    JumpToFrameFn backup = nullptr;
    std::unique_ptr<PublishedHostHook> published_hook;
    size_t consumers = 0;
};

ExceptionBridgeState& ExceptionBridge() {
    static ExceptionBridgeState state;
    return state;
}

std::atomic<JumpToFrameFn> g_jump_to_frame_backup{nullptr};

uintptr_t Distance(uintptr_t left, uintptr_t right) {
    return left > right ? left - right : right - left;
}

struct NearStubPages {
    uintptr_t base = 0;
    size_t page_size = 0;
    uint32_t page_count = 0;
    uint32_t used_pages = 0;
};

std::mutex& NearStubPagesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<NearStubPages>& NearStubPagePools() {
    // Published callback and RET veneers can still be reached by a stale
    // instruction fetch after logical unhook. Never reuse a consumed page.
    static auto* pools = new std::vector<NearStubPages>();
    return *pools;
}

bool IsPooledNearStubPage(const void* entry) {
    if (entry == nullptr) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(entry);
    std::lock_guard lock(NearStubPagesMutex());
    for (const auto& pool : NearStubPagePools()) {
        const size_t span = pool.page_size * pool.page_count;
        if (address >= pool.base && address - pool.base < span &&
            (address - pool.base) % pool.page_size == 0) {
            return true;
        }
    }
    return false;
}

bool NearSpanFits(uintptr_t start, size_t span, uintptr_t target, uintptr_t reach) {
    if (span == 0 || start > UINTPTR_MAX - (span - 1)) return false;
    return Distance(start, target) < reach && Distance(start + span - 1, target) < reach;
}

void* ReserveNearStubPages(uintptr_t target_page, uintptr_t target, uintptr_t reach,
                           size_t page_size, uint32_t page_count) {
    const size_t span = page_size * page_count;
    uint32_t hint_failed = 0;
    uint32_t hint_out_of_reach = 0;
    uintptr_t last_hint_mapping = 0;
    uint32_t fixed_failed = 0;
    uint32_t fixed_exists = 0;
    uint32_t fixed_invalid = 0;
    uint32_t fixed_nomem = 0;
    uint32_t fixed_other = 0;
    int last_fixed_errno = 0;
    const auto accept = [&](void* mapped) -> void* {
        if (mapped == MAP_FAILED) return nullptr;
        if (NearSpanFits(reinterpret_cast<uintptr_t>(mapped), span, target, reach)) return mapped;
        munmap(mapped, span);
        return nullptr;
    };

    // A non-fixed address is only a hint, never permission to overwrite an
    // image. Accept the returned address only after proving the *whole* pool
    // is in direct-B reach. This also works on native-translation runtimes
    // that do not honor a particular MAP_FIXED_NOREPLACE candidate.
    constexpr uintptr_t kHintStep = uintptr_t{8} << 20;
    for (uintptr_t delta = kHintStep; delta < reach; delta += kHintStep) {
        const uintptr_t hints[2] = {
            target_page <= UINTPTR_MAX - delta ? target_page + delta : 0,
            target_page >= delta ? target_page - delta : 0,
        };
        for (uintptr_t hint : hints) {
            if (hint == 0) continue;
            void* requested = mmap(reinterpret_cast<void*>(hint), span, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (requested == MAP_FAILED) {
                ++hint_failed;
                continue;
            }
            if (!NearSpanFits(reinterpret_cast<uintptr_t>(requested), span, target, reach)) {
                ++hint_out_of_reach;
                last_hint_mapping = reinterpret_cast<uintptr_t>(requested);
            }
            if (void* mapped = accept(requested); mapped != nullptr) {
                return mapped;
            }
        }
    }

    // Search exact holes if non-fixed hints were redirected. A large pool is
    // scanned at its own span first; the one-page fallback scans every page.
    const uintptr_t step = span;
    for (uintptr_t delta = page_size; delta < reach; delta += step) {
        const uintptr_t candidates[2] = {
            target_page <= UINTPTR_MAX - delta ? target_page + delta : 0,
            target_page >= delta + span - page_size ? target_page - delta - (span - page_size) : 0,
        };
        for (uintptr_t candidate : candidates) {
            if (candidate == 0 || !NearSpanFits(candidate, span, target, reach)) continue;
            void* requested = mmap(reinterpret_cast<void*>(candidate), span, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (requested == MAP_FAILED) {
                ++fixed_failed;
                last_fixed_errno = errno;
                switch (errno) {
                case EEXIST:
                    ++fixed_exists;
                    break;
                case EINVAL:
                    ++fixed_invalid;
                    break;
                case ENOMEM:
                    ++fixed_nomem;
                    break;
                default:
                    ++fixed_other;
                    break;
                }
                continue;
            }
            if (void* mapped = accept(requested); mapped != nullptr) {
                return mapped;
            }
        }
    }
#if defined(__ANDROID__)
    AndroidLogPrint(ANDROID_LOG_ERROR, "CallbackStub",
                    "near mmap failed target=0x%llx pages=%u hint_fail=%u hint_far=%u "
                    "last_hint=0x%llx fixed_fail=%u exists=%u invalid=%u nomem=%u other=%u "
                    "last_errno=%d",
                    static_cast<unsigned long long>(target), page_count, hint_failed,
                    hint_out_of_reach, static_cast<unsigned long long>(last_hint_mapping),
                    fixed_failed, fixed_exists, fixed_invalid, fixed_nomem, fixed_other,
                    last_fixed_errno);
#endif
    return nullptr;
}

void* AllocateNearStubPage(uintptr_t target, uintptr_t reach, size_t page_size) {
    std::lock_guard lock(NearStubPagesMutex());
    auto& pools = NearStubPagePools();
    for (auto& pool : pools) {
        if (pool.page_size != page_size) continue;
        for (uint32_t index = 0; index < pool.page_count; ++index) {
            const uint32_t bit = uint32_t{1} << index;
            if ((pool.used_pages & bit) != 0) continue;
            const uintptr_t page = pool.base + static_cast<uintptr_t>(index) * page_size;
            if (!NearSpanFits(page, page_size, target, reach)) continue;
            pool.used_pages |= bit;
            if (mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE) == 0) {
                return reinterpret_cast<void*>(page);
            }
            // The failed page is never reused. Do not release another page
            // that a future instruction-fetch continuation may have named.
            munmap(reinterpret_cast<void*>(page), page_size);
        }
    }

    try {
        pools.reserve(pools.size() + 1);
    } catch (...) {
        return nullptr;
    }
    const uintptr_t target_page = target & ~(static_cast<uintptr_t>(page_size) - 1);
    for (uint32_t page_count : {32U, 8U, 1U}) {
        void* mapped = ReserveNearStubPages(target_page, target, reach, page_size, page_count);
        if (mapped == nullptr) continue;
        pools.push_back({reinterpret_cast<uintptr_t>(mapped), page_size, page_count, 1});
        if (mprotect(mapped, page_size, PROT_READ | PROT_WRITE) == 0) return mapped;
        munmap(mapped, page_size);
        return nullptr;
    }
#if defined(__ANDROID__)
    AndroidLogPrint(ANDROID_LOG_ERROR, "CallbackStub",
                    "no direct-branch-reachable page for target=0x%llx reach=0x%llx page=%zu",
                    static_cast<unsigned long long>(target), static_cast<unsigned long long>(reach),
                    page_size);
#endif
    return nullptr;
}

enum class SharedVeneerKind : uint8_t { kCallback, kReturn, kGeneratedGate };

struct SharedVeneerSlot {
    std::atomic<void*> cookie{nullptr};
    bool issued = false;
};

struct SharedVeneerPage {
    uintptr_t base = 0;
    size_t page_size = 0;
    size_t slot_count = 0;
    std::unique_ptr<SharedVeneerSlot[]> slots;
};

std::mutex& SharedVeneerPagesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::unique_ptr<SharedVeneerPage>>& SharedVeneerPages() {
    // Executable slots and the separate atomic cookie cells outlive all
    // published HookRecords/CodePayloads. A slot is issued at most once.
    static auto* pages = new std::vector<std::unique_ptr<SharedVeneerPage>>();
    return *pages;
}

std::pair<size_t, size_t> SharedVeneerRange(SharedVeneerKind kind, size_t count) {
    const size_t third = count / 3;
    switch (kind) {
    case SharedVeneerKind::kCallback:
        return {0, third};
    case SharedVeneerKind::kReturn:
        return {third, 2 * third};
    case SharedVeneerKind::kGeneratedGate:
        return {2 * third, count};
    }
    return {0, 0};
}

void* SharedVeneerDispatcher(SharedVeneerKind kind) {
    switch (kind) {
    case SharedVeneerKind::kCallback:
        return reinterpret_cast<void*>(&dartplant_arm64_callback_entry);
    case SharedVeneerKind::kReturn:
        return reinterpret_cast<void*>(&dartplant_arm64_return_entry);
    case SharedVeneerKind::kGeneratedGate:
        return reinterpret_cast<void*>(&dartplant_arm64_generated_publication_gate_entry);
    }
    return nullptr;
}

void* AllocateSharedVeneer(SharedVeneerKind kind, void* cookie, uintptr_t target,
                           size_t* out_size) {
    if (cookie == nullptr || target == 0 || out_size == nullptr) return nullptr;
    constexpr size_t kSlotSize = 32;
    const long raw_page_size = sysconf(_SC_PAGESIZE);
    if (raw_page_size <= 0) return nullptr;
    const size_t page_size = static_cast<size_t>(raw_page_size);
    if (page_size < 3 * kSlotSize || page_size % kSlotSize != 0) return nullptr;

    std::lock_guard lock(SharedVeneerPagesMutex());
    auto& pages = SharedVeneerPages();
    const auto issue = [&](SharedVeneerPage& page) -> void* {
        if (page.page_size != page_size) return nullptr;
        const auto [begin, end] = SharedVeneerRange(kind, page.slot_count);
        for (size_t index = begin; index < end; ++index) {
            SharedVeneerSlot& slot = page.slots[index];
            const uintptr_t entry = page.base + index * kSlotSize;
            if (slot.issued || !NearSpanFits(entry, kSlotSize, target, kArm64BranchReach)) {
                continue;
            }
            // The page is already RX and must never be patched again. The
            // release-store publishes the complete immutable cookie to LDAR
            // before the caller can publish a branch to this slot.
            slot.issued = true;
            slot.cookie.store(cookie, std::memory_order_release);
            *out_size = 0;  // Shared page has no per-veneer munmap ownership.
            return reinterpret_cast<void*>(entry);
        }
        return nullptr;
    };
    for (const auto& page : pages) {
        if (void* entry = issue(*page); entry != nullptr) return entry;
    }

    std::unique_ptr<SharedVeneerPage> page;
    try {
        pages.reserve(pages.size() + 1);
        page = std::make_unique<SharedVeneerPage>();
        page->page_size = page_size;
        page->slot_count = page_size / kSlotSize;
        page->slots = std::make_unique<SharedVeneerSlot[]>(page->slot_count);
    } catch (...) {
        return nullptr;
    }
    void* mapped = AllocateNearStubPage(target, kArm64BranchReach, page_size);
    if (mapped == nullptr) return nullptr;
    page->base = reinterpret_cast<uintptr_t>(mapped);
    for (size_t index = 0; index < page->slot_count; ++index) {
        SharedVeneerKind slot_kind = SharedVeneerKind::kGeneratedGate;
        if (index < page->slot_count / 3) {
            slot_kind = SharedVeneerKind::kCallback;
        } else if (index < 2 * (page->slot_count / 3)) {
            slot_kind = SharedVeneerKind::kReturn;
        }
        auto* code = reinterpret_cast<uint32_t*>(page->base + index * kSlotSize);
        code[0] = 0x58000091;  // ldr x17, +16 (atomic cookie cell address).
        code[1] = 0xc8dffe31;  // ldar x17, [x17] (acquire published cookie).
        code[2] = 0x58000090;  // ldr x16, +16 (common dispatcher at +24).
        code[3] = 0xd61f0200;  // br x16.
        auto* cell = &page->slots[index].cookie;
        std::memcpy(reinterpret_cast<uint8_t*>(code) + 16, &cell, sizeof(cell));
        void* dispatcher = SharedVeneerDispatcher(slot_kind);
        std::memcpy(reinterpret_cast<uint8_t*>(code) + 24, &dispatcher, sizeof(dispatcher));
    }
    __builtin___clear_cache(reinterpret_cast<char*>(mapped),
                            reinterpret_cast<char*>(mapped) + page_size);
    if (mprotect(mapped, page_size, PROT_READ | PROT_EXEC) != 0) {
        // The unused near page remains owned by the process pool; no veneer
        // was issued and no instruction branch could have been published.
        return nullptr;
    }
    pages.push_back(std::move(page));
    return issue(*pages.back());
}

void* AllocateCallbackStub(size_t allocation_size, uintptr_t target, uintptr_t required_reach) {
    if (target != 0) {
        const long page_size_value = sysconf(_SC_PAGESIZE);
        if (page_size_value <= 0) return nullptr;
        const uintptr_t page_size = static_cast<uintptr_t>(page_size_value);

        // A patched RET has only B +/-128 MiB of reach. Reuse an unconsumed
        // page from a verified near pool, or reserve a new pool without
        // replacing existing Dart/Flutter mappings.
        if (required_reach != 0 && required_reach <= kArm64BranchReach) {
            if (allocation_size != page_size) return nullptr;
            return AllocateNearStubPage(target, required_reach, page_size);
        }

        // A non-null mmap address is a hint, not MAP_FIXED: it cannot replace
        // an existing mapping. Try both sides of the Dart image at increasing
        // distances and accept only a mapping reachable by AArch64 ADRP.
        constexpr uintptr_t kStep = uintptr_t{16} << 20;
        const uintptr_t reach = required_reach == 0 ? kArm64AdrpReach : required_reach;
        const uint32_t attempts_per_side =
            static_cast<uint32_t>(std::max<uintptr_t>(1, (reach - 1) / kStep));
        for (uint32_t attempt = 1; attempt <= attempts_per_side; ++attempt) {
            const uintptr_t delta = kStep * attempt;
            const uintptr_t hints[2] = {
                target <= UINTPTR_MAX - delta ? target + delta : 0,
                target >= delta ? target - delta : 0,
            };
            for (uintptr_t hint : hints) {
                if (hint == 0) continue;
                void* mapped = mmap(reinterpret_cast<void*>(hint), allocation_size,
                                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (mapped == MAP_FAILED) continue;
                if (Distance(reinterpret_cast<uintptr_t>(mapped), target) < reach) {
                    return mapped;
                }
                munmap(mapped, allocation_size);
            }
        }
    }
    if (required_reach != 0) return nullptr;
    void* mapped =
        mmap(nullptr, allocation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return mapped == MAP_FAILED ? nullptr : mapped;
}

bool EncodeDirectBranch(uintptr_t from, uintptr_t to, uint32_t* out_instruction) {
    if (out_instruction == nullptr || (from & 3U) != 0 || (to & 3U) != 0) return false;
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if ((delta & 3) != 0 || delta <= -static_cast<int64_t>(kArm64BranchReach) ||
        delta >= static_cast<int64_t>(kArm64BranchReach)) {
        return false;
    }
    const int64_t imm26 = delta >> 2;
    *out_instruction = 0x14000000U | (static_cast<uint32_t>(imm26) & 0x03ffffffU);
    return true;
}

bool WriteExecutableInstruction(uintptr_t address, uint32_t instruction) {
    const long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) return false;
    const uintptr_t page_size = static_cast<uintptr_t>(page_size_value);
    const uintptr_t page = address & ~(page_size - 1);
    if (mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE | PROT_EXEC) !=
        0) {
        return false;
    }
    auto* slot = reinterpret_cast<uint32_t*>(address);
    __atomic_store_n(slot, instruction, __ATOMIC_RELEASE);
    __builtin___clear_cache(reinterpret_cast<char*>(address),
                            reinterpret_cast<char*>(address + sizeof(uint32_t)));
    return mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_EXEC) == 0;
}

bool ReadSelfWord(uintptr_t address, uintptr_t* out) {
    if (address == 0 || out == nullptr) return false;
    iovec local{out, sizeof(*out)};
    iovec remote{reinterpret_cast<void*>(address), sizeof(*out)};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(sizeof(*out));
}

bool IsKnownExecutableAddress(uintptr_t address) {
    std::lock_guard lock(State().mutex);
    for (const auto& module : State().modules) {
        if (module.ContainsExecutable(address, sizeof(uint32_t))) return true;
    }
    return false;
}
#endif

}  // namespace

#if defined(__aarch64__)
extern "C"
    [[noreturn]] void dartplant_arm64_jump_to_frame_hook(uintptr_t program_counter,
                                                         uintptr_t stack_pointer,
                                                         uintptr_t frame_pointer, void* thread) {
    dartplant_arm64_dispatch_exception_unwind(stack_pointer, frame_pointer);
    JumpToFrameFn backup = g_jump_to_frame_backup.load(std::memory_order_acquire);
    if (backup == nullptr) __builtin_trap();
    backup(program_counter, stack_pointer, frame_pointer, thread);
    __builtin_unreachable();
}
#endif

void RegisterArm64ExceptionBridgeConsumer(DartPlantHook* hook) {
#if defined(__aarch64__)
    if (hook == nullptr || hook->exception_bridge_consumer) return;
    auto& state = ExceptionBridge();
    std::lock_guard lock(state.mutex);
    ++state.consumers;
    hook->exception_bridge_consumer = true;
#else
    (void) hook;
#endif
}

void ReleaseArm64ExceptionBridgeConsumer(DartPlantHook* hook) {
#if defined(__aarch64__)
    if (hook == nullptr || !hook->exception_bridge_consumer) return;
    auto& state = ExceptionBridge();
    std::lock_guard lock(state.mutex);
    hook->exception_bridge_consumer = false;
    if (state.consumers != 0) --state.consumers;
    // JumpToFrame is a noreturn transfer into Dart's exception handler. There
    // is no post-call quiescence point where this bridge can safely prove that
    // the current replacement and its backup trampoline are no longer being
    // executed on any thread. In particular, exception cleanup may make the
    // last DartPlant invocation leave kUnhooking while this very replacement
    // is still running. Keep the process-global bridge installed once it has
    // been established; consumers only track logical users. This guarantees
    // that g_jump_to_frame_backup and the backend trampoline stay valid across
    // self-unhook, runtime retirement, and cross-thread exception races.
#else
    (void) hook;
#endif
}

bool EnsureArm64ExceptionBridge(
    DartPlantHook* hook, const DartPlantArm64Context& context,
    const std::shared_ptr<DartPlantListenerRecord>& execution_listener) {
#if defined(__aarch64__)
    const DartPlantMethod* method =
        execution_listener != nullptr && execution_listener->requested_method != nullptr
            ? execution_listener->requested_method.get()
            : (hook == nullptr ? nullptr : hook->method_storage.get());
    if (hook == nullptr || method == nullptr || method->function == nullptr ||
        method->function->source == DartFunctionSource::kSynthetic) {
        return true;
    }
    if (execution_listener == nullptr) {
        // No listener owns this IsolateGroup. Do not dereference the
        // HookRecord's historical first-owner adapter: that owner may already
        // have retired while the process-wide entry patch remains shared by
        // sibling owners. A previously installed JumpToFrame bridge is
        // process-global and sufficient for passthrough bookkeeping; otherwise
        // fail closed and retain the publication entrant.
        auto& state = ExceptionBridge();
        std::lock_guard lock(state.mutex);
        return state.target != 0 && state.backup != nullptr && state.published_hook != nullptr;
    }
    const uintptr_t thread = static_cast<uintptr_t>(context.x[26]);
    const auto& binding = execution_listener != nullptr
                              ? execution_listener->exception_bridge_binding
                              : hook->exception_bridge_binding;
    DartPlantVmAdapter* vm_adapter =
        execution_listener != nullptr ? execution_listener->vm_adapter : hook->vm_adapter;
    const auto& runtime_generation = execution_listener != nullptr
                                         ? execution_listener->runtime_generation
                                         : hook->runtime_generation;
    const uint64_t expected_runtime_generation =
        execution_listener != nullptr ? execution_listener->expected_runtime_generation
                                      : hook->expected_runtime_generation;
    const bool adapter_bound_live =
        method->function->source == DartFunctionSource::kLiveVm && vm_adapter != nullptr;
    if (thread == 0 || !binding.verified ||
        ((adapter_bound_live &&
          (binding.target == 0 || runtime_generation == nullptr ||
           runtime_generation->load(std::memory_order_acquire) != expected_runtime_generation)) ||
         (!adapter_bound_live && binding.target == 0 && binding.thread_offset == 0))) {
        SetLastError("Dart JumpToFrame capability binding is unavailable or stale");
        return false;
    }
    if (vm_adapter != nullptr) {
        DartPlantVmCapabilityProof proof{};
        proof.struct_size = sizeof(proof);
        const RuntimeProfileRecord* profile = nullptr;
        if (VmAdapterGetCapabilityBinding(vm_adapter, DARTPLANT_VM_CAP_EXCEPTION_BRIDGE_LAYOUT,
                                          &proof, &profile) != DARTPLANT_OK ||
            profile == nullptr || proof.resolved_target != binding.target ||
            proof.artifact_generation != binding.artifact_generation ||
            proof.isolate_generation != binding.isolate_generation ||
            dartplant::vm_abi::BuildCapabilityAbiKey(
                *profile, dartplant::vm_abi::kCapabilityExceptionBridgeLayout) !=
                binding.abi_domain_key) {
            SetLastError("Dart JumpToFrame capability binding changed after hook admission");
            return false;
        }
    }
    uintptr_t target = binding.target;
    if (binding.target == 0) {
        if (binding.thread_offset == 0 || !ReadSelfWord(thread + binding.thread_offset, &target)) {
            SetLastError("failed to resolve offline Dart JumpToFrame target");
            return false;
        }
    }
    if (method->function->source == DartFunctionSource::kLiveVm &&
        !IsKnownExecutableAddress(target)) {
        SetLastError("verified Dart JumpToFrame target is no longer executable");
        return false;
    }

    auto& state = ExceptionBridge();
    std::lock_guard lock(state.mutex);
    if (state.target != 0) {
        if (state.target != target) {
            SetLastError(
                "Dart JumpToFrame target changed after the process exception bridge was installed");
            return false;
        }
        return state.backup != nullptr && state.published_hook != nullptr;
    }
    const HostApiBinding* host_binding = hook->host_binding;
    if (!HostBindingSupportsPublishedHooks(host_binding)) {
        SetLastError("exception bridge host cannot safely publish Dart control flow");
        return false;
    }

    auto published = std::make_unique<PublishedHostHook>();
    DartPlantStatus status = PreparePublishedHostHook(
        published.get(), host_binding, target,
        reinterpret_cast<void*>(&dartplant_arm64_jump_to_frame_hook), false);
    if (status != DARTPLANT_OK) return false;

    void* backup = nullptr;
    status = InstallPublishedHostHook(published.get(), &backup);
    if (status != DARTPLANT_OK || backup == nullptr) {
        if (published->ever_published) {
            RetainPublishedHostHookForProcessLifetime(std::move(published));
        } else {
            DestroyPublishedHostHookGate(published.get());
        }
        SetLastError("failed to hook Dart JumpToFrame for exception cleanup");
        return false;
    }

    state.target = target;
    state.backup = reinterpret_cast<JumpToFrameFn>(backup);
    state.published_hook = std::move(published);
    // The gate is still INSTALLING here. Publish the callable original first,
    // then arm replacement reachability with a release store.
    g_jump_to_frame_backup.store(state.backup, std::memory_order_release);
    ArmPublishedHostHook(state.published_hook.get());
    return true;
#else
    (void) hook;
    (void) context;
    (void) execution_listener;
    return false;
#endif
}

void* CreateArm64HostPublicationGateStub(HostPublicationGate* gate, uintptr_t target,
                                         bool track_generated_entrants, size_t* out_size) {
#if defined(__aarch64__)
    if (gate == nullptr || target == 0 || out_size == nullptr) return nullptr;
    if (track_generated_entrants) {
        return AllocateSharedVeneer(SharedVeneerKind::kGeneratedGate, gate, target, out_size);
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return nullptr;
    const size_t allocation_size = static_cast<size_t>(page_size);
    // Keep generated-entry gates inside direct-B reach when possible. This is
    // friendly to hosts that enable Dobby's near-branch plugin while still
    // leaving the host responsible for the physical target patch.
    auto* code = static_cast<uint32_t*>(AllocateCallbackStub(
        allocation_size, target, track_generated_entrants ? kArm64BranchReach : 0));
    if (code == nullptr) return nullptr;

    code[0] = 0x58000091;  // ldr x17, +16 (HostPublicationGate*).
    code[1] = 0x580000b0;  // ldr x16, +20 (common gate entry).
    code[2] = 0xd61f0200;  // br x16.
    code[3] = 0xd503201f;  // nop.
    std::memcpy(reinterpret_cast<uint8_t*>(code) + 16, &gate, sizeof(gate));
    void* common = track_generated_entrants
                       ? reinterpret_cast<void*>(&dartplant_arm64_generated_publication_gate_entry)
                       : reinterpret_cast<void*>(&dartplant_arm64_native_publication_gate_entry);
    std::memcpy(reinterpret_cast<uint8_t*>(code) + 24, &common, sizeof(common));
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code) + 32);
    if (mprotect(code, allocation_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, allocation_size);
        return nullptr;
    }
    *out_size = allocation_size;
    return code;
#else
    (void) gate;
    (void) target;
    (void) track_generated_entrants;
    (void) out_size;
    return nullptr;
#endif
}

void* CreateArm64CallbackStub(DartPlantHook* hook, uintptr_t target, size_t* out_size) {
#if defined(__aarch64__)
    if (hook == nullptr || out_size == nullptr) return nullptr;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return nullptr;
    const size_t allocation_size = static_cast<size_t>(page_size);
    const bool synthetic_native =
        hook->method_storage != nullptr && hook->method_storage->function != nullptr &&
        hook->method_storage->function->source == DartFunctionSource::kSynthetic;
    if (!synthetic_native) {
        // All executable instructions were frozen while the shared page was
        // still private RW. This slot publishes only a one-time cookie.
        return AllocateSharedVeneer(SharedVeneerKind::kCallback, hook, target, out_size);
    }
    // Real Dart hooks also patch each RET to a per-hook return veneer. AArch64
    // B has a +/-128 MiB range, which is stricter than ADRP and therefore also
    // avoids the backend's far absolute-literal entry trampoline. Synthetic
    // native fixtures keep the older entry-only requirement.
    const uintptr_t required_reach =
        synthetic_native ? ((target & (alignof(uint64_t) - 1)) != 0 ? kArm64AdrpReach : 0)
                         : kArm64BranchReach;
    auto* code =
        static_cast<uint32_t*>(AllocateCallbackStub(allocation_size, target, required_reach));
    if (code == nullptr) return nullptr;

    // x16/x17 are AArch64 IP0/IP1 scratch registers. The veneer places the hook
    // in x17 and tail-branches to the common entry without changing x30.
    code[0] = 0x58000091;
    // Instruction is at +4; a 20-byte literal offset lands at the common entry
    // literal at +24.
    code[1] = 0x580000b0;
    code[2] = 0xd61f0200;
    code[3] = 0xd503201f;
    std::memcpy(reinterpret_cast<uint8_t*>(code) + 16, &hook, sizeof(hook));
    void* common = synthetic_native
                       ? reinterpret_cast<void*>(&dartplant_arm64_native_callback_entry)
                       : reinterpret_cast<void*>(&dartplant_arm64_callback_entry);
    std::memcpy(reinterpret_cast<uint8_t*>(code) + 24, &common, sizeof(common));

    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code) + 32);
    if (mprotect(code, allocation_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, allocation_size);
        return nullptr;
    }
    *out_size = allocation_size;
    return code;
#else
    (void) hook;
    (void) target;
    (void) out_size;
    return nullptr;
#endif
}

void* CreateArm64PayloadReturnStub(DartCodePayload* payload, uintptr_t target, size_t* out_size) {
#if defined(__aarch64__)
    if (payload == nullptr || out_size == nullptr || target == 0) return nullptr;
    return AllocateSharedVeneer(SharedVeneerKind::kReturn, payload, target, out_size);
#else
    (void) payload;
    (void) target;
    (void) out_size;
    return nullptr;
#endif
}

bool CollectReachableArm64Returns(const uint8_t* code, size_t size, uintptr_t logical_start,
                                  std::vector<Arm64ReturnPatch>* out_returns) {
    if (code == nullptr || out_returns == nullptr || size < sizeof(uint32_t) ||
        (logical_start & 3U) != 0 || (size & 3U) != 0 || logical_start > UINTPTR_MAX - size) {
        return false;
    }
    try {
        out_returns->clear();
        const size_t instruction_count = size / sizeof(uint32_t);
        std::vector<uint8_t> visited(instruction_count, 0);
        std::vector<size_t> pending{0};
        const uintptr_t logical_end = logical_start + size;

        const auto enqueue = [&](uintptr_t target, std::vector<size_t>* worklist) -> bool {
            if (target < logical_start || target >= logical_end || (target & 3U) != 0) return false;
            const size_t index = static_cast<size_t>((target - logical_start) / sizeof(uint32_t));
            if (index >= instruction_count) return false;
            worklist->push_back(index);
            return true;
        };

        while (!pending.empty()) {
            const size_t index = pending.back();
            pending.pop_back();
            if (visited[index] != 0) continue;
            visited[index] = 1;
            uint32_t instruction = 0;
            std::memcpy(&instruction, code + index * sizeof(uint32_t), sizeof(instruction));
            const uintptr_t pc = logical_start + index * sizeof(uint32_t);
            if (IsDartReturn(instruction)) {
                out_returns->push_back({.address = pc, .original_instruction = instruction});
                continue;
            }
            if (IsTerminalTrap(instruction)) continue;
            if (IsIndirectBranch(instruction)) return false;

            int64_t branch_delta = 0;
            bool conditional = false;
            if (DecodeDirectBranch(instruction, &branch_delta, &conditional)) {
                const int64_t signed_pc = static_cast<int64_t>(pc);
                const int64_t signed_target = signed_pc + branch_delta;
                if (signed_target < 0 ||
                    !enqueue(static_cast<uintptr_t>(signed_target), &pending)) {
                    return false;
                }
                if (!conditional) continue;
            }

            // BL/BLR are returning calls, so their only intra-Function successor is
            // the fallthrough instruction. All ordinary instructions fall through
            // as well. Reaching the end without a RET/taken B is an unsupported
            // tail exit rather than permission to scan into an adjacent Code body.
            if (index + 1 >= instruction_count) return false;
            pending.push_back(index + 1);
        }
        std::sort(out_returns->begin(), out_returns->end(),
                  [](const Arm64ReturnPatch& left, const Arm64ReturnPatch& right) {
                      return left.address < right.address;
                  });
        out_returns->erase(
            std::unique(out_returns->begin(), out_returns->end(),
                        [](const Arm64ReturnPatch& left, const Arm64ReturnPatch& right) {
                            return left.address == right.address;
                        }),
            out_returns->end());
        return !out_returns->empty();
    } catch (...) {
        out_returns->clear();
        return false;
    }
}

DartPlantStatus InstallArm64ReturnInterception(DartPlantHook* hook) {
#if defined(__aarch64__)
    if (hook == nullptr || hook->code_target == nullptr || hook->code_target->payload == nullptr ||
        hook->code_target->entry == 0 || hook->code_target->code_size < sizeof(uint32_t)) {
        SetLastError("Dart callback hook has no exact code range for exception-safe returns");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    auto payload = hook->code_target->payload;
    std::lock_guard payload_lock(payload->mutex);
    const uintptr_t start = hook->code_target->entry;
    const uintptr_t end = start + hook->code_target->code_size;
    const uintptr_t payload_end = payload->end();
    if (end <= start || (start & 3U) != 0 || payload_end == 0 || end > payload_end ||
        !payload->Contains(start, hook->code_target->code_size) ||
        payload->pristine_bytes.size() != payload->instructions_length) {
        SetLastError("Dart callback code range is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (payload->return_entry == nullptr) {
        payload->return_entry = CreateArm64PayloadReturnStub(payload.get(), payload->start,
                                                             &payload->return_entry_size);
        if (payload->return_entry == nullptr) {
            SetLastError("failed to allocate payload-level ARM64 return veneer");
            return DARTPLANT_HOOK_FAILED;
        }
    }

    std::vector<Arm64ReturnPatch> candidates;
    const size_t pristine_offset = static_cast<size_t>(start - payload->start);
    if (!CollectReachableArm64Returns(payload->pristine_bytes.data() + pristine_offset, end - start,
                                      start, &candidates)) {
        SetLastError(
            "Dart callback entry has no complete reachable ARM64 RET graph; indirect/tail exits fail closed");
        if (payload->return_interception_consumers == 0 && payload->return_patches.empty()) {
            DestroyUnpublishedReturnEntry(payload.get());
        }
        return DARTPLANT_UNSUPPORTED_ABI;
    }

    const uintptr_t return_entry = reinterpret_cast<uintptr_t>(payload->return_entry);
    std::vector<Arm64ReturnPatch> newly_installed;
    std::vector<Arm64ReturnPatch*> existing_consumed;
    std::vector<uintptr_t> acquired_sites;
    std::vector<Arm64ReturnPatch> residual;
    try {
        newly_installed.reserve(candidates.size());
        existing_consumed.reserve(candidates.size());
        acquired_sites.reserve(candidates.size());
        residual.reserve(candidates.size());
        if (candidates.size() >
            payload->return_patches.max_size() - payload->return_patches.size()) {
            SetLastError("too many ARM64 Dart return interception sites");
            if (payload->return_interception_consumers == 0 && payload->return_patches.empty()) {
                DestroyUnpublishedReturnEntry(payload.get());
            }
            return DARTPLANT_HOOK_FAILED;
        }
        payload->return_patches.reserve(payload->return_patches.size() + candidates.size());
    } catch (...) {
        SetLastError("failed to reserve ARM64 Dart return interception ownership");
        if (payload->return_interception_consumers == 0 && payload->return_patches.empty()) {
            DestroyUnpublishedReturnEntry(payload.get());
        }
        return DARTPLANT_HOOK_FAILED;
    }
    const auto rollback_newly_installed = [&](const char* error) {
        residual.clear();
        bool clean_rollback = true;
        for (auto it = newly_installed.rbegin(); it != newly_installed.rend(); ++it) {
            uint32_t current = 0;
            std::memcpy(&current, reinterpret_cast<const void*>(it->address), sizeof(current));
            if (current == it->original_instruction) continue;
            if (current == it->patched_instruction &&
                WriteExecutableInstruction(it->address, it->original_instruction)) {
                continue;
            }
            clean_rollback = false;
            std::memcpy(&current, reinterpret_cast<const void*>(it->address), sizeof(current));
            if (current == it->patched_instruction) residual.push_back(*it);
        }
        if (!residual.empty()) {
            hook->payload_return_sites.clear();
            for (auto& patch : residual) {
                patch.consumers = 1;
                hook->payload_return_sites.push_back(patch.address);
                payload->return_patches.push_back(patch);
            }
            hook->payload_return_consumer = true;
            ++payload->return_interception_consumers;
        }
        if (payload->return_interception_consumers == 0 && payload->return_patches.empty()) {
            DestroyUnpublishedReturnEntry(payload.get());
        }
        SetLastError(
            clean_rollback
                ? error
                : "ARM64 Dart return interception failed and could not be fully rolled back");
        return DARTPLANT_HOOK_FAILED;
    };
    for (const auto& candidate : candidates) {
        auto existing = std::find_if(payload->return_patches.begin(), payload->return_patches.end(),
                                     [&candidate](const Arm64ReturnPatch& patch) {
                                         return patch.address == candidate.address;
                                     });
        uint32_t current = 0;
        std::memcpy(&current, reinterpret_cast<const void*>(candidate.address), sizeof(current));
        if (existing != payload->return_patches.end()) {
            if (existing->original_instruction != candidate.original_instruction ||
                existing->consumers == 0 || current != existing->patched_instruction) {
                return rollback_newly_installed(
                    "existing payload RET patch no longer matches managed ownership");
            }
            existing_consumed.push_back(&*existing);
            acquired_sites.push_back(candidate.address);
            continue;
        }
        uint32_t branch = 0;
        if (current != candidate.original_instruction ||
            !EncodeDirectBranch(candidate.address, return_entry, &branch) ||
            !RetainPublishedReturnPayload(payload) ||
            !WriteExecutableInstruction(candidate.address, branch)) {
            return rollback_newly_installed("failed to install ARM64 Dart return interception");
        }
        Arm64ReturnPatch installed = candidate;
        installed.patched_instruction = branch;
        installed.consumers = 1;
        newly_installed.push_back(installed);
        acquired_sites.push_back(candidate.address);
    }
    for (Arm64ReturnPatch* patch : existing_consumed) ++patch->consumers;
    payload->return_patches.insert(payload->return_patches.end(), newly_installed.begin(),
                                   newly_installed.end());
    ++payload->return_interception_consumers;
    hook->payload_return_consumer = true;
    hook->payload_return_sites = std::move(acquired_sites);
    return DARTPLANT_OK;
#else
    (void) hook;
    SetLastError("Dart return interception requires ARM64");
    return DARTPLANT_UNSUPPORTED_ABI;
#endif
}

bool RestoreArm64ReturnInterception(DartPlantHook* hook) {
#if defined(__aarch64__)
    if (hook == nullptr) return false;
    if (!hook->payload_return_consumer) return true;
    if (hook->code_target == nullptr || hook->code_target->payload == nullptr) return false;
    auto payload = hook->code_target->payload;
    std::lock_guard payload_lock(payload->mutex);
    if (payload->return_interception_consumers == 0 || hook->payload_return_sites.empty()) {
        return false;
    }

    std::vector<Arm64ReturnPatch> restore_sites;
    for (uintptr_t address : hook->payload_return_sites) {
        const auto patch = std::find_if(
            payload->return_patches.begin(), payload->return_patches.end(),
            [address](const Arm64ReturnPatch& value) { return value.address == address; });
        if (patch == payload->return_patches.end() || patch->consumers == 0) return false;
        uint32_t current = 0;
        std::memcpy(&current, reinterpret_cast<const void*>(patch->address), sizeof(current));
        // Even a shared site must still contain the branch DartPlant owns
        // before this consumer is allowed to relinquish ownership. Otherwise a
        // foreign writer could make the refcount lie and the final consumer
        // would later restore over somebody else's code.
        if (current != patch->patched_instruction) return false;
        if (patch->consumers == 1) restore_sites.push_back(*patch);
    }

    std::vector<Arm64ReturnPatch> restored;
    for (const auto& patch : restore_sites) {
        uint32_t current = 0;
        std::memcpy(&current, reinterpret_cast<const void*>(patch.address), sizeof(current));
        if (current != patch.patched_instruction ||
            !WriteExecutableInstruction(patch.address, patch.original_instruction)) {
            bool rollback_ok = true;
            for (auto it = restored.rbegin(); it != restored.rend(); ++it) {
                if (!WriteExecutableInstruction(it->address, it->patched_instruction)) {
                    rollback_ok = false;
                }
            }
            if (!rollback_ok) {
                SetLastError("failed to roll back partial ARM64 Dart return restoration");
            }
            return false;
        }
        restored.push_back(patch);
    }

    for (uintptr_t address : hook->payload_return_sites) {
        auto patch = std::find_if(
            payload->return_patches.begin(), payload->return_patches.end(),
            [address](const Arm64ReturnPatch& value) { return value.address == address; });
        if (patch == payload->return_patches.end() || patch->consumers == 0) return false;
        --patch->consumers;
    }
    payload->return_patches.erase(
        std::remove_if(payload->return_patches.begin(), payload->return_patches.end(),
                       [](const Arm64ReturnPatch& patch) { return patch.consumers == 0; }),
        payload->return_patches.end());
    --payload->return_interception_consumers;
    hook->payload_return_consumer = false;
    hook->payload_return_sites.clear();
    if (payload->return_interception_consumers == 0) {
        if (!payload->return_patches.empty()) return false;
        // Do not unmap a veneer that has ever been made reachable from live
        // Dart code. An unhooked sibling can fetch the managed branch without
        // participating in HookRecord::in_flight, so there is no local grace
        // period proving that executable page (or its payload cookie) unused.
        DestroyUnpublishedReturnEntry(payload.get());
    }
    return true;
#else
    (void) hook;
    return true;
#endif
}

void DestroyArm64CallbackStub(void* entry, size_t size) {
#if defined(__aarch64__)
    // A callback page may also host a payload-level RET veneer. RET patches
    // can have stale instruction fetches after rollback even when the entry
    // hook itself was never published. Keep all consumed near-pool pages
    // reserved; unused pages stay PROT_NONE and are never reused after issue.
    if (entry != nullptr && size != 0 && !IsPooledNearStubPage(entry)) munmap(entry, size);
#else
    (void) entry;
    (void) size;
#endif
}

}  // namespace dartplant

#if !defined(__aarch64__)
extern "C" uint8_t dartplant_arm64_invoke_original(DartPlantArm64Context*, void*) {
    dartplant::SetLastError("synchronous original invocation requires ARM64");
    return 0;
}
#endif
