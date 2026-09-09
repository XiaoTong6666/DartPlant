// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ELF_DYNAMIC_VIEW_H_
#define DARTPLANT_ELF_DYNAMIC_VIEW_H_

#include <elf.h>
#include <stdint.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "elf/elf_types.h"

namespace dartplant {

enum class ElfDynamicViewStatus : uint8_t {
    kAvailable = 0,
    kUnavailable,
    kMalformed,
};

enum class ElfDynamicLookupStatus : uint8_t {
    kFound = 0,
    kNotFound,
    kUnavailable,
    kMalformed,
};

struct ElfDynamicSymbol {
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t info = 0;
    uint16_t section_index = SHN_UNDEF;
};

struct ElfDynamicLookupResult {
    ElfDynamicLookupStatus status = ElfDynamicLookupStatus::kUnavailable;
    ElfDynamicSymbol symbol{};
};

// A parsed PT_DYNAMIC view. Pointer-valued DT_* entries deliberately remain
// ELF virtual addresses. The backing reader is solely responsible for mapping
// those VAs to file offsets or loaded runtime addresses.
struct ElfDynamicView {
    uint64_t symtab_va = 0;
    uint64_t strtab_va = 0;
    uint64_t strtab_size = 0;
    uint64_t symbol_entry_size = 0;
    std::optional<uint64_t> sysv_hash_va;
    std::optional<uint64_t> gnu_hash_va;
};

struct ElfDynamicViewResult {
    ElfDynamicViewStatus status = ElfDynamicViewStatus::kUnavailable;
    ElfDynamicView view{};
};

// Reads one bounded ELF virtual-address space. Loaded images admit bytes in a
// PT_LOAD's p_memsz range; file images admit only p_filesz and translate using
// p_offset + (va - p_vaddr). This keeps all DT_* addresses in one ELF-VA
// coordinate system while making the file/memory boundary explicit.
class ElfVaReader {
public:
    virtual ~ElfVaReader() = default;
    virtual bool Read(uint64_t va, void* output, size_t size) const = 0;
    virtual std::optional<uint64_t> ReadableBytes(uint64_t va) const = 0;
};

class LoadedElfReader final : public ElfVaReader {
public:
    LoadedElfReader(uintptr_t load_bias, std::span<const ElfProgramHeaderView> headers)
        : load_bias_(load_bias), headers_(headers) {}

    bool Read(uint64_t va, void* output, size_t size) const override;
    std::optional<uint64_t> ReadableBytes(uint64_t va) const override;

private:
    uintptr_t load_bias_ = 0;
    std::span<const ElfProgramHeaderView> headers_;
};

class FileElfReader final : public ElfVaReader {
public:
    FileElfReader(std::span<const uint8_t> bytes, std::span<const ElfProgramHeaderView> headers)
        : bytes_(bytes), headers_(headers) {}

    bool Read(uint64_t va, void* output, size_t size) const override;
    std::optional<uint64_t> ReadableBytes(uint64_t va) const override;

private:
    std::span<const uint8_t> bytes_;
    std::span<const ElfProgramHeaderView> headers_;
};

// Converts a complete file-backed VA range through a PT_LOAD. The range must
// fit p_filesz; bytes that exist only in p_memsz/BSS never resolve to a file
// offset.
std::optional<uint64_t> ElfVaToFileOffset(std::span<const ElfProgramHeaderView> headers,
                                          uint64_t va, size_t size = 1);

// Parses the ELF64/AArch64 program table without requiring section headers.
bool ParseElf64ProgramHeaders(std::span<const uint8_t> bytes,
                              std::vector<ElfProgramHeaderView>* out_headers);

// Returns a unique program header of the requested type. Multiple matching
// entries are deliberately ambiguous and return nullopt.
std::optional<ElfProgramHeaderView> FindElfProgramHeader(
    std::span<const ElfProgramHeaderView> headers, uint32_t type);

// Validates that one VA range is contained in a single PT_LOAD carrying all
// required flags. loaded_image selects p_memsz rather than p_filesz.
bool ElfVaRangeHasFlags(std::span<const ElfProgramHeaderView> headers, uint64_t va, uint64_t size,
                        uint32_t required_flags, bool loaded_image);

// Parses at most dynamic_size bytes and requires DT_NULL within that bound.
// DT_* pointer values are not relocated here. A usable dynamic symbol-table
// view requires SYMTAB/STRTAB/STRSZ/SYMENT; SysV/GNU hash tables are optional
// indexing capabilities. Every advertised table anchor must be readable
// through the supplied backing.
ElfDynamicViewResult ReadElfDynamicView(const ElfVaReader& reader, uint64_t dynamic_va,
                                        uint64_t dynamic_size);

// Exact dynamic-symbol lookup. GNU hash is preferred when both hash styles are
// present, matching Android bionic. GNU hash lookup uses bloom/bucket/chain
// semantics directly and does not invent a SysV-style dynsym count.
ElfDynamicLookupResult FindElfDynamicSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                            std::string_view name);

}  // namespace dartplant

#endif  // DARTPLANT_ELF_DYNAMIC_VIEW_H_
