// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ABI_CALLING_CONVENTION_H_
#define DARTPLANT_ABI_CALLING_CONVENTION_H_

#include <stdint.h>

#include <array>
#include <vector>

#include "abi/call_layout.h"
#include "abi/evidence.h"

namespace dartplant::abi {

struct DartCallingConventionProfile {
    uint32_t word_size = 0;
    uint8_t dart_sp_register = 0;

    std::array<uint8_t, 8> gp_argument_registers{};
    uint8_t gp_argument_register_count = 0;
    std::array<uint8_t, 8> fpu_argument_registers{};
    uint8_t fpu_argument_register_count = 0;

    uint8_t tagged_result_gp_register = 0;
    uint8_t unboxed_int64_result_gp_register = 0;
    uint8_t unboxed_double_result_fpu_register = 0;
    std::array<uint8_t, 2> pair_of_tagged_result_gp_registers{};

    // Exact Location::ToEntrySpRelative() delta for this target/profile.
    // Stack locations are first allocated in FP slots by the compiler and are
    // then normalized to the Dart entry SP captured by DartPlant's trampoline.
    int32_t fp_to_entry_sp_slot_delta = 0;
};

// Dart ARM64 AOT register convention from constants_arm64.h and stack-frame
// conversion constants from stack_frame_arm64.h / locations.cc.
DartCallingConventionProfile Arm64AotCallingConventionProfile();

// Mirrors compiler::ComputeCallingConvention() for the currently supported
// ordinary Dart AOT representations. Unknown/conflicting Function evidence is
// never guessed and fails closed before a typed DartCallFrame can be exposed.
DartCallLayoutStatus ComputeDartCallLayout(const DartFunctionAbiResolution& abi,
                                           const DartCallingConventionProfile& profile,
                                           DartCallLayout* out_layout);

// Convenience wrapper for the only production target currently supported by
// DartPlant. Kept for current internal callers/tests while the generic profile
// becomes the P5/P6 boundary.
bool ComputeArm64AotCallLayout(const DartFunctionAbiResolution& abi, DartCallLayout* out_layout);

}  // namespace dartplant::abi

#endif  // DARTPLANT_ABI_CALLING_CONVENTION_H_
