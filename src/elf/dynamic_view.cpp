// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "elf/dynamic_view.h"

#include <elf.h>
#include <string.h>

#include <algorithm>
#include <array>

namespace dartplant {
namespace {

template <typename T>
bool RangeFits(T start, T size, T limit) {
    return start <= limit && size <= limit - start;
}

bool CheckedAddVa(uint64_t base, uint64_t offset, uint64_t* output) {
    if (output == nullptr || base > UINT64_MAX - offset) return false;
    *output = base + offset;
    return true;
}

bool CheckedMul(uint64_t left, uint64_t right, uint64_t* output) {
    if (output == nullptr || (left != 0 && right > UINT64_MAX / left)) return false;
    *output = left * right;
    return true;
}

bool CheckedMulAddVa(uint64_t base, uint64_t index, uint64_t stride, uint64_t* output) {
    uint64_t offset = 0;
    return CheckedMul(index, stride, &offset) && CheckedAddVa(base, offset, output);
}

bool IsValidLoadAlignment(uint64_t alignment, uint64_t offset, uint64_t virtual_address) {
    if (alignment <= 1) return true;
    return (alignment & (alignment - 1)) == 0 && virtual_address % alignment == offset % alignment;
}

bool DynamicAnchorReadable(const ElfVaReader& reader, uint64_t va, uint64_t minimum_size) {
    if (va == 0) return false;
    const auto remaining = reader.ReadableBytes(va);
    return remaining.has_value() && minimum_size <= *remaining;
}

template <typename T>
bool SetDynamicSingleton(T value, std::optional<T>* slot) {
    if (slot == nullptr) return false;
    if (slot->has_value() && *slot != value) return false;
    *slot = value;
    return true;
}

template <typename T>
bool ReadValue(const ElfVaReader& reader, uint64_t va, T* output) {
    return output != nullptr && reader.Read(va, output, sizeof(T));
}

uint32_t SysvHash(std::string_view name) {
    uint32_t hash = 0;
    for (const unsigned char value : name) {
        hash = (hash << 4) + value;
        const uint32_t high = hash & 0xf0000000U;
        if (high != 0) hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

uint32_t GnuHash(std::string_view name) {
    uint32_t hash = 5381;
    for (const unsigned char value : name) hash = hash * 33U + value;
    return hash;
}

bool ReadSymbolName(const ElfVaReader& reader, const ElfDynamicView& view, const Elf64_Sym& symbol,
                    std::string* output) {
    if (output == nullptr || symbol.st_name >= view.strtab_size) {
        return false;
    }
    uint64_t start = 0;
    if (!CheckedAddVa(view.strtab_va, symbol.st_name, &start)) return false;
    const uint64_t remaining = view.strtab_size - symbol.st_name;
    output->clear();
    output->reserve(static_cast<size_t>(std::min<uint64_t>(remaining, 64)));
    for (uint64_t index = 0; index < remaining; ++index) {
        char value = '\0';
        uint64_t entry_va = 0;
        if (!CheckedAddVa(start, index, &entry_va) || !reader.Read(entry_va, &value, 1)) {
            return false;
        }
        if (value == '\0') return true;
        output->push_back(value);
    }
    return false;
}

ElfDynamicLookupResult MatchSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                   uint64_t index, std::string_view wanted) {
    if (view.symbol_entry_size != sizeof(Elf64_Sym)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    uint64_t symbol_va = 0;
    if (!CheckedMulAddVa(view.symtab_va, index, view.symbol_entry_size, &symbol_va)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    Elf64_Sym symbol{};
    if (!ReadValue(reader, symbol_va, &symbol)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    std::string actual;
    if (!ReadSymbolName(reader, view, symbol, &actual)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    if (actual != wanted) return {.status = ElfDynamicLookupStatus::kNotFound};
    if (symbol.st_shndx == SHN_UNDEF || symbol.st_value == 0) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    return {
        .status = ElfDynamicLookupStatus::kFound,
        .symbol =
            {
                .value = symbol.st_value,
                .size = symbol.st_size,
                .info = symbol.st_info,
                .other = symbol.st_other,
                .section_index = symbol.st_shndx,
            },
    };
}

bool SameDynamicSymbol(const ElfDynamicSymbol& left, const ElfDynamicSymbol& right) {
    return left.value == right.value && left.size == right.size && left.info == right.info &&
           left.other == right.other && left.section_index == right.section_index;
}

bool RememberDynamicSymbol(const ElfDynamicLookupResult& match,
                           std::optional<ElfDynamicSymbol>* candidate) {
    if (candidate == nullptr || match.status != ElfDynamicLookupStatus::kFound) return false;
    if (candidate->has_value() && !SameDynamicSymbol(**candidate, match.symbol)) return false;
    *candidate = match.symbol;
    return true;
}

ElfDynamicLookupResult FindSysvSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                      std::string_view name) {
    if (!view.sysv_hash_va.has_value()) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    std::array<uint32_t, 2> header{};
    if (!reader.Read(*view.sysv_hash_va, header.data(), sizeof(header)) || header[0] == 0) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const uint32_t bucket_count = header[0];
    const uint32_t chain_count = header[1];
    if (chain_count == 0 || view.symbol_entry_size != sizeof(Elf64_Sym)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    uint64_t buckets_va = 0;
    uint64_t bucket_va = 0;
    uint64_t buckets_size = 0;
    uint64_t chains_size = 0;
    uint64_t hash_table_size = 0;
    uint64_t dynsym_size = 0;
    if (!CheckedAddVa(*view.sysv_hash_va, sizeof(header), &buckets_va) ||
        !CheckedMul(bucket_count, sizeof(uint32_t), &buckets_size) ||
        !CheckedMul(chain_count, sizeof(uint32_t), &chains_size) ||
        !CheckedAddVa(sizeof(header), buckets_size, &hash_table_size) ||
        !CheckedAddVa(hash_table_size, chains_size, &hash_table_size) ||
        !CheckedMul(chain_count, view.symbol_entry_size, &dynsym_size) ||
        !DynamicAnchorReadable(reader, *view.sysv_hash_va, hash_table_size) ||
        !DynamicAnchorReadable(reader, view.symtab_va, dynsym_size) ||
        !CheckedMulAddVa(buckets_va, SysvHash(name) % bucket_count, sizeof(uint32_t), &bucket_va)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    uint32_t symbol_index = 0;
    if (!ReadValue(reader, bucket_va, &symbol_index)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    uint64_t chains_va = 0;
    if (!CheckedAddVa(buckets_va, buckets_size, &chains_va)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    std::optional<ElfDynamicSymbol> candidate;
    for (uint64_t visited = 0; symbol_index != STN_UNDEF; ++visited) {
        if (symbol_index >= chain_count || visited >= chain_count) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        const auto match = MatchSymbol(reader, view, symbol_index, name);
        if (match.status == ElfDynamicLookupStatus::kMalformed) return match;
        if (match.status == ElfDynamicLookupStatus::kFound &&
            !RememberDynamicSymbol(match, &candidate)) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        uint32_t next = 0;
        uint64_t chain_va = 0;
        if (!CheckedMulAddVa(chains_va, symbol_index, sizeof(uint32_t), &chain_va) ||
            !ReadValue(reader, chain_va, &next)) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        if (next == symbol_index) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        symbol_index = next;
    }
    return candidate.has_value()
               ? ElfDynamicLookupResult{
                     .status = ElfDynamicLookupStatus::kFound,
                     .symbol = *candidate,
                 }
               : ElfDynamicLookupResult{.status = ElfDynamicLookupStatus::kNotFound};
}

ElfDynamicLookupResult FindGnuSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                     std::string_view name) {
    if (!view.gnu_hash_va.has_value()) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    std::array<uint32_t, 4> header{};
    if (!reader.Read(*view.gnu_hash_va, header.data(), sizeof(header))) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const uint32_t bucket_count = header[0];
    const uint32_t symbol_offset = header[1];
    const uint32_t bloom_words = header[2];
    const uint32_t bloom_shift = header[3];
    // GNU hash values are 32-bit even for ELFCLASS64. The GNU hash Bloom filter computes its
    // second bit from (hash >> shift2), and Android's linker performs that shift on a uint32_t
    // hash. Reject shift counts outside the 32-bit hash domain before evaluating the shift.
    if (bucket_count == 0 || bloom_words == 0 || bloom_shift >= 32 ||
        (bloom_words & (bloom_words - 1)) != 0) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }

    uint64_t bloom_base = 0;
    uint64_t bloom_size = 0;
    uint64_t buckets_va = 0;
    uint64_t buckets_size = 0;
    uint64_t fixed_table_size = 0;
    if (!CheckedAddVa(*view.gnu_hash_va, sizeof(header), &bloom_base) ||
        !CheckedMul(bloom_words, sizeof(Elf64_Addr), &bloom_size) ||
        !CheckedAddVa(bloom_base, bloom_size, &buckets_va) ||
        !CheckedMul(bucket_count, sizeof(uint32_t), &buckets_size) ||
        !CheckedAddVa(sizeof(header), bloom_size, &fixed_table_size) ||
        !CheckedAddVa(fixed_table_size, buckets_size, &fixed_table_size) ||
        !DynamicAnchorReadable(reader, *view.gnu_hash_va, fixed_table_size)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }

    const uint32_t hash = GnuHash(name);
    constexpr uint32_t kWordBits = sizeof(Elf64_Addr) * 8;
    const uint64_t bloom_index = (hash / kWordBits) & (bloom_words - 1);
    Elf64_Addr bloom = 0;
    uint64_t bloom_va = 0;
    if (!CheckedMulAddVa(bloom_base, bloom_index, sizeof(Elf64_Addr), &bloom_va) ||
        !ReadValue(reader, bloom_va, &bloom)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const Elf64_Addr mask = (Elf64_Addr{1} << (hash % kWordBits)) |
                            (Elf64_Addr{1} << ((hash >> bloom_shift) % kWordBits));
    if ((bloom & mask) != mask) return {.status = ElfDynamicLookupStatus::kNotFound};

    uint64_t bucket_va = 0;
    if (!CheckedMulAddVa(buckets_va, hash % bucket_count, sizeof(uint32_t), &bucket_va)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    uint32_t symbol_index = 0;
    if (!ReadValue(reader, bucket_va, &symbol_index)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    if (symbol_index == 0) return {.status = ElfDynamicLookupStatus::kNotFound};
    if (symbol_index < symbol_offset) return {.status = ElfDynamicLookupStatus::kMalformed};

    uint64_t chains_va = 0;
    if (!CheckedAddVa(buckets_va, buckets_size, &chains_va)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const auto remaining = reader.ReadableBytes(chains_va);
    if (!remaining.has_value()) return {.status = ElfDynamicLookupStatus::kMalformed};
    const uint64_t max_chain_entries = *remaining / sizeof(uint32_t);
    uint64_t chain_index = symbol_index - symbol_offset;
    std::optional<ElfDynamicSymbol> candidate;
    for (; chain_index < max_chain_entries; ++chain_index) {
        uint32_t chain_hash = 0;
        uint64_t chain_va = 0;
        if (!CheckedMulAddVa(chains_va, chain_index, sizeof(uint32_t), &chain_va) ||
            !ReadValue(reader, chain_va, &chain_hash)) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        if ((chain_hash | 1U) == (hash | 1U)) {
            const auto match = MatchSymbol(reader, view, symbol_index, name);
            if (match.status == ElfDynamicLookupStatus::kMalformed) return match;
            if (match.status == ElfDynamicLookupStatus::kFound &&
                !RememberDynamicSymbol(match, &candidate)) {
                return {.status = ElfDynamicLookupStatus::kMalformed};
            }
        }
        if ((chain_hash & 1U) != 0) {
            return candidate.has_value()
                       ? ElfDynamicLookupResult{
                             .status = ElfDynamicLookupStatus::kFound,
                             .symbol = *candidate,
                         }
                       : ElfDynamicLookupResult{.status = ElfDynamicLookupStatus::kNotFound};
        }
        if (symbol_index == UINT32_MAX) return {.status = ElfDynamicLookupStatus::kMalformed};
        ++symbol_index;
    }
    return {.status = ElfDynamicLookupStatus::kMalformed};
}

}  // namespace

bool ValidateElfLoadLayout(std::span<const ElfProgramHeaderView> headers) {
    bool saw_load = false;
    uint64_t previous_load_va = 0;
    uint64_t previous_load_end = 0;
    for (const auto& header : headers) {
        if (header.type != PT_LOAD) continue;
        uint64_t load_end = 0;
        if (header.file_size > header.memory_size ||
            !CheckedAddVa(header.virtual_address, header.memory_size, &load_end) ||
            !IsValidLoadAlignment(header.alignment, header.offset, header.virtual_address) ||
            (saw_load && (header.virtual_address < previous_load_va ||
                          header.virtual_address < previous_load_end))) {
            return false;
        }
        saw_load = true;
        previous_load_va = header.virtual_address;
        previous_load_end = load_end;
    }
    return true;
}

bool ValidateElfLoadFileBounds(std::span<const uint8_t> bytes,
                               std::span<const ElfProgramHeaderView> headers) {
    for (const auto& header : headers) {
        if (header.type == PT_LOAD &&
            !RangeFits(header.offset, header.file_size, static_cast<uint64_t>(bytes.size()))) {
            return false;
        }
    }
    return true;
}

bool ValidateElfLoadSegments(std::span<const uint8_t> bytes,
                             std::span<const ElfProgramHeaderView> headers) {
    return ValidateElfLoadLayout(headers) && ValidateElfLoadFileBounds(bytes, headers);
}

std::optional<uint64_t> ElfVaToFileOffset(std::span<const ElfProgramHeaderView> headers,
                                          uint64_t va, size_t size) {
    if (size == 0) return std::nullopt;
    const uint64_t width = static_cast<uint64_t>(size);
    std::optional<uint64_t> result;
    for (const auto& header : headers) {
        if (header.type != PT_LOAD || va < header.virtual_address) continue;
        const uint64_t delta = va - header.virtual_address;
        if (delta >= header.file_size || width > header.file_size - delta ||
            header.offset > UINT64_MAX - delta) {
            continue;
        }
        const uint64_t candidate = header.offset + delta;
        if (result.has_value() && *result != candidate) return std::nullopt;
        result = candidate;
    }
    return result;
}

bool ElfProgramHeaderHasCanonicalFileBacking(std::span<const ElfProgramHeaderView> headers,
                                             const ElfProgramHeaderView& header,
                                             uint32_t required_flags) {
    if (header.file_size == 0 || header.file_size > header.memory_size ||
        header.file_size > SIZE_MAX) {
        return false;
    }
    const auto offset =
        ElfVaToFileOffset(headers, header.virtual_address, static_cast<size_t>(header.file_size));
    return offset.has_value() && *offset == header.offset &&
           ElfVaRangeHasFlags(headers, header.virtual_address, header.file_size, required_flags,
                              false);
}

std::optional<uint64_t> LoadedElfReader::ReadableBytes(uint64_t va) const {
    for (const auto& header : headers_) {
        if (header.type != PT_LOAD || (header.flags & PF_R) == 0 || va < header.virtual_address) {
            continue;
        }
        const uint64_t delta = va - header.virtual_address;
        if (delta < header.memory_size) return header.memory_size - delta;
    }
    return std::nullopt;
}

bool LoadedElfReader::Read(uint64_t va, void* output, size_t size) const {
    if (output == nullptr || size == 0) return false;
    const auto remaining = ReadableBytes(va);
    if (!remaining.has_value() || static_cast<uint64_t>(size) > *remaining ||
        va > UINTPTR_MAX - load_bias_) {
        return false;
    }
    const uintptr_t runtime = load_bias_ + static_cast<uintptr_t>(va);
    if (size > UINTPTR_MAX - runtime) return false;
    memcpy(output, reinterpret_cast<const void*>(runtime), size);
    return true;
}

std::optional<uint64_t> FileBackedLoadedElfReader::ReadableBytes(uint64_t va) const {
    std::optional<uint64_t> remaining;
    std::optional<uint64_t> file_offset;
    for (const auto& header : headers_) {
        if (header.type != PT_LOAD || va < header.virtual_address) continue;
        const uint64_t delta = va - header.virtual_address;
        if (delta >= header.file_size || header.offset > UINT64_MAX - delta) continue;
        const uint64_t candidate_offset = header.offset + delta;
        if ((header.flags & PF_R) == 0 ||
            (file_offset.has_value() && *file_offset != candidate_offset)) {
            return std::nullopt;
        }
        file_offset = candidate_offset;
        const uint64_t candidate_remaining = header.file_size - delta;
        remaining =
            remaining.has_value() ? std::min(*remaining, candidate_remaining) : candidate_remaining;
    }
    return remaining;
}

bool FileBackedLoadedElfReader::Read(uint64_t va, void* output, size_t size) const {
    if (output == nullptr || size == 0) return false;
    const auto remaining = ReadableBytes(va);
    const auto offset = ElfVaToFileOffset(headers_, va, size);
    if (!remaining.has_value() || !offset.has_value() || static_cast<uint64_t>(size) > *remaining ||
        va > UINTPTR_MAX - load_bias_) {
        return false;
    }
    const uintptr_t runtime = load_bias_ + static_cast<uintptr_t>(va);
    if (size > UINTPTR_MAX - runtime) return false;
    memcpy(output, reinterpret_cast<const void*>(runtime), size);
    return true;
}

std::optional<uint64_t> FileElfReader::ReadableBytes(uint64_t va) const {
    for (const auto& header : headers_) {
        if (header.type != PT_LOAD || va < header.virtual_address) continue;
        const uint64_t delta = va - header.virtual_address;
        if (delta >= header.file_size || header.offset > bytes_.size()) continue;
        const uint64_t file_available = bytes_.size() - static_cast<size_t>(header.offset);
        if (delta >= file_available) continue;
        return std::min(header.file_size - delta, file_available - delta);
    }
    return std::nullopt;
}

bool FileElfReader::Read(uint64_t va, void* output, size_t size) const {
    if (output == nullptr || size == 0) return false;
    const auto offset = ElfVaToFileOffset(headers_, va, size);
    if (!offset.has_value() || *offset > bytes_.size() ||
        static_cast<uint64_t>(size) > bytes_.size() - static_cast<size_t>(*offset)) {
        return false;
    }
    memcpy(output, bytes_.data() + static_cast<size_t>(*offset), size);
    return true;
}

bool ParseElf64ProgramHeaders(std::span<const uint8_t> bytes,
                              std::vector<ElfProgramHeaderView>* out_headers) {
    if (out_headers == nullptr || bytes.size() < sizeof(Elf64_Ehdr)) return false;
    Elf64_Ehdr header{};
    memcpy(&header, bytes.data(), sizeof(header));
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT || header.e_type != ET_DYN ||
        header.e_machine != EM_AARCH64 || header.e_phentsize != sizeof(Elf64_Phdr) ||
        header.e_phnum == PN_XNUM) {
        return false;
    }
    const uint64_t table_size = uint64_t{header.e_phnum} * sizeof(Elf64_Phdr);
    if (!RangeFits<uint64_t>(header.e_phoff, table_size, bytes.size())) return false;

    std::vector<ElfProgramHeaderView> parsed;
    parsed.reserve(header.e_phnum);
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr phdr{};
        uint64_t offset = 0;
        if (!CheckedMulAddVa(header.e_phoff, index, sizeof(Elf64_Phdr), &offset)) return false;
        memcpy(&phdr, bytes.data() + static_cast<size_t>(offset), sizeof(phdr));
        parsed.push_back({
            .type = phdr.p_type,
            .flags = phdr.p_flags,
            .offset = phdr.p_offset,
            .virtual_address = phdr.p_vaddr,
            .file_size = phdr.p_filesz,
            .memory_size = phdr.p_memsz,
            .alignment = phdr.p_align,
        });
    }
    if (!ValidateElfLoadSegments(bytes, parsed)) return false;
    *out_headers = std::move(parsed);
    return true;
}

ElfProgramHeaderLookupResult FindElfProgramHeader(std::span<const ElfProgramHeaderView> headers,
                                                  uint32_t type) {
    ElfProgramHeaderLookupResult result{};
    bool found = false;
    for (const auto& header : headers) {
        if (header.type != type) continue;
        if (found) {
            return {.status = ElfProgramHeaderLookupStatus::kAmbiguous};
        }
        result.status = ElfProgramHeaderLookupStatus::kFound;
        result.header = header;
        found = true;
    }
    return result;
}

bool ElfVaRangeHasFlags(std::span<const ElfProgramHeaderView> headers, uint64_t va, uint64_t size,
                        uint32_t required_flags, bool loaded_image) {
    if (size == 0) return false;
    for (const auto& header : headers) {
        if (header.type != PT_LOAD || (header.flags & required_flags) != required_flags ||
            va < header.virtual_address) {
            continue;
        }
        const uint64_t available = loaded_image ? header.memory_size : header.file_size;
        const uint64_t delta = va - header.virtual_address;
        if (delta < available && size <= available - delta) return true;
    }
    return false;
}

ElfDynamicViewResult ReadElfDynamicView(const ElfVaReader& reader, uint64_t dynamic_va,
                                        uint64_t dynamic_size) {
    if (dynamic_va == 0 || dynamic_size < sizeof(Elf64_Dyn)) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }
    if ((dynamic_size % sizeof(Elf64_Dyn)) != 0) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }
    const auto dynamic_backing = reader.ReadableBytes(dynamic_va);
    if (!dynamic_backing.has_value() || dynamic_size > *dynamic_backing) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }

    std::optional<uint64_t> symtab_va;
    std::optional<uint64_t> strtab_va;
    std::optional<uint64_t> strtab_size;
    std::optional<uint64_t> symbol_entry_size;
    std::optional<uint64_t> sysv_hash_va;
    std::optional<uint64_t> gnu_hash_va;
    bool saw_null = false;
    const uint64_t entry_count = dynamic_size / sizeof(Elf64_Dyn);
    for (uint64_t index = 0; index < entry_count; ++index) {
        Elf64_Dyn entry{};
        uint64_t entry_va = 0;
        if (!CheckedMulAddVa(dynamic_va, index, sizeof(Elf64_Dyn), &entry_va) ||
            !reader.Read(entry_va, &entry, sizeof(entry))) {
            return {.status = ElfDynamicViewStatus::kMalformed};
        }
        if (entry.d_tag == DT_NULL) {
            saw_null = true;
            break;
        }
        switch (entry.d_tag) {
        case DT_SYMTAB:
            if (!SetDynamicSingleton(static_cast<uint64_t>(entry.d_un.d_ptr), &symtab_va)) {
                return {.status = ElfDynamicViewStatus::kMalformed};
            }
            break;
        case DT_STRTAB:
            if (!SetDynamicSingleton(static_cast<uint64_t>(entry.d_un.d_ptr), &strtab_va)) {
                return {.status = ElfDynamicViewStatus::kMalformed};
            }
            break;
        case DT_STRSZ:
            if (!SetDynamicSingleton(static_cast<uint64_t>(entry.d_un.d_val), &strtab_size)) {
                return {.status = ElfDynamicViewStatus::kMalformed};
            }
            break;
        case DT_SYMENT:
            if (!SetDynamicSingleton(static_cast<uint64_t>(entry.d_un.d_val), &symbol_entry_size)) {
                return {.status = ElfDynamicViewStatus::kMalformed};
            }
            break;
        case DT_HASH:
            if (!SetDynamicSingleton(static_cast<uint64_t>(entry.d_un.d_ptr), &sysv_hash_va)) {
                return {.status = ElfDynamicViewStatus::kMalformed};
            }
            break;
        case DT_GNU_HASH:
            if (!SetDynamicSingleton(static_cast<uint64_t>(entry.d_un.d_ptr), &gnu_hash_va)) {
                return {.status = ElfDynamicViewStatus::kMalformed};
            }
            break;
        default:
            break;
        }
    }
    if (!saw_null) return {.status = ElfDynamicViewStatus::kMalformed};
    ElfDynamicView view{
        .symtab_va = symtab_va.value_or(0),
        .strtab_va = strtab_va.value_or(0),
        .strtab_size = strtab_size.value_or(0),
        .symbol_entry_size = symbol_entry_size.value_or(0),
        .sysv_hash_va = sysv_hash_va,
        .gnu_hash_va = gnu_hash_va,
    };
    if (view.symbol_entry_size != 0 && view.symbol_entry_size != sizeof(Elf64_Sym)) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }
    if (view.symtab_va == 0 || view.strtab_va == 0 || view.strtab_size == 0 ||
        view.symbol_entry_size == 0) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }
    if (!DynamicAnchorReadable(reader, view.symtab_va, sizeof(Elf64_Sym)) ||
        !DynamicAnchorReadable(reader, view.strtab_va, view.strtab_size) ||
        (view.sysv_hash_va.has_value() &&
         !DynamicAnchorReadable(reader, *view.sysv_hash_va, sizeof(uint32_t) * 2)) ||
        (view.gnu_hash_va.has_value() &&
         !DynamicAnchorReadable(reader, *view.gnu_hash_va, sizeof(uint32_t) * 4))) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }
    return {.status = ElfDynamicViewStatus::kAvailable, .view = view};
}

ElfDynamicLookupResult FindElfDynamicSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                            std::string_view name) {
    if (name.empty()) return {.status = ElfDynamicLookupStatus::kMalformed};
    if (view.gnu_hash_va.has_value()) return FindGnuSymbol(reader, view, name);
    if (view.sysv_hash_va.has_value()) return FindSysvSymbol(reader, view, name);
    return {.status = ElfDynamicLookupStatus::kMalformed};
}

}  // namespace dartplant
