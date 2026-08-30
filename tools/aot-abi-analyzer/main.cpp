// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "aot_abi_analyzer.h"

namespace {

std::string RepresentationName(dartplant::aot::ParameterEvidence representation) {
    using Representation = dartplant::aot::ParameterEvidence;
    switch (representation) {
    case Representation::kTagged:
        return "tagged";
    case Representation::kUnboxedInt64:
        return "unboxed-int64";
    case Representation::kUnboxedDouble:
        return "unboxed-double";
    case Representation::kPairOfTagged:
        return "pair-of-tagged";
    case Representation::kUnknown:
        return "unknown";
    }
    return "unknown";
}

std::string LocationName(dartplant::aot::LocationKind kind) {
    using Kind = dartplant::aot::LocationKind;
    switch (kind) {
    case Kind::kGpRegister:
        return "gp";
    case Kind::kFpuRegister:
        return "fpu";
    case Kind::kEntryStack:
        return "entry-stack";
    case Kind::kUnknown:
        return "unknown";
    }
    return "unknown";
}

std::string AddressMaterializationName(dartplant::aot::AddressMaterializationKind kind) {
    using Kind = dartplant::aot::AddressMaterializationKind;
    switch (kind) {
    case Kind::kAdr:
        return "adr";
    case Kind::kAdrpAdd:
        return "adrp-add";
    }
    return "unknown";
}

bool ParseAddress(std::string_view value, uint64_t* output) {
    if (output == nullptr) return false;
    int base = 10;
    if (value.starts_with("0x")) {
        base = 16;
        value.remove_prefix(2);
    }
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), *output, base);
    return error == std::errc{} && end == value.data() + value.size();
}

void PrintJson(const dartplant::aot::AnalysisResult& result) {
    std::cout << "{\n";
    std::cout << "  \"schema_version\": 1,\n";
    std::cout << "  \"decoded_instructions\": " << result.decoded_instructions << ",\n";
    std::cout << "  \"basic_block_count\": " << result.basic_block_count << ",\n";
    std::cout << "  \"has_unknown_control_flow\": "
              << (result.has_unknown_control_flow ? "true" : "false") << ",\n";
    std::cout << "  \"uses_arguments_descriptor\": "
              << (result.uses_arguments_descriptor ? "true" : "false") << ",\n";
    std::cout << "  \"reached_return\": " << (result.reached_return ? "true" : "false") << ",\n";
    std::cout << "  \"structural_evidence_truncated\": "
              << (result.structural_evidence_truncated ? "true" : "false") << ",\n";
    std::cout << "  \"direct_calls\": [";
    for (size_t index = 0; index < result.direct_call_count; ++index) {
        if (index != 0) std::cout << ", ";
        std::cout << "{\"site\": " << result.direct_calls[index].site
                  << ", \"target\": " << result.direct_calls[index].target << "}";
    }
    std::cout << "],\n";

    std::cout << "  \"return_sites\": [";
    for (size_t index = 0; index < result.return_site_count; ++index) {
        if (index != 0) std::cout << ", ";
        std::cout << result.return_sites[index];
    }
    std::cout << "],\n";

    std::cout << "  \"external_branches\": [";
    for (size_t index = 0; index < result.external_branch_count; ++index) {
        if (index != 0) std::cout << ", ";
        std::cout << "{\"site\": " << result.external_branches[index].site
                  << ", \"target\": " << result.external_branches[index].target << "}";
    }
    std::cout << "],\n";

    std::cout << "  \"indirect_call_sites\": [";
    for (size_t index = 0; index < result.indirect_call_count; ++index) {
        if (index != 0) std::cout << ", ";
        std::cout << result.indirect_call_sites[index];
    }
    std::cout << "],\n";

    std::cout << "  \"indirect_branch_sites\": [";
    for (size_t index = 0; index < result.indirect_branch_count; ++index) {
        if (index != 0) std::cout << ", ";
        std::cout << result.indirect_branch_sites[index];
    }
    std::cout << "],\n";

    std::cout << "  \"address_materializations\": [";
    for (size_t index = 0; index < result.address_materialization_count; ++index) {
        if (index != 0) std::cout << ", ";
        const auto& materialization = result.address_materializations[index];
        std::cout << "{\"kind\": \"" << AddressMaterializationName(materialization.kind)
                  << "\", \"site\": " << materialization.site
                  << ", \"completion_site\": " << materialization.completion_site
                  << ", \"destination_register\": "
                  << static_cast<unsigned>(materialization.destination_register)
                  << ", \"target\": " << materialization.target << "}";
    }
    std::cout << "],\n";

    std::cout << "  \"observations\": [";
    for (size_t index = 0; index < result.observation_count; ++index) {
        if (index != 0) std::cout << ",";
        const auto& observation = result.observations[index];
        std::cout << "\n    {\"location\": \"" << LocationName(observation.location.kind)
                  << "\", \"register\": "
                  << static_cast<unsigned>(observation.location.register_index)
                  << ", \"stack_offset\": " << observation.location.stack_offset
                  << ", \"representation\": \"" << RepresentationName(observation.representation)
                  << "\"}";
    }
    if (result.observation_count != 0) std::cout << "\n  ";
    std::cout << "]\n";
    std::cout << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string code_path;
    uint64_t address = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--code-file" && index + 1 < argc) {
            code_path = argv[++index];
        } else if (arg == "--address" && index + 1 < argc) {
            if (!ParseAddress(argv[++index], &address)) {
                std::cerr << "invalid --address\n";
                return 2;
            }
        } else {
            std::cerr << "usage: dartplant_aot_abi_analyzer_cli --code-file FILE "
                         "[--address ADDRESS]\n";
            return 2;
        }
    }
    if (code_path.empty()) {
        std::cerr << "missing --code-file\n";
        return 2;
    }

    std::ifstream input(code_path, std::ios::binary);
    if (!input) {
        std::cerr << "failed to open code file\n";
        return 2;
    }
    const std::vector<uint8_t> code((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    if (code.empty()) {
        std::cerr << "code file is empty\n";
        return 2;
    }

    PrintJson(dartplant::aot::AnalyzeArm64Entry(code.data(), code.size(), address));
    return 0;
}
