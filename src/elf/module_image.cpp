// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "elf/module_image.h"

#include <elf.h>
#include <link.h>
#include <string.h>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

#include "core/internal.h"
#include "elf/dynamic_view.h"

namespace dartplant {
namespace {

constexpr uint32_t kGnuBuildIdType = 3;

bool AlignUp4(uint64_t value, uint64_t* output) {
    if (output == nullptr || value > std::numeric_limits<uint64_t>::max() - 3) return false;
    *output = (value + 3) & ~uint64_t{3};
    return true;
}

std::string BaseName(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string BytesToHex(const uint8_t* bytes, size_t size) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return stream.str();
}

ElfBuildIdResult ReadLoadedGnuBuildIdNotesInternal(uintptr_t load_bias,
                                                   std::span<const ElfProgramHeaderView> headers) {
    if (!ValidateElfLoadLayout(headers)) {
        return {.status = ElfBuildIdStatus::kMalformed, .build_id = {}};
    }
    ElfBuildIdResult aggregate = {.status = ElfBuildIdStatus::kNotFound, .build_id = {}};
    for (const auto& header : headers) {
        if (header.type != PT_NOTE) continue;
        if (!ElfProgramHeaderHasCanonicalFileBacking(headers, header, PF_R) ||
            header.virtual_address > UINTPTR_MAX - load_bias) {
            return {.status = ElfBuildIdStatus::kMalformed, .build_id = {}};
        }
        const uintptr_t start = load_bias + static_cast<uintptr_t>(header.virtual_address);
        if (header.file_size > UINTPTR_MAX - start) {
            return {.status = ElfBuildIdStatus::kMalformed, .build_id = {}};
        }
        const auto result = ParseGnuBuildIdNotes(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(start), static_cast<size_t>(header.file_size)));
        aggregate = MergeElfBuildIdResults(aggregate, result);
        if (aggregate.status == ElfBuildIdStatus::kMalformed) return aggregate;
    }
    return aggregate;
}

int CollectModule(dl_phdr_info* info, size_t, void* data) {
    auto* modules = static_cast<std::vector<ModuleImage>*>(data);
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
        return 0;
    }

    std::vector<ElfProgramHeaderView> headers;
    headers.reserve(info->dlpi_phnum);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & header = info->dlpi_phdr[i];
        headers.push_back({
            .type = header.p_type,
            .flags = header.p_flags,
            .offset = header.p_offset,
            .virtual_address = header.p_vaddr,
            .file_size = header.p_filesz,
            .memory_size = header.p_memsz,
            .alignment = header.p_align,
        });
    }
    if (!ValidateElfLoadLayout(headers)) return 0;
    ModuleImage image;
    if (!BuildModuleImageFromProgramHeaders(info->dlpi_name, info->dlpi_addr, headers, &image)) {
        return 0;
    }
    const auto build_id = ReadLoadedGnuBuildIdNotes(info->dlpi_addr, headers);
    if (build_id.status == ElfBuildIdStatus::kMalformed ||
        build_id.status == ElfBuildIdStatus::kAmbiguous) {
        return 0;
    }
    image.build_id = build_id.build_id;
    modules->push_back(std::move(image));
    return 0;
}

}  // namespace

ElfBuildIdResult ReadLoadedGnuBuildIdNotes(uintptr_t load_bias,
                                           std::span<const ElfProgramHeaderView> headers) {
    return ReadLoadedGnuBuildIdNotesInternal(load_bias, headers);
}

ElfBuildIdResult ParseGnuBuildIdNotes(std::span<const uint8_t> notes) {
    uint64_t cursor = 0;
    std::optional<std::string> build_id;
    while (cursor < notes.size()) {
        const uint64_t remaining = notes.size() - cursor;
        if (remaining < sizeof(ElfW(Nhdr))) {
            const auto tail = notes.subspan(static_cast<size_t>(cursor));
            if (std::all_of(tail.begin(), tail.end(), [](uint8_t value) { return value == 0; })) {
                break;
            }
            return {.status = ElfBuildIdStatus::kMalformed, .build_id = {}};
        }

        ElfW(Nhdr) note{};
        memcpy(&note, notes.data() + static_cast<size_t>(cursor), sizeof(note));
        cursor += sizeof(note);

        uint64_t padded_name = 0;
        uint64_t padded_desc = 0;
        if (!AlignUp4(note.n_namesz, &padded_name) || !AlignUp4(note.n_descsz, &padded_desc) ||
            padded_name > notes.size() - cursor) {
            return {.status = ElfBuildIdStatus::kMalformed, .build_id = {}};
        }
        const uint64_t name_offset = cursor;
        cursor += padded_name;
        if (padded_desc > notes.size() - cursor) {
            return {.status = ElfBuildIdStatus::kMalformed, .build_id = {}};
        }
        const uint64_t descriptor_offset = cursor;
        cursor += padded_desc;

        if (note.n_type == kGnuBuildIdType && note.n_namesz == 4 && note.n_descsz != 0 &&
            memcmp(notes.data() + static_cast<size_t>(name_offset), "GNU\0", 4) == 0) {
            const std::string candidate =
                BytesToHex(notes.data() + static_cast<size_t>(descriptor_offset),
                           static_cast<size_t>(note.n_descsz));
            if (build_id.has_value() && *build_id != candidate) {
                return {.status = ElfBuildIdStatus::kAmbiguous, .build_id = {}};
            }
            build_id = candidate;
        }
    }
    if (build_id.has_value()) {
        return {.status = ElfBuildIdStatus::kFound, .build_id = std::move(*build_id)};
    }
    return {.status = ElfBuildIdStatus::kNotFound, .build_id = {}};
}

ElfBuildIdResult MergeElfBuildIdResults(const ElfBuildIdResult& current,
                                        const ElfBuildIdResult& next) {
    if (current.status == ElfBuildIdStatus::kMalformed ||
        next.status == ElfBuildIdStatus::kMalformed) {
        return {.status = ElfBuildIdStatus::kMalformed, .build_id = {}};
    }
    if (current.status == ElfBuildIdStatus::kAmbiguous ||
        next.status == ElfBuildIdStatus::kAmbiguous) {
        return {.status = ElfBuildIdStatus::kAmbiguous, .build_id = {}};
    }
    if (current.status == ElfBuildIdStatus::kFound && next.status == ElfBuildIdStatus::kFound) {
        return current.build_id == next.build_id
                   ? current
                   : ElfBuildIdResult{.status = ElfBuildIdStatus::kAmbiguous, .build_id = {}};
    }
    return current.status == ElfBuildIdStatus::kFound ? current : next;
}

bool BuildModuleImageFromProgramHeaders(std::string_view path, uintptr_t load_bias,
                                        std::span<const ElfProgramHeaderView> headers,
                                        ModuleImage* out_image) {
    if (out_image == nullptr || path.empty()) return false;
    ModuleImage image;
    image.path = path;
    image.name = BaseName(path);
    image.load_bias = load_bias;

    for (const ElfProgramHeaderView& header : headers) {
        if (header.type != PT_LOAD || (header.flags & PF_X) == 0) continue;
        if (header.file_size > header.memory_size ||
            header.virtual_address > UINTPTR_MAX - load_bias) {
            return false;
        }
        const uintptr_t start = load_bias + static_cast<uintptr_t>(header.virtual_address);
        if (header.memory_size > UINTPTR_MAX - start ||
            header.offset > UINT64_MAX - header.file_size) {
            return false;
        }
        image.executable_ranges.push_back({
            .start = start,
            .end = start + static_cast<uintptr_t>(header.memory_size),
            .file_offset = header.offset,
            .virtual_address = header.virtual_address,
            .file_size = header.file_size,
        });
    }
    std::sort(image.executable_ranges.begin(), image.executable_ranges.end(),
              [](const ExecutableRange& left, const ExecutableRange& right) {
                  return left.virtual_address < right.virtual_address;
              });
    *out_image = std::move(image);
    return true;
}

bool ModuleImage::ContainsExecutable(uintptr_t address, size_t size) const {
    if (size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    const uintptr_t end = address + size;
    return std::any_of(executable_ranges.begin(), executable_ranges.end(),
                       [address, end](const ExecutableRange& range) {
                           return address >= range.start && end <= range.end;
                       });
}

std::optional<uintptr_t> ModuleImage::Resolve(DartPlantAddressKind kind, uint64_t address) const {
    switch (kind) {
    case DARTPLANT_ADDRESS_RUNTIME:
        return static_cast<uintptr_t>(address);
    case DARTPLANT_ADDRESS_ELF_VA:
        if (address > UINTPTR_MAX - load_bias) {
            return std::nullopt;
        }
        return load_bias + static_cast<uintptr_t>(address);
    case DARTPLANT_ADDRESS_FILE_OFFSET: {
        std::optional<uintptr_t> result;
        for (const ExecutableRange& range : executable_ranges) {
            if (range.file_offset > UINT64_MAX - range.file_size) continue;
            const uint64_t file_end = range.file_offset + range.file_size;
            if (address < range.file_offset || address >= file_end) continue;
            const uint64_t delta = address - range.file_offset;
            if (range.virtual_address > UINTPTR_MAX - load_bias) return std::nullopt;
            const uintptr_t base = load_bias + static_cast<uintptr_t>(range.virtual_address);
            if (delta > UINTPTR_MAX - base) return std::nullopt;
            const uintptr_t candidate = base + static_cast<uintptr_t>(delta);
            if (result.has_value() && *result != candidate) return std::nullopt;
            result = candidate;
        }
        return result;
    }
    case DARTPLANT_ADDRESS_SNAPSHOT_OFFSET:
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<ModuleImage> EnumerateModules() {
    std::vector<ModuleImage> modules;
    dl_iterate_phdr(CollectModule, &modules);
    return modules;
}

std::optional<ModuleImage> FindModule(const std::vector<ModuleImage>& modules,
                                      const std::string& name) {
    auto found = std::find_if(modules.begin(), modules.end(), [&name](const ModuleImage& image) {
        return image.name == name || image.path == name;
    });
    if (found == modules.end()) {
        return std::nullopt;
    }
    return *found;
}

std::string FingerprintCode(const void* address, size_t size) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t hash = kOffsetBasis;
    const auto* bytes = static_cast<const uint8_t*>(address);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

}  // namespace dartplant
