// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "elf/dynamic_view.h"

#include <elf.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <limits>

namespace dartplant {
namespace {

template <typename T>
bool RangeFits(T start, T size, T limit) {
    return start <= limit && size <= limit - start;
}

bool DynamicAnchorReadable(const ElfVaReader& reader, uint64_t va, uint64_t minimum_size) {
    if (va == 0) return false;
    const auto remaining = reader.ReadableBytes(va);
    return remaining.has_value() && minimum_size <= *remaining;
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
    if (output == nullptr || symbol.st_name >= view.strtab_size ||
        view.strtab_va > UINT64_MAX - symbol.st_name) {
        return false;
    }
    const uint64_t start = view.strtab_va + symbol.st_name;
    const uint64_t remaining = view.strtab_size - symbol.st_name;
    output->clear();
    output->reserve(static_cast<size_t>(std::min<uint64_t>(remaining, 64)));
    for (uint64_t index = 0; index < remaining; ++index) {
        char value = '\0';
        if (!reader.Read(start + index, &value, 1)) return false;
        if (value == '\0') return true;
        output->push_back(value);
    }
    return false;
}

ElfDynamicLookupResult MatchSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                   uint64_t index, std::string_view wanted) {
    if (view.symbol_entry_size != sizeof(Elf64_Sym) ||
        index > (UINT64_MAX - view.symtab_va) / view.symbol_entry_size) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    Elf64_Sym symbol{};
    if (!ReadValue(reader, view.symtab_va + index * view.symbol_entry_size, &symbol)) {
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
                .section_index = symbol.st_shndx,
            },
    };
}

ElfDynamicLookupResult FindSysvSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                      std::string_view name) {
    if (!view.sysv_hash_va.has_value()) {
        return {.status = ElfDynamicLookupStatus::kUnavailable};
    }
    std::array<uint32_t, 2> header{};
    if (!reader.Read(*view.sysv_hash_va, header.data(), sizeof(header)) || header[0] == 0) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const uint32_t bucket_count = header[0];
    const uint32_t chain_count = header[1];
    if (chain_count == 0 || *view.sysv_hash_va > UINT64_MAX - 8) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const uint64_t buckets_va = *view.sysv_hash_va + 8;
    const uint64_t bucket_offset = uint64_t{SysvHash(name) % bucket_count} * sizeof(uint32_t);
    if (buckets_va > UINT64_MAX - bucket_offset) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    uint32_t symbol_index = 0;
    if (!ReadValue(reader, buckets_va + bucket_offset, &symbol_index)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const uint64_t buckets_size = uint64_t{bucket_count} * sizeof(uint32_t);
    if (buckets_va > UINT64_MAX - buckets_size) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const uint64_t chains_va = buckets_va + buckets_size;
    for (uint32_t visited = 0; symbol_index != STN_UNDEF; ++visited) {
        if (symbol_index >= chain_count || visited >= chain_count) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        const auto match = MatchSymbol(reader, view, symbol_index, name);
        if (match.status == ElfDynamicLookupStatus::kFound ||
            match.status == ElfDynamicLookupStatus::kMalformed) {
            return match;
        }
        uint32_t next = 0;
        if (!ReadValue(reader, chains_va + uint64_t{symbol_index} * sizeof(uint32_t), &next)) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        symbol_index = next;
    }
    return {.status = ElfDynamicLookupStatus::kNotFound};
}

ElfDynamicLookupResult FindGnuSymbol(const ElfVaReader& reader, const ElfDynamicView& view,
                                     std::string_view name) {
    if (!view.gnu_hash_va.has_value()) {
        return {.status = ElfDynamicLookupStatus::kUnavailable};
    }
    std::array<uint32_t, 4> header{};
    if (!reader.Read(*view.gnu_hash_va, header.data(), sizeof(header))) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const uint32_t bucket_count = header[0];
    const uint32_t symbol_offset = header[1];
    const uint32_t bloom_words = header[2];
    const uint32_t bloom_shift = header[3];
    if (bucket_count == 0 || bloom_words == 0 || (bloom_words & (bloom_words - 1)) != 0) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }

    const uint32_t hash = GnuHash(name);
    constexpr uint32_t kWordBits = sizeof(Elf64_Addr) * 8;
    const uint64_t bloom_base = *view.gnu_hash_va + sizeof(header);
    const uint64_t bloom_index = (hash / kWordBits) & (bloom_words - 1);
    Elf64_Addr bloom = 0;
    if (!ReadValue(reader, bloom_base + bloom_index * sizeof(Elf64_Addr), &bloom)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const Elf64_Addr mask = (Elf64_Addr{1} << (hash % kWordBits)) |
                            (Elf64_Addr{1} << ((hash >> bloom_shift) % kWordBits));
    if ((bloom & mask) != mask) return {.status = ElfDynamicLookupStatus::kNotFound};

    const uint64_t buckets_va = bloom_base + uint64_t{bloom_words} * sizeof(Elf64_Addr);
    uint32_t symbol_index = 0;
    if (!ReadValue(reader, buckets_va + uint64_t{hash % bucket_count} * sizeof(uint32_t),
                   &symbol_index)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    if (symbol_index == 0) return {.status = ElfDynamicLookupStatus::kNotFound};
    if (symbol_index < symbol_offset) return {.status = ElfDynamicLookupStatus::kMalformed};

    const uint64_t chains_va = buckets_va + uint64_t{bucket_count} * sizeof(uint32_t);
    const auto remaining = reader.ReadableBytes(chains_va);
    if (!remaining.has_value()) return {.status = ElfDynamicLookupStatus::kMalformed};
    const uint64_t max_chain_entries = *remaining / sizeof(uint32_t);
    uint64_t chain_index = symbol_index - symbol_offset;
    for (; chain_index < max_chain_entries; ++chain_index, ++symbol_index) {
        uint32_t chain_hash = 0;
        if (!ReadValue(reader, chains_va + chain_index * sizeof(uint32_t), &chain_hash)) {
            return {.status = ElfDynamicLookupStatus::kMalformed};
        }
        if ((chain_hash | 1U) == (hash | 1U)) {
            const auto match = MatchSymbol(reader, view, symbol_index, name);
            if (match.status == ElfDynamicLookupStatus::kFound ||
                match.status == ElfDynamicLookupStatus::kMalformed) {
                return match;
            }
        }
        if ((chain_hash & 1U) != 0) return {.status = ElfDynamicLookupStatus::kNotFound};
    }
    return {.status = ElfDynamicLookupStatus::kMalformed};
}

}  // namespace

std::optional<uint64_t> ElfVaToFileOffset(std::span<const ElfProgramHeaderView> headers,
                                          uint64_t va, size_t size) {
    if (size == 0) return std::nullopt;
    const uint64_t width = static_cast<uint64_t>(size);
    for (const auto& header : headers) {
        if (header.type != PT_LOAD || va < header.virtual_address) continue;
        const uint64_t delta = va - header.virtual_address;
        if (delta >= header.file_size || width > header.file_size - delta ||
            header.offset > UINT64_MAX - delta) {
            continue;
        }
        return header.offset + delta;
    }
    return std::nullopt;
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

    out_headers->clear();
    out_headers->reserve(header.e_phnum);
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr phdr{};
        const uint64_t offset = header.e_phoff + uint64_t{index} * sizeof(Elf64_Phdr);
        memcpy(&phdr, bytes.data() + static_cast<size_t>(offset), sizeof(phdr));
        if (phdr.p_type == PT_LOAD && phdr.p_filesz > phdr.p_memsz) return false;
        out_headers->push_back({
            .type = phdr.p_type,
            .flags = phdr.p_flags,
            .offset = phdr.p_offset,
            .virtual_address = phdr.p_vaddr,
            .file_size = phdr.p_filesz,
            .memory_size = phdr.p_memsz,
        });
    }
    return true;
}

std::optional<ElfProgramHeaderView> FindElfProgramHeader(
    std::span<const ElfProgramHeaderView> headers, uint32_t type) {
    std::optional<ElfProgramHeaderView> found;
    for (const auto& header : headers) {
        if (header.type != type) continue;
        if (found.has_value()) return std::nullopt;
        found = header;
    }
    return found;
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
        return {.status = ElfDynamicViewStatus::kUnavailable};
    }
    if ((dynamic_size % sizeof(Elf64_Dyn)) != 0) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }
    const auto dynamic_backing = reader.ReadableBytes(dynamic_va);
    if (!dynamic_backing.has_value() || dynamic_size > *dynamic_backing) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }

    ElfDynamicView view{};
    bool saw_null = false;
    const uint64_t entry_count = dynamic_size / sizeof(Elf64_Dyn);
    for (uint64_t index = 0; index < entry_count; ++index) {
        Elf64_Dyn entry{};
        const uint64_t entry_va = dynamic_va + index * sizeof(Elf64_Dyn);
        if (entry_va < dynamic_va || !reader.Read(entry_va, &entry, sizeof(entry))) {
            return {.status = ElfDynamicViewStatus::kMalformed};
        }
        if (entry.d_tag == DT_NULL) {
            saw_null = true;
            break;
        }
        switch (entry.d_tag) {
        case DT_SYMTAB:
            view.symtab_va = entry.d_un.d_ptr;
            break;
        case DT_STRTAB:
            view.strtab_va = entry.d_un.d_ptr;
            break;
        case DT_STRSZ:
            view.strtab_size = entry.d_un.d_val;
            break;
        case DT_SYMENT:
            view.symbol_entry_size = entry.d_un.d_val;
            break;
        case DT_HASH:
            view.sysv_hash_va = entry.d_un.d_ptr;
            break;
        case DT_GNU_HASH:
            view.gnu_hash_va = entry.d_un.d_ptr;
            break;
        default:
            break;
        }
    }
    if (!saw_null) return {.status = ElfDynamicViewStatus::kMalformed};
    if (view.symbol_entry_size != 0 && view.symbol_entry_size != sizeof(Elf64_Sym)) {
        return {.status = ElfDynamicViewStatus::kMalformed};
    }
    if (view.symtab_va == 0 || view.strtab_va == 0 || view.strtab_size == 0 ||
        view.symbol_entry_size == 0) {
        return {.status = ElfDynamicViewStatus::kUnavailable};
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
    return {.status = ElfDynamicLookupStatus::kUnavailable};
}

}  // namespace dartplant
