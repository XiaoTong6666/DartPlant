// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <elf.h>
#include <link.h>
#include <string.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "core/internal.h"

namespace dartplant {
namespace {

constexpr uint32_t kGnuBuildIdType = 3;

uintptr_t AlignUp(uintptr_t value, uintptr_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
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

std::string ReadBuildId(const dl_phdr_info* info) {
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & header = info->dlpi_phdr[i];
        if (header.p_type != PT_NOTE) {
            continue;
        }
        const auto* cursor = reinterpret_cast<const uint8_t*>(info->dlpi_addr + header.p_vaddr);
        const auto* end = cursor + header.p_memsz;
        while (cursor + sizeof(ElfW(Nhdr)) <= end) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(cursor);
            cursor += sizeof(ElfW(Nhdr));
            if (cursor + AlignUp(note->n_namesz, 4) + AlignUp(note->n_descsz, 4) > end) {
                break;
            }
            const char* name = reinterpret_cast<const char*>(cursor);
            cursor += AlignUp(note->n_namesz, 4);
            const uint8_t* descriptor = cursor;
            cursor += AlignUp(note->n_descsz, 4);
            if (note->n_type == kGnuBuildIdType && note->n_namesz >= 3 &&
                memcmp(name, "GNU", 3) == 0) {
                return BytesToHex(descriptor, note->n_descsz);
            }
        }
    }
    return {};
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
        });
    }
    ModuleImage image;
    if (!BuildModuleImageFromProgramHeaders(info->dlpi_name, info->dlpi_addr, headers, &image)) {
        return 0;
    }
    image.build_id = ReadBuildId(info);
    modules->push_back(std::move(image));
    return 0;
}

}  // namespace

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

std::optional<uintptr_t> ModuleImage::Resolve(DartPlantAddressKind kind, uint64_t address,
                                              uint64_t section_va) const {
    switch (kind) {
    case DARTPLANT_ADDRESS_RUNTIME:
        return static_cast<uintptr_t>(address);
    case DARTPLANT_ADDRESS_ELF_VA:
        if (address > UINTPTR_MAX - load_bias) {
            return std::nullopt;
        }
        return load_bias + static_cast<uintptr_t>(address);
    case DARTPLANT_ADDRESS_FILE_OFFSET:
        for (const ExecutableRange& range : executable_ranges) {
            if (range.file_offset > UINT64_MAX - range.file_size) continue;
            const uint64_t file_end = range.file_offset + range.file_size;
            if (address >= range.file_offset && address < file_end) {
                const uint64_t delta = address - range.file_offset;
                if (range.virtual_address > UINTPTR_MAX - load_bias ||
                    delta > UINTPTR_MAX - (load_bias + range.virtual_address)) {
                    return std::nullopt;
                }
                return load_bias + range.virtual_address + delta;
            }
        }
        return std::nullopt;
    case DARTPLANT_ADDRESS_SNAPSHOT_OFFSET:
        if (section_va == 0) {
            if (executable_ranges.empty()) {
                return std::nullopt;
            }
            section_va = executable_ranges.front().virtual_address;
        }
        if (section_va > UINTPTR_MAX - load_bias) {
            return std::nullopt;
        }
        const uintptr_t base = load_bias + static_cast<uintptr_t>(section_va);
        if (address > UINTPTR_MAX - base) {
            return std::nullopt;
        }
        return base + static_cast<uintptr_t>(address);
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
