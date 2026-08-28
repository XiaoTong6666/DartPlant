// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "aot_abi_analyzer.h"

#include <capstone/arm64.h>
#include <capstone/capstone.h>

// Capstone 6 keeps the ARM64 compatibility structs but uses AARCH64 enum
// types inside their operands. Keep all operand comparisons in that enum.
#define ARM64_OP_REG AARCH64_OP_REG
#define ARM64_OP_IMM AARCH64_OP_IMM
#define ARM64_OP_MEM AARCH64_OP_MEM
#define ARM64_REG_X0 AARCH64_REG_X0
#define ARM64_REG_X4 AARCH64_REG_X4
#define ARM64_REG_X30 AARCH64_REG_X30
#define ARM64_REG_W0 AARCH64_REG_W0
#define ARM64_REG_W30 AARCH64_REG_W30
#define ARM64_REG_XZR AARCH64_REG_XZR
#define ARM64_REG_WZR AARCH64_REG_WZR
#define ARM64_REG_SP AARCH64_REG_SP
#define ARM64_REG_FP AARCH64_REG_FP
#define ARM64_REG_X15 AARCH64_REG_X15

#include <algorithm>
#include <array>
#include <vector>

namespace dartplant::aot {
namespace {

constexpr size_t kGpCount = 31;
constexpr size_t kFpuCount = 32;
constexpr int32_t kStackMin = -256;
constexpr int32_t kStackMax = 768;
constexpr size_t kStackCount = (kStackMax - kStackMin) / 8 + 1;

struct Source {
    AbiLocation location{};
    bool valid = false;

    friend bool operator==(const Source&, const Source&) = default;
};

struct State {
    std::array<Source, kGpCount> gp{};
    std::array<Source, kFpuCount> fpu{};
    std::array<Source, kStackCount> stack{};
    // Current Dart SP (x15) and FP (x29) expressed as affine offsets from the
    // Dart entry SP. This is intentionally unrelated to the architecture CSP.
    bool dart_sp_known = true;
    int32_t dart_sp_to_entry = 0;
    bool fp_known = false;
    int32_t fp_to_entry = 0;
};

struct Decoded {
    cs_insn* instructions = nullptr;
    size_t instruction_count = 0;
    std::vector<std::vector<size_t>> successors;
    std::vector<size_t> block_starts;
};

bool IsGpRegister(aarch64_reg reg) {
    return (reg >= ARM64_REG_X0 && reg <= ARM64_REG_X30) ||
           (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30);
}

bool IsFpuRegister(aarch64_reg reg) { return reg >= AARCH64_REG_D0 && reg <= AARCH64_REG_D31; }

uint8_t RegisterIndex(aarch64_reg reg) {
    if (reg >= AARCH64_REG_X0 && reg <= AARCH64_REG_X30) return reg - AARCH64_REG_X0;
    if (reg >= AARCH64_REG_W0 && reg <= AARCH64_REG_W30) return reg - AARCH64_REG_W0;
    if (reg >= AARCH64_REG_D0 && reg <= AARCH64_REG_D31) return reg - AARCH64_REG_D0;
    return 0;
}

bool IsZeroRegister(aarch64_reg reg) { return reg == AARCH64_REG_XZR || reg == AARCH64_REG_WZR; }

bool IsConditionalBranch(const cs_insn& instruction) {
    const bool conditional_b = instruction.id == ARM64_INS_B && instruction.detail != nullptr &&
                               instruction.detail->arm64.cc != AArch64CC_Invalid &&
                               instruction.detail->arm64.cc != AArch64CC_AL;
    return conditional_b || instruction.id == ARM64_INS_BC || instruction.id == ARM64_INS_CBZ ||
           instruction.id == ARM64_INS_CBNZ || instruction.id == ARM64_INS_TBZ ||
           instruction.id == ARM64_INS_TBNZ;
}

bool IsTerminator(const cs_insn& instruction) {
    return instruction.id == ARM64_INS_RET || instruction.id == ARM64_INS_BR ||
           instruction.id == ARM64_INS_BLR || instruction.id == ARM64_INS_BRK ||
           instruction.id == ARM64_INS_B || IsConditionalBranch(instruction);
}

bool IsDirectBranch(const cs_insn& instruction) {
    return instruction.id == ARM64_INS_B || IsConditionalBranch(instruction);
}

bool IsDoubleOperation(unsigned int id) {
    switch (id) {
    case ARM64_INS_FADD:
    case ARM64_INS_FSUB:
    case ARM64_INS_FMUL:
    case ARM64_INS_FDIV:
    case ARM64_INS_FNEG:
    case ARM64_INS_FABS:
    case ARM64_INS_FSQRT:
    case ARM64_INS_FCMP:
        return true;
    default:
        return false;
    }
}

bool IsExplicitIntegerOperation(unsigned int id) {
    switch (id) {
    case ARM64_INS_SDIV:
    case ARM64_INS_UDIV:
    case ARM64_INS_SDIVR:
    case ARM64_INS_UDIVR:
    case ARM64_INS_SMULH:
    case ARM64_INS_UMULH:
        return true;
    default:
        return false;
    }
}

size_t StackIndex(int32_t offset) {
    if (offset < kStackMin || offset > kStackMax || offset % 8 != 0) return kStackCount;
    return static_cast<size_t>((offset - kStackMin) / 8);
}

Source StackSource(int32_t offset) {
    Source source;
    source.location.kind = LocationKind::kEntryStack;
    source.location.stack_offset = offset;
    source.valid = true;
    return source;
}

Source GpSource(uint8_t index) {
    Source source;
    source.location.kind = LocationKind::kGpRegister;
    source.location.register_index = index;
    source.valid = true;
    return source;
}

Source FpuSource(uint8_t index) {
    Source source;
    source.location.kind = LocationKind::kFpuRegister;
    source.location.register_index = index;
    source.valid = true;
    return source;
}

bool SameSource(const Source& left, const Source& right) {
    return left.valid == right.valid &&
           (!left.valid || (left.location.kind == right.location.kind &&
                            left.location.register_index == right.location.register_index &&
                            left.location.stack_offset == right.location.stack_offset));
}

void InvalidateGp(State* state, aarch64_reg reg) {
    if (IsGpRegister(reg)) state->gp[RegisterIndex(reg)] = {};
}

void InvalidateFpu(State* state, aarch64_reg reg) {
    if (IsFpuRegister(reg)) state->fpu[RegisterIndex(reg)] = {};
}

void InvalidateRegister(State* state, aarch64_reg reg) {
    InvalidateGp(state, reg);
    InvalidateFpu(state, reg);
}

Source ReadSource(const State& state, aarch64_reg reg) {
    if (IsGpRegister(reg)) return state.gp[RegisterIndex(reg)];
    if (IsFpuRegister(reg)) return state.fpu[RegisterIndex(reg)];
    return {};
}

void WriteSource(State* state, aarch64_reg reg, Source source) {
    if (IsGpRegister(reg)) state->gp[RegisterIndex(reg)] = source;
    if (IsFpuRegister(reg)) state->fpu[RegisterIndex(reg)] = source;
}

void MergeSource(Source* destination, const Source& incoming) {
    if (!SameSource(*destination, incoming)) *destination = {};
}

bool MergeState(State* destination, const State& incoming) {
    const State before = *destination;
    for (size_t i = 0; i < kGpCount; ++i) MergeSource(&destination->gp[i], incoming.gp[i]);
    for (size_t i = 0; i < kFpuCount; ++i) MergeSource(&destination->fpu[i], incoming.fpu[i]);
    for (size_t i = 0; i < kStackCount; ++i) MergeSource(&destination->stack[i], incoming.stack[i]);
    if (destination->dart_sp_known != incoming.dart_sp_known ||
        (destination->dart_sp_known &&
         destination->dart_sp_to_entry != incoming.dart_sp_to_entry)) {
        destination->dart_sp_known = false;
    }
    if (destination->fp_known != incoming.fp_known ||
        (destination->fp_known && destination->fp_to_entry != incoming.fp_to_entry)) {
        destination->fp_known = false;
    }
    return before.gp != destination->gp || before.fpu != destination->fpu ||
           before.stack != destination->stack ||
           before.dart_sp_known != destination->dart_sp_known ||
           before.dart_sp_to_entry != destination->dart_sp_to_entry ||
           before.fp_known != destination->fp_known ||
           before.fp_to_entry != destination->fp_to_entry;
}

bool ResolveEntryStackOffset(const State& state, const cs_aarch64_op& operand,
                             int32_t* out_offset) {
    if (operand.type != AARCH64_OP_MEM || out_offset == nullptr) return false;
    int32_t base_offset = 0;
    if (operand.mem.base == ARM64_REG_X15) {
        if (!state.dart_sp_known) return false;
        base_offset = state.dart_sp_to_entry;
    } else if (operand.mem.base == AARCH64_REG_FP) {
        if (!state.fp_known) return false;
        base_offset = state.fp_to_entry;
    } else {
        return false;
    }
    *out_offset = base_offset + operand.mem.disp;
    return true;
}

Source ReadMemorySource(const State& state, const cs_aarch64_op& operand) {
    int32_t offset = 0;
    if (!ResolveEntryStackOffset(state, operand, &offset)) return {};
    const size_t index = StackIndex(offset);
    return index < kStackCount ? state.stack[index] : Source{};
}

void WriteMemorySource(State* state, const cs_aarch64_op& operand, Source source) {
    int32_t offset = 0;
    if (!ResolveEntryStackOffset(*state, operand, &offset)) return;
    const size_t index = StackIndex(offset);
    if (index < kStackCount) state->stack[index] = source;
}

void UpdateDartStackPointer(const cs_insn& instruction, State* state) {
    const auto& arm64 = instruction.detail->arm64;
    if (arm64.op_count < 3 || arm64.operands[0].type != ARM64_OP_REG ||
        arm64.operands[0].reg != ARM64_REG_X15 || arm64.operands[1].type != ARM64_OP_REG ||
        arm64.operands[1].reg != ARM64_REG_X15 || arm64.operands[2].type != ARM64_OP_IMM) {
        return;
    }
    if (!state->dart_sp_known) return;
    const int64_t amount = arm64.operands[2].imm;
    if (instruction.id == ARM64_INS_ADD) {
        state->dart_sp_to_entry += static_cast<int32_t>(amount);
    } else if (instruction.id == ARM64_INS_SUB) {
        state->dart_sp_to_entry -= static_cast<int32_t>(amount);
    }
}

void ApplyDartStackWriteback(const cs_insn& instruction, State* state) {
    if (!instruction.detail->writeback || !state->dart_sp_known) return;
    const auto& arm64 = instruction.detail->arm64;
    const cs_aarch64_op* memory = nullptr;
    for (uint8_t i = 0; i < arm64.op_count; ++i) {
        if (arm64.operands[i].type == ARM64_OP_MEM && arm64.operands[i].mem.base == ARM64_REG_X15) {
            memory = &arm64.operands[i];
            break;
        }
    }
    if (memory == nullptr) return;

    int64_t delta = memory->mem.disp;
    if (arm64.post_index) {
        delta = 0;
        for (uint8_t i = 0; i < arm64.op_count; ++i) {
            if (arm64.operands[i].type == ARM64_OP_IMM) {
                delta = arm64.operands[i].imm;
            }
        }
    }
    state->dart_sp_to_entry += static_cast<int32_t>(delta);
}

void UpdateFramePointer(const cs_insn& instruction, State* state) {
    const auto& arm64 = instruction.detail->arm64;
    if (arm64.op_count < 2 || arm64.operands[0].type != ARM64_OP_REG ||
        arm64.operands[0].reg != AARCH64_REG_FP || arm64.operands[1].type != ARM64_OP_REG) {
        return;
    }
    if ((instruction.id == ARM64_INS_MOV || instruction.id == ARM64_INS_ORR) &&
        arm64.operands[1].reg == ARM64_REG_X15 && state->dart_sp_known) {
        state->fp_known = true;
        state->fp_to_entry = state->dart_sp_to_entry;
    } else if (instruction.id == ARM64_INS_ADD && arm64.op_count >= 3 &&
               arm64.operands[1].reg == ARM64_REG_X15 && arm64.operands[2].type == ARM64_OP_IMM &&
               state->dart_sp_known) {
        state->fp_known = true;
        state->fp_to_entry = state->dart_sp_to_entry + static_cast<int32_t>(arm64.operands[2].imm);
    }
}

void ApplyDirectCallClobber(State* state) {
    // Dart-generated calls follow the platform call preservation contract for
    // the registers relevant to this offline proof. Forget every volatile GP
    // value and every volatile FPU value; preserving a stale provenance across
    // BL would be a false proof, while dropping too much only yields Unknown.
    for (uint8_t reg = 0; reg <= 17; ++reg) state->gp[reg] = {};
    state->gp[30] = {};
    for (uint8_t reg = 0; reg <= 7; ++reg) state->fpu[reg] = {};
    for (uint8_t reg = 16; reg < kFpuCount; ++reg) state->fpu[reg] = {};
}

void AddObservation(AnalysisResult* result, const Source& source, ParameterEvidence evidence,
                    uint64_t pc) {
    if (!source.valid || source.location.kind == LocationKind::kUnknown) return;
    for (size_t i = 0; i < result->observation_count; ++i) {
        auto& observation = result->observations[i];
        if (observation.location.kind != source.location.kind ||
            observation.location.register_index != source.location.register_index ||
            observation.location.stack_offset != source.location.stack_offset) {
            continue;
        }
        if (observation.representation != evidence) {
            observation.representation = ParameterEvidence::kUnknown;
            observation.proof = abi::DartAbiProofState::kConflicting;
        }
        observation.first_observation_pc =
            observation.first_observation_pc == 0 ? pc : observation.first_observation_pc;
        ++observation.observation_count;
        if (source.location.kind == LocationKind::kFpuRegister &&
            source.location.register_index < result->fpu_arguments.size()) {
            result->fpu_arguments[source.location.register_index] = observation.representation;
        }
        return;
    }
    if (result->observation_count >= result->observations.size()) return;
    auto& observation = result->observations[result->observation_count++];
    observation.location = source.location;
    observation.representation = evidence;
    observation.source = abi::DartAbiEvidenceSource::kAotCodeAnalysis;
    observation.proof = abi::DartAbiProofState::kProven;
    observation.first_observation_pc = pc;
    observation.observation_count = 1;
    if (source.location.kind == LocationKind::kFpuRegister &&
        source.location.register_index < result->fpu_arguments.size()) {
        result->fpu_arguments[source.location.register_index] = evidence;
    }
}

Decoded Decode(csh handle, const uint8_t* code, size_t size, uint64_t address) {
    Decoded decoded;
    cs_insn* raw = nullptr;
    const size_t count = cs_disasm(handle, code, size, address, 0, &raw);
    decoded.instructions = raw;
    decoded.instruction_count = count;
    if (decoded.instruction_count == 0) return decoded;

    std::vector<uint64_t> starts{decoded.instructions[0].address};
    for (size_t i = 0; i < decoded.instruction_count; ++i) {
        const auto& instruction = decoded.instructions[i];
        if (IsTerminator(instruction) && i + 1 < decoded.instruction_count) {
            starts.push_back(decoded.instructions[i + 1].address);
        }
        if (IsDirectBranch(instruction) && instruction.detail->arm64.op_count > 0 &&
            instruction.detail->arm64.operands[0].type == ARM64_OP_IMM) {
            starts.push_back(static_cast<uint64_t>(instruction.detail->arm64.operands[0].imm));
        }
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    for (uint64_t start : starts) {
        size_t index = 0;
        while (index < decoded.instruction_count && decoded.instructions[index].address != start) {
            ++index;
        }
        if (index < decoded.instruction_count) {
            decoded.block_starts.push_back(index);
        }
    }
    std::sort(decoded.block_starts.begin(), decoded.block_starts.end());
    for (size_t block = 0; block < decoded.block_starts.size(); ++block) {
        const size_t begin = decoded.block_starts[block];
        const size_t end = block + 1 < decoded.block_starts.size() ? decoded.block_starts[block + 1]
                                                                   : decoded.instruction_count;
        std::vector<size_t> successors;
        if (end > begin) {
            const auto& last = decoded.instructions[end - 1];
            if (IsDirectBranch(last) && last.detail->arm64.op_count > 0 &&
                last.detail->arm64.operands[0].type == ARM64_OP_IMM) {
                const uint64_t target = static_cast<uint64_t>(last.detail->arm64.operands[0].imm);
                for (size_t candidate = 0; candidate < decoded.block_starts.size(); ++candidate) {
                    if (decoded.instructions[decoded.block_starts[candidate]].address == target) {
                        successors.push_back(candidate);
                    }
                }
            }
            if (IsConditionalBranch(last) && block + 1 < decoded.block_starts.size()) {
                successors.push_back(block + 1);
            } else if (!IsTerminator(last) && block + 1 < decoded.block_starts.size()) {
                successors.push_back(block + 1);
            }
        }
        decoded.successors.push_back(std::move(successors));
    }
    return decoded;
}

void InitializeEntryState(State* state) {
    // Dart ARM64's current direct GP argument sequence is R1,R2,R3,R5,R6,R7.
    for (uint8_t reg : {1, 2, 3, 5, 6, 7}) state->gp[reg] = GpSource(reg);
    for (uint8_t reg = 0; reg < 6; ++reg) state->fpu[reg] = FpuSource(reg);
    for (int32_t offset = 0; offset <= kStackMax; offset += 8) {
        const size_t index = StackIndex(offset);
        if (index < kStackCount) state->stack[index] = StackSource(offset);
    }
}

void ProcessInstruction(csh handle, const cs_insn& instruction, State* state,
                        AnalysisResult* result) {
    const auto& arm64 = instruction.detail->arm64;
    const uint64_t pc = instruction.address;
    Source preserved_destination;
    aarch64_reg preserved_register = AARCH64_REG_INVALID;

    for (uint8_t operand = 0; operand < arm64.op_count; ++operand) {
        if (arm64.operands[operand].type == ARM64_OP_MEM &&
            arm64.operands[operand].mem.base == ARM64_REG_X4) {
            result->uses_arguments_descriptor = true;
        }
    }

    if (instruction.id == ARM64_INS_ADD && arm64.op_count >= 3 &&
        arm64.operands[0].type == ARM64_OP_REG && arm64.operands[1].type == ARM64_OP_REG &&
        arm64.operands[2].type == ARM64_OP_IMM && arm64.operands[2].imm == 0) {
        preserved_register = arm64.operands[0].reg;
        preserved_destination = ReadSource(*state, arm64.operands[1].reg);
    } else if ((instruction.id == ARM64_INS_MOV || instruction.id == ARM64_INS_ORR) &&
               arm64.op_count >= 2 && arm64.operands[0].type == ARM64_OP_REG &&
               arm64.operands[1].type == ARM64_OP_REG) {
        const auto source_reg = arm64.operands[1].reg;
        preserved_register = arm64.operands[0].reg;
        preserved_destination =
            IsZeroRegister(source_reg) ? Source{} : ReadSource(*state, source_reg);
    } else if (instruction.id == ARM64_INS_FMOV && arm64.op_count == 2 &&
               arm64.operands[0].type == ARM64_OP_REG && arm64.operands[1].type == ARM64_OP_REG) {
        preserved_register = arm64.operands[0].reg;
        preserved_destination = ReadSource(*state, arm64.operands[1].reg);
    } else if (instruction.id == ARM64_INS_LDR || instruction.id == ARM64_INS_LDUR) {
        if (arm64.operands[0].type == ARM64_OP_REG && arm64.operands[1].type == ARM64_OP_MEM) {
            Source source = ReadMemorySource(*state, arm64.operands[1]);
            preserved_register = arm64.operands[0].reg;
            preserved_destination = source;
        }
    } else if (instruction.id == ARM64_INS_LDP && arm64.op_count >= 3 &&
               arm64.operands[0].type == ARM64_OP_REG && arm64.operands[1].type == ARM64_OP_REG &&
               arm64.operands[2].type == ARM64_OP_MEM) {
        int32_t offset = 0;
        if (ResolveEntryStackOffset(*state, arm64.operands[2], &offset)) {
            const size_t first_index = StackIndex(offset);
            const size_t second_index = StackIndex(offset + 8);
            WriteSource(state, arm64.operands[0].reg,
                        first_index < kStackCount ? state->stack[first_index] : Source{});
            WriteSource(state, arm64.operands[1].reg,
                        second_index < kStackCount ? state->stack[second_index] : Source{});
        }
    } else if (instruction.id == ARM64_INS_STR || instruction.id == ARM64_INS_STUR) {
        if (arm64.operands[0].type == ARM64_OP_REG && arm64.operands[1].type == ARM64_OP_MEM) {
            WriteMemorySource(state, arm64.operands[1], ReadSource(*state, arm64.operands[0].reg));
        }
    } else if (instruction.id == ARM64_INS_STP && arm64.op_count >= 3 &&
               arm64.operands[0].type == ARM64_OP_REG && arm64.operands[1].type == ARM64_OP_REG &&
               arm64.operands[2].type == ARM64_OP_MEM) {
        int32_t offset = 0;
        if (ResolveEntryStackOffset(*state, arm64.operands[2], &offset)) {
            const size_t first_index = StackIndex(offset);
            const size_t second_index = StackIndex(offset + 8);
            if (first_index < kStackCount) {
                state->stack[first_index] = ReadSource(*state, arm64.operands[0].reg);
            }
            if (second_index < kStackCount) {
                state->stack[second_index] = ReadSource(*state, arm64.operands[1].reg);
            }
        }
    }

    cs_regs regs_read{};
    cs_regs regs_write{};
    uint8_t read_count = 0;
    uint8_t write_count = 0;
    cs_regs_access(handle, &instruction, regs_read, &read_count, regs_write, &write_count);

    if (IsDoubleOperation(instruction.id)) {
        for (uint8_t operand = 0; operand < arm64.op_count; ++operand) {
            const auto& value = arm64.operands[operand];
            if (value.type != ARM64_OP_REG || !IsFpuRegister(value.reg)) continue;
            const Source source = ReadSource(*state, value.reg);
            if (source.location.kind == LocationKind::kFpuRegister ||
                source.location.kind == LocationKind::kEntryStack) {
                AddObservation(result, source, ParameterEvidence::kUnboxedDouble, pc);
            }
        }
    }
    if (IsExplicitIntegerOperation(instruction.id)) {
        for (uint8_t operand = 1; operand < arm64.op_count; ++operand) {
            const auto& value = arm64.operands[operand];
            if (value.type != ARM64_OP_REG || !IsGpRegister(value.reg)) continue;
            AddObservation(result, ReadSource(*state, value.reg), ParameterEvidence::kUnboxedInt64,
                           pc);
        }
    }

    if (instruction.id == ARM64_INS_RET) {
        result->reached_return = true;
    }

    for (uint8_t reg = 0; reg < write_count; ++reg) {
        InvalidateRegister(state, static_cast<aarch64_reg>(regs_write[reg]));
    }
    if (preserved_register != AARCH64_REG_INVALID) {
        WriteSource(state, preserved_register, preserved_destination);
    }
    UpdateDartStackPointer(instruction, state);
    UpdateFramePointer(instruction, state);
    ApplyDartStackWriteback(instruction, state);
    if (instruction.id == ARM64_INS_BL) ApplyDirectCallClobber(state);
}

}  // namespace

AnalysisResult AnalyzeArm64Entry(const uint8_t* code, size_t size, uint64_t address) {
    AnalysisResult result;
    result.fpu_arguments.fill(ParameterEvidence::kUnknown);
    if (code == nullptr || size == 0) return result;

    csh handle = 0;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK) return result;
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    Decoded decoded = Decode(handle, code, size, address);
    result.decoded_instructions = decoded.instruction_count;
    result.basic_block_count = decoded.block_starts.size();
    if (decoded.instruction_count == 0) {
        cs_free(decoded.instructions, decoded.instruction_count);
        cs_close(&handle);
        return result;
    }

    std::vector<State> entry_states(decoded.block_starts.size());
    std::vector<bool> has_state(decoded.block_starts.size(), false);
    InitializeEntryState(&entry_states[0]);
    has_state[0] = true;
    std::vector<size_t> worklist{0};
    while (!worklist.empty()) {
        const size_t block = worklist.back();
        worklist.pop_back();
        State state = entry_states[block];
        const size_t begin = decoded.block_starts[block];
        const size_t end = block + 1 < decoded.block_starts.size() ? decoded.block_starts[block + 1]
                                                                   : decoded.instruction_count;
        for (size_t index = begin; index < end; ++index) {
            const auto& instruction = decoded.instructions[index];
            if (IsTerminator(instruction) && instruction.id != ARM64_INS_RET) {
                result.stopped_at_control_flow = true;
            }
            if ((instruction.id == ARM64_INS_BR || instruction.id == ARM64_INS_BLR) &&
                instruction.id != ARM64_INS_RET) {
                result.has_unknown_control_flow = true;
            }
            ProcessInstruction(handle, instruction, &state, &result);
        }
        for (size_t successor : decoded.successors[block]) {
            if (!has_state[successor]) {
                entry_states[successor] = state;
                has_state[successor] = true;
                worklist.push_back(successor);
            } else if (MergeState(&entry_states[successor], state)) {
                worklist.push_back(successor);
            }
        }
    }
    if (result.has_unknown_control_flow) {
        for (size_t i = 0; i < result.observation_count; ++i) {
            result.observations[i].representation = ParameterEvidence::kUnknown;
            result.observations[i].proof = abi::DartAbiProofState::kUnknown;
        }
        result.fpu_arguments.fill(ParameterEvidence::kUnknown);
        result.return_evidence = ParameterEvidence::kUnknown;
    }
    cs_free(decoded.instructions, decoded.instruction_count);
    cs_close(&handle);
    return result;
}

const AbiObservation* FindObservation(const AnalysisResult& result, const AbiLocation& location) {
    for (size_t i = 0; i < result.observation_count; ++i) {
        const auto& candidate = result.observations[i].location;
        if (candidate.kind == location.kind &&
            candidate.register_index == location.register_index &&
            candidate.stack_offset == location.stack_offset) {
            return &result.observations[i];
        }
    }
    return nullptr;
}

}  // namespace dartplant::aot
