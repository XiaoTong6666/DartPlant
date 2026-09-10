// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ELF_ELF_TYPES_H_
#define DARTPLANT_ELF_ELF_TYPES_H_

#include <stdint.h>

namespace dartplant {

// Program-header fields that are relevant to DartPlant's ELF mapping layer.
// Values remain ELF file/virtual-address facts here; runtime relocation is a
// property of a loaded-image reader, not of this record.
struct ElfProgramHeaderView {
    uint32_t type = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t virtual_address = 0;
    uint64_t file_size = 0;
    uint64_t memory_size = 0;
    uint64_t alignment = 0;
};

}  // namespace dartplant

#endif  // DARTPLANT_ELF_ELF_TYPES_H_
