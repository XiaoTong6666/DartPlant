// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>

#include "core/internal.h"

#if defined(__aarch64__) && !defined(MAP_FIXED_NOREPLACE)
#define MAP_FIXED_NOREPLACE 0x100000
#endif

extern "C" void dartplant_arm64_callback_entry();
extern "C" void dartplant_arm64_native_callback_entry();
extern "C" void dartplant_arm64_return_entry();
extern "C" void dartplant_arm64_dispatch_exception_unwind(uintptr_t target_spreg,
                                                          uintptr_t target_fp);

namespace dartplant {

namespace {

#if defined(__aarch64__)
constexpr uintptr_t kArm64AdrpReach = uintptr_t{1} << 32;
constexpr uintptr_t kArm64BranchReach = uintptr_t{1} << 27;

using JumpToFrameFn = void (*)(uintptr_t program_counter, uintptr_t stack_pointer,
                               uintptr_t frame_pointer, void* thread);

struct ExceptionBridgeState {
    std::mutex mutex;
    uintptr_t target = 0;
    JumpToFrameFn backup = nullptr;
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

void* AllocateCallbackStub(size_t allocation_size, uintptr_t target, uintptr_t required_reach) {
    if (target != 0) {
        const long page_size_value = sysconf(_SC_PAGESIZE);
        if (page_size_value <= 0) return nullptr;
        const uintptr_t page_size = static_cast<uintptr_t>(page_size_value);
        const uintptr_t target_page = target & ~(page_size - 1);

        // A patched RET has only B +/-128 MiB of reach. mmap(address) is merely
        // a hint and Android may return a distant page, so for this stricter
        // case search exact free pages with MAP_FIXED_NOREPLACE. This never
        // replaces an existing Dart/Flutter mapping.
        if (required_reach != 0 && required_reach <= kArm64BranchReach) {
            // A direct B can reach any instruction within +/-128 MiB, but the
            // free mapping need not share a particular 1-MiB residue with the
            // Dart target. Walk at the real page granularity so every possible
            // page-aligned hole in range is considered. MAP_FIXED_NOREPLACE is
            // essential here: it may claim only an actually free page and can
            // never replace Dart/Flutter code or data.
            const uintptr_t near_step = page_size;
            const uint32_t attempts =
                static_cast<uint32_t>((required_reach - page_size) / near_step);
            for (uint32_t attempt = 1; attempt <= attempts; ++attempt) {
                const uintptr_t delta = near_step * attempt;
                const uintptr_t candidates[2] = {
                    target_page <= UINTPTR_MAX - delta ? target_page + delta : 0,
                    target_page >= delta ? target_page - delta : 0,
                };
                for (uintptr_t candidate : candidates) {
                    if (candidate == 0) continue;
                    void* mapped = mmap(reinterpret_cast<void*>(candidate), allocation_size,
                                        PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                    if (mapped == MAP_FAILED) continue;
                    if (Distance(reinterpret_cast<uintptr_t>(mapped), target) < required_reach) {
                        return mapped;
                    }
                    munmap(mapped, allocation_size);
                }
            }
            return nullptr;
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

bool IsDartReturn(uint32_t instruction) { return instruction == 0xd65f03c0U; }

int64_t SignExtend(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

bool BranchTargetOutsideCode(uintptr_t pc, uintptr_t start, uintptr_t end, uint32_t instruction) {
    int64_t delta = 0;
    if ((instruction & 0xfc000000U) == 0x14000000U) {  // B imm26 (not BL).
        delta = SignExtend(instruction & 0x03ffffffU, 26) << 2;
    } else if ((instruction & 0xff000010U) == 0x54000000U) {  // B.cond imm19.
        delta = SignExtend((instruction >> 5) & 0x7ffffU, 19) << 2;
    } else if ((instruction & 0x7e000000U) == 0x34000000U) {  // CBZ/CBNZ imm19.
        delta = SignExtend((instruction >> 5) & 0x7ffffU, 19) << 2;
    } else if ((instruction & 0x7e000000U) == 0x36000000U) {  // TBZ/TBNZ imm14.
        delta = SignExtend((instruction >> 5) & 0x3fffU, 14) << 2;
    } else {
        return false;
    }
    const uintptr_t target = static_cast<uintptr_t>(static_cast<int64_t>(pc) + delta);
    return target < start || target >= end;
}

bool IsIndirectBranch(uint32_t instruction) {
    return (instruction & 0xfffffc1fU) == 0xd61f0000U;  // BR Xn.
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
    std::atomic_ref<uint32_t>(*slot).store(instruction, std::memory_order_release);
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

bool EnsureArm64ExceptionBridge(DartPlantHook* hook, const DartPlantArm64Context& context) {
#if defined(__aarch64__)
    if (hook == nullptr || hook->method_storage == nullptr ||
        hook->method_storage->function == nullptr ||
        hook->method_storage->function->source == DartFunctionSource::kSynthetic) {
        return true;
    }
    const uint32_t offset = hook->method_storage->function->thread_jump_to_frame_entry_point_offset;
    const uintptr_t thread = static_cast<uintptr_t>(context.x[26]);
    uintptr_t target = 0;
    if (offset == 0 || thread == 0 || !ReadSelfWord(thread + offset, &target) || target == 0 ||
        !IsKnownExecutableAddress(target)) {
        SetLastError("failed to resolve Dart JumpToFrame for exception-safe callbacks");
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
        return state.backup != nullptr;
    }
    const HostApiBinding* binding = hook->host_binding;
    if (binding == nullptr || binding->hook == nullptr || binding->unhook == nullptr) {
        SetLastError("exception bridge has no installing host backend");
        return false;
    }
    void* backup = nullptr;
    if (binding->hook(binding->user_data, reinterpret_cast<void*>(target),
                      reinterpret_cast<void*>(&dartplant_arm64_jump_to_frame_hook), &backup) != 0 ||
        backup == nullptr) {
        SetLastError("failed to hook Dart JumpToFrame for exception cleanup");
        return false;
    }
    state.target = target;
    state.backup = reinterpret_cast<JumpToFrameFn>(backup);
    g_jump_to_frame_backup.store(state.backup, std::memory_order_release);
    return true;
#else
    (void) hook;
    (void) context;
    return false;
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

    // Real Dart returns are intercepted before RET so the original LR remains
    // the actual Dart caller PC for exception unwinding. The RET replacement
    // branches here; this veneer reloads Hook* after the original body has
    // freely used x16/x17 and enters the common return dispatcher.
    if (!synthetic_native) {
        auto* return_code = code + 16;  // +64 bytes.
        return_code[0] = 0x58000091;    // ldr x17, +16 (Hook* at +80).
        return_code[1] = 0x580000b0;    // ldr x16, +20 (common at +88).
        return_code[2] = 0xd61f0200;    // br x16.
        return_code[3] = 0xd503201f;    // nop.
        std::memcpy(reinterpret_cast<uint8_t*>(code) + 80, &hook, sizeof(hook));
        void* return_common = reinterpret_cast<void*>(&dartplant_arm64_return_entry);
        std::memcpy(reinterpret_cast<uint8_t*>(code) + 88, &return_common, sizeof(return_common));
        hook->replacement_return_entry = return_code;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code) + 96);
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

DartPlantStatus InstallArm64ReturnInterception(DartPlantHook* hook) {
#if defined(__aarch64__)
    if (hook == nullptr || hook->code_target == nullptr ||
        hook->replacement_return_entry == nullptr || hook->code_target->entry == 0 ||
        hook->code_target->code_size < sizeof(uint32_t)) {
        SetLastError("Dart callback hook has no exact code range for exception-safe returns");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    const uintptr_t start = hook->code_target->entry;
    const uintptr_t end = start + hook->code_target->code_size;
    if (end <= start || (start & 3U) != 0) {
        SetLastError("Dart callback code range is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    std::vector<Arm64ReturnPatch> candidates;
    for (uintptr_t pc = start; pc + sizeof(uint32_t) <= end; pc += sizeof(uint32_t)) {
        uint32_t instruction = 0;
        std::memcpy(&instruction, reinterpret_cast<const void*>(pc), sizeof(instruction));
        if (IsDartReturn(instruction)) {
            candidates.push_back({pc, instruction});
            continue;
        }
        if (IsIndirectBranch(instruction) || BranchTargetOutsideCode(pc, start, end, instruction)) {
            SetLastError(
                "Dart callback Code has an unsupported non-RET/tail exit; leave interception would be incomplete");
            return DARTPLANT_UNSUPPORTED_ABI;
        }
    }
    if (candidates.empty()) {
        SetLastError("Dart callback Code has no interceptable ARM64 RET");
        return DARTPLANT_UNSUPPORTED_ABI;
    }

    const uintptr_t return_entry = reinterpret_cast<uintptr_t>(hook->replacement_return_entry);
    hook->return_patches.clear();
    for (const auto& candidate : candidates) {
        uint32_t branch = 0;
        if (!EncodeDirectBranch(candidate.address, return_entry, &branch) ||
            !WriteExecutableInstruction(candidate.address, branch)) {
            for (auto it = hook->return_patches.rbegin(); it != hook->return_patches.rend(); ++it) {
                (void) WriteExecutableInstruction(it->address, it->original_instruction);
            }
            hook->return_patches.clear();
            SetLastError("failed to install ARM64 Dart return interception");
            return DARTPLANT_HOOK_FAILED;
        }
        hook->return_patches.push_back(candidate);
    }
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
    bool ok = true;
    for (auto it = hook->return_patches.rbegin(); it != hook->return_patches.rend(); ++it) {
        if (!WriteExecutableInstruction(it->address, it->original_instruction)) ok = false;
    }
    if (ok) hook->return_patches.clear();
    return ok;
#else
    (void) hook;
    return true;
#endif
}

void DestroyArm64CallbackStub(void* entry, size_t size) {
#if defined(__aarch64__)
    if (entry != nullptr && size != 0) munmap(entry, size);
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
