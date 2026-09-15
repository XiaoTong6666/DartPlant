// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "elf/dynamic_view.h"
#include "elf/module_image.h"
#include "runtime/flutter_snapshot_internal.h"

namespace {

std::optional<std::vector<uint8_t>> ReadFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

void PrintHex(std::string_view key, uint64_t value) {
    std::cout << key << "\t0x" << std::hex << value << std::dec << '\n';
}

void PrintString(std::string_view key, std::string_view value) {
    std::cout << key << '\t' << value << '\n';
}

bool EmitSymbol(std::string_view name, const dartplant::ElfDynamicLookupResult& result,
                std::span<const dartplant::ElfProgramHeaderView> headers, uint32_t required_flags,
                const std::vector<uint8_t>& bytes) {
    if (result.status != dartplant::ElfDynamicLookupStatus::kFound ||
        !dartplant::SnapshotSymbolMatchesContract(result.symbol, headers, required_flags, false) ||
        result.symbol.size > SIZE_MAX) {
        return false;
    }
    const auto offset = dartplant::ElfVaToFileOffset(headers, result.symbol.value,
                                                     static_cast<size_t>(result.symbol.size));
    if (!offset.has_value() || *offset > bytes.size() ||
        result.symbol.size > bytes.size() - *offset) {
        return false;
    }
    const std::string prefix = "symbol." + std::string(name) + ".";
    PrintHex(prefix + "value", result.symbol.value);
    PrintHex(prefix + "size", result.symbol.size);
    PrintHex(prefix + "file_offset", *offset);
    PrintHex(prefix + "binding", ELF64_ST_BIND(result.symbol.info));
    PrintHex(prefix + "type", ELF64_ST_TYPE(result.symbol.info));
    PrintHex(prefix + "section_index", result.symbol.section_index);
    return true;
}

std::optional<dartplant::ElfBuildIdResult> ReadFileBuildId(
    const std::vector<uint8_t>& bytes, std::span<const dartplant::ElfProgramHeaderView> headers) {
    dartplant::ElfBuildIdResult aggregate{};
    for (const auto& header : headers) {
        if (header.type != PT_NOTE) continue;
        if (header.offset > bytes.size() || header.file_size > bytes.size() - header.offset ||
            header.file_size > SIZE_MAX) {
            return std::nullopt;
        }
        const auto notes =
            std::span<const uint8_t>(bytes.data() + static_cast<size_t>(header.offset),
                                     static_cast<size_t>(header.file_size));
        aggregate =
            dartplant::MergeElfBuildIdResults(aggregate, dartplant::ParseGnuBuildIdNotes(notes));
    }
    return aggregate;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "usage: dartplant_elf_contract_probe <elf> [--symbol <name>]\n";
        return 2;
    }
    const bool generic_symbol_mode = argc == 4;
    if (generic_symbol_mode && std::string_view(argv[2]) != "--symbol") {
        std::cerr << "expected --symbol\n";
        return 2;
    }
    const auto bytes = ReadFile(argv[1]);
    if (!bytes.has_value() || bytes->size() < sizeof(Elf64_Ehdr)) {
        std::cerr << "cannot read ELF\n";
        return 3;
    }
    Elf64_Ehdr elf_header{};
    std::memcpy(&elf_header, bytes->data(), sizeof(elf_header));
    if (elf_header.e_ident[EI_CLASS] != ELFCLASS64 || elf_header.e_machine != EM_AARCH64) {
        std::cerr << "not ELF64/AArch64\n";
        return 4;
    }

    std::vector<dartplant::ElfProgramHeaderView> headers;
    if (!dartplant::ParseElf64ProgramHeaders(*bytes, &headers) ||
        !dartplant::ValidateElfLoadSegments(*bytes, headers)) {
        std::cerr << "program header validation failed\n";
        return 5;
    }
    size_t load_count = 0;
    for (const auto& header : headers) load_count += header.type == PT_LOAD ? 1 : 0;
    PrintString("class", "ELF64");
    PrintString("machine", "AArch64");
    PrintHex("load_count", load_count);

    const auto build_id = ReadFileBuildId(*bytes, headers);
    if (!build_id.has_value() || build_id->status != dartplant::ElfBuildIdStatus::kFound) {
        std::cerr << "GNU Build ID unavailable or ambiguous\n";
        return 6;
    }
    PrintString("build_id", build_id->build_id);

    const auto dynamic = dartplant::FindElfProgramHeader(headers, PT_DYNAMIC);
    if (dynamic.status != dartplant::ElfProgramHeaderLookupStatus::kFound ||
        !dartplant::ElfProgramHeaderHasCanonicalFileBacking(headers, dynamic.header, PF_R)) {
        std::cerr << "PT_DYNAMIC unavailable or malformed\n";
        return 7;
    }
    dartplant::FileElfReader reader(*bytes, headers);
    const auto dynamic_view = dartplant::ReadElfDynamicView(reader, dynamic.header.virtual_address,
                                                            dynamic.header.file_size);
    if (dynamic_view.status != dartplant::ElfDynamicViewStatus::kAvailable) {
        std::cerr << "dynamic view malformed\n";
        return 8;
    }
    const auto& view = dynamic_view.view;
    PrintString("hash_style", view.sysv_hash_va.has_value() && view.gnu_hash_va.has_value() ? "both"
                              : view.sysv_hash_va.has_value()                               ? "sysv"
                              : view.gnu_hash_va.has_value()                                ? "gnu"
                                                             : "none");
    PrintHex("symtab_va", view.symtab_va);
    PrintHex("strtab_va", view.strtab_va);
    PrintHex("strtab_size", view.strtab_size);
    PrintHex("symbol_entry_size", view.symbol_entry_size);

    if (generic_symbol_mode) {
        const std::string_view name(argv[3]);
        const auto symbol = dartplant::FindElfDynamicSymbol(reader, view, name);
        if (symbol.status != dartplant::ElfDynamicLookupStatus::kFound ||
            symbol.symbol.size > SIZE_MAX) {
            std::cerr << "generic dynamic symbol lookup failed\n";
            return 9;
        }
        const auto offset = dartplant::ElfVaToFileOffset(headers, symbol.symbol.value,
                                                         static_cast<size_t>(symbol.symbol.size));
        if (!offset.has_value()) {
            std::cerr << "generic dynamic symbol is not file-backed\n";
            return 9;
        }
        PrintString("generic_symbol.name", name);
        PrintHex("generic_symbol.value", symbol.symbol.value);
        PrintHex("generic_symbol.size", symbol.symbol.size);
        PrintHex("generic_symbol.file_offset", *offset);
        PrintHex("generic_symbol.binding", ELF64_ST_BIND(symbol.symbol.info));
        PrintHex("generic_symbol.type", ELF64_ST_TYPE(symbol.symbol.info));
        return 0;
    }

    const auto data = dartplant::FindElfDynamicSymbol(reader, view, "_kDartIsolateSnapshotData");
    const auto instructions =
        dartplant::FindElfDynamicSymbol(reader, view, "_kDartIsolateSnapshotInstructions");
    const auto snapshot_build_id =
        dartplant::FindElfDynamicSymbol(reader, view, "_kDartSnapshotBuildId");
    if (!EmitSymbol("_kDartIsolateSnapshotData", data, headers, PF_R, *bytes) ||
        !EmitSymbol("_kDartIsolateSnapshotInstructions", instructions, headers, PF_R | PF_X,
                    *bytes) ||
        !EmitSymbol("_kDartSnapshotBuildId", snapshot_build_id, headers, PF_R, *bytes)) {
        std::cerr << "snapshot symbol contract failed\n";
        return 9;
    }

    const auto data_offset = dartplant::ElfVaToFileOffset(headers, data.symbol.value,
                                                          static_cast<size_t>(data.symbol.size));
    if (!data_offset.has_value()) return 10;
    const auto data_bytes = std::span<const uint8_t>(
        bytes->data() + static_cast<size_t>(*data_offset), static_cast<size_t>(data.symbol.size));
    const auto snapshot_header = dartplant::ParseDartSnapshotHeader(data_bytes);
    if (!snapshot_header.has_value()) {
        std::cerr << "Dart snapshot header malformed\n";
        return 11;
    }
    PrintString("snapshot_hash", snapshot_header->snapshot_hash);
    PrintString("snapshot_features", snapshot_header->features);
    PrintHex("snapshot_kind", snapshot_header->kind);
    PrintHex("snapshot_declared_length", snapshot_header->declared_length);
    if (const auto program_hash =
            dartplant::ParseDartDeferredProgramHash(data_bytes, *snapshot_header);
        program_hash.has_value()) {
        PrintHex("deferred_program_hash", *program_hash);
    } else {
        PrintString("deferred_program_hash", "none");
    }
    return 0;
}
