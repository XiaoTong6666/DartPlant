// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "core/internal.h"

extern "C" void dartplant_arm64_callback_entry();

namespace dartplant {

void* CreateArm64CallbackStub(DartPlantHook* hook, size_t* out_size) {
#if defined(__aarch64__)
    if (hook == nullptr || out_size == nullptr) return nullptr;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return nullptr;
    const size_t allocation_size = static_cast<size_t>(page_size);
    auto* code = static_cast<uint32_t*>(
        mmap(nullptr, allocation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return nullptr;

    // x16/x17 are AArch64 IP0/IP1 scratch registers. The veneer places the hook
    // in x17 and tail-branches to the common entry without changing x30.
    code[0] = 0x58000091;
    // Instruction is at +4; a 20-byte literal offset lands at the common entry
    // literal at +24.
    code[1] = 0x580000b0;
    code[2] = 0xd61f0200;
    code[3] = 0xd503201f;
    std::memcpy(reinterpret_cast<uint8_t*>(code) + 16, &hook, sizeof(hook));
    void* common = reinterpret_cast<void*>(&dartplant_arm64_callback_entry);
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
    (void) out_size;
    return nullptr;
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
extern "C" uint64_t dartplant_arm64_invoke_original(DartPlantArm64Context*, void*) {
    dartplant::SetLastError("synchronous original invocation requires ARM64");
    return 0;
}
#endif
