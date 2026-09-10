// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ELF_MODULE_IMAGE_H_
#define DARTPLANT_ELF_MODULE_IMAGE_H_

#include <stdint.h>

#include <span>
#include <string>

#include "elf/elf_types.h"

namespace dartplant {

enum class ElfBuildIdStatus : uint8_t {
    kFound = 0,
    kNotFound,
    kAmbiguous,
    kMalformed,
};

struct ElfBuildIdResult {
    ElfBuildIdStatus status = ElfBuildIdStatus::kNotFound;
    std::string build_id;
};

// Parses one bounded PT_NOTE payload. Every note size/alignment operation is
// checked before advancing the cursor, so hostile n_namesz/n_descsz values
// cannot wrap and escape the segment. Multiple notes and inter-note padding are
// supported. Repeated identical GNU build IDs are accepted, distinct values
// are ambiguous, and a malformed note stream fails closed.
ElfBuildIdResult ParseGnuBuildIdNotes(std::span<const uint8_t> notes);

// Merges the result from another bounded PT_NOTE segment. Malformed data
// dominates, then ambiguity; identical found IDs remain a single identity.
ElfBuildIdResult MergeElfBuildIdResults(const ElfBuildIdResult& current,
                                        const ElfBuildIdResult& next);

// Reads GNU build IDs from loaded PT_NOTE ranges. Each note range is bounded
// by p_filesz and must be contained in a readable PT_LOAD before it is
// dereferenced.
ElfBuildIdResult ReadLoadedGnuBuildIdNotes(uintptr_t load_bias,
                                           std::span<const ElfProgramHeaderView> headers);

}  // namespace dartplant

#endif  // DARTPLANT_ELF_MODULE_IMAGE_H_
