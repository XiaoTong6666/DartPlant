// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dartplant/live_vm.h"

#if defined(__linux__)
#include <dirent.h>
#include <ucontext.h>
#endif

#include "core/internal.h"

namespace dartplant {
namespace {

constexpr uint32_t kDefaultMaxRounds = 64;
constexpr uint32_t kDefaultPerThreadTimeoutUs = 5000;
constexpr uint32_t kDefaultRoundSleepUs = 1307;
[[maybe_unused]] constexpr uint32_t kWaitQuantumUs = 50;
[[maybe_unused]] constexpr uint32_t kUiActivityPolls = 32;
[[maybe_unused]] constexpr uint32_t kUiActivityPollSpacingUs = 250;
std::mutex g_bootstrap_mutex;

#if defined(__linux__) && defined(__aarch64__)

struct CapturedArm64Context {
    uint64_t pc = 0;
    uint64_t sp = 0;
    uint64_t x22 = 0;
    uint64_t x26 = 0;
    uint64_t x27 = 0;
    uint64_t x28 = 0;
};

volatile sig_atomic_t g_capture_active = 0;
volatile sig_atomic_t g_capture_ready = 0;
volatile sig_atomic_t g_expected_tid = 0;
CapturedArm64Context g_capture{};

void CaptureSignalHandler(int, siginfo_t*, void* raw_context) {
    if (__atomic_load_n(&g_capture_active, __ATOMIC_ACQUIRE) == 0 || raw_context == nullptr) return;
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    if (tid != static_cast<pid_t>(__atomic_load_n(&g_expected_tid, __ATOMIC_RELAXED))) return;

    const auto* context = static_cast<const ucontext_t*>(raw_context);
    __atomic_store_n(&g_capture.pc, context->uc_mcontext.pc, __ATOMIC_RELAXED);
    __atomic_store_n(&g_capture.sp, context->uc_mcontext.sp, __ATOMIC_RELAXED);
    __atomic_store_n(&g_capture.x22, context->uc_mcontext.regs[22], __ATOMIC_RELAXED);
    __atomic_store_n(&g_capture.x26, context->uc_mcontext.regs[26], __ATOMIC_RELAXED);
    __atomic_store_n(&g_capture.x27, context->uc_mcontext.regs[27], __ATOMIC_RELAXED);
    __atomic_store_n(&g_capture.x28, context->uc_mcontext.regs[28], __ATOMIC_RELAXED);
    __atomic_store_n(&g_capture_ready, 1, __ATOMIC_RELEASE);
}

class ScopedSamplingSignal final {
public:
    bool Install() {
        // Use signals whose Linux default disposition is ignore. If a sampled
        // thread blocks the signal and delivery is delayed past our timeout,
        // restoring SIG_DFL must not turn the pending sample into a future
        // process-killing event. We only borrow a signal while its current
        // disposition is still SIG_DFL, so applications that own either signal
        // are left untouched.
        constexpr int kCandidates[] = {SIGWINCH, SIGURG};
        for (const int candidate : kCandidates) {
            struct sigaction current{};
            if (sigaction(candidate, nullptr, &current) != 0 || current.sa_handler != SIG_DFL) {
                continue;
            }

            struct sigaction action{};
            sigemptyset(&action.sa_mask);
            action.sa_sigaction = CaptureSignalHandler;
            action.sa_flags = SA_SIGINFO | SA_RESTART;
            if (sigaction(candidate, &action, &old_action_) == 0) {
                signal_ = candidate;
                installed_ = true;
                return true;
            }
        }
        return false;
    }

    ~ScopedSamplingSignal() {
        __atomic_store_n(&g_capture_active, 0, __ATOMIC_RELEASE);
        if (installed_) sigaction(signal_, &old_action_, nullptr);
    }

    int signal_number() const { return signal_; }

private:
    bool installed_ = false;
    int signal_ = 0;
    struct sigaction old_action_{};
};

struct ThreadSampleTarget {
    pid_t tid = 0;
    int priority = 0;
};

int ThreadPriority(pid_t tid) {
    char path[64] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    std::ifstream stream(path);
    std::string name;
    std::getline(stream, name);
    if (name.size() >= 3 && name.ends_with(".ui")) return 3;
    if (name.find("dart") != std::string::npos) return 2;
    if (name.find("flutter") != std::string::npos) return 1;
    return 0;
}

char ThreadState(pid_t tid) {
    char path[64] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    std::ifstream stream(path);
    std::string stat;
    std::getline(stream, stat);
    const size_t comm_end = stat.rfind(')');
    if (comm_end == std::string::npos || comm_end + 2 >= stat.size()) return '\0';
    return stat[comm_end + 2];
}

std::vector<ThreadSampleTarget> ListProcessThreads() {
    std::vector<ThreadSampleTarget> targets;
    DIR* directory = opendir("/proc/self/task");
    if (directory == nullptr) return targets;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (end != nullptr && *end == '\0' && value > 0) {
            const pid_t tid = static_cast<pid_t>(value);
            targets.push_back({tid, ThreadPriority(tid)});
        }
    }
    closedir(directory);

    const pid_t self_tid = static_cast<pid_t>(syscall(SYS_gettid));
    std::stable_sort(targets.begin(), targets.end(),
                     [self_tid](const auto& left, const auto& right) {
                         const bool left_self = left.tid == self_tid;
                         const bool right_self = right.tid == self_tid;
                         if (left_self != right_self) return !left_self;
                         if (left.priority != right.priority) return left.priority > right.priority;
                         return left.tid < right.tid;
                     });
    return targets;
}

enum class CaptureResult {
    kCaptured,
    kSignalFailed,
    kTimedOut,
};

CaptureResult CaptureThread(pid_t tid, int signal_number, uint32_t timeout_us,
                            DartPlantLiveVmArm64Registers* out_registers) {
    if (out_registers == nullptr) return CaptureResult::kSignalFailed;
    __atomic_store_n(&g_expected_tid, static_cast<sig_atomic_t>(tid), __ATOMIC_RELAXED);
    __atomic_store_n(&g_capture_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_capture_active, 1, __ATOMIC_RELEASE);
    if (syscall(SYS_tgkill, getpid(), tid, signal_number) != 0) {
        __atomic_store_n(&g_capture_active, 0, __ATOMIC_RELEASE);
        return CaptureResult::kSignalFailed;
    }

    uint32_t waited_us = 0;
    while (__atomic_load_n(&g_capture_ready, __ATOMIC_ACQUIRE) == 0 && waited_us < timeout_us) {
        std::this_thread::sleep_for(std::chrono::microseconds(kWaitQuantumUs));
        waited_us += kWaitQuantumUs;
    }
    __atomic_store_n(&g_capture_active, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&g_capture_ready, __ATOMIC_ACQUIRE) == 0) {
        return CaptureResult::kTimedOut;
    }

    DartPlantLiveVmArm64Registers registers{};
    registers.struct_size = sizeof(registers);
    registers.tid = static_cast<uint32_t>(tid);
    registers.pc = __atomic_load_n(&g_capture.pc, __ATOMIC_RELAXED);
    registers.sp = __atomic_load_n(&g_capture.sp, __ATOMIC_RELAXED);
    registers.thr = __atomic_load_n(&g_capture.x26, __ATOMIC_RELAXED);
    registers.pp = __atomic_load_n(&g_capture.x27, __ATOMIC_RELAXED);
    registers.heap_bits = __atomic_load_n(&g_capture.x28, __ATOMIC_RELAXED);
    registers.null_value = __atomic_load_n(&g_capture.x22, __ATOMIC_RELAXED);
    *out_registers = registers;
    return CaptureResult::kCaptured;
}

#endif

[[maybe_unused]] DartPlantLiveVmBootstrapOptions NormalizeOptions(
    const DartPlantLiveVmBootstrapOptions* source) {
    DartPlantLiveVmBootstrapOptions options{};
    options.struct_size = sizeof(options);
    options.max_rounds = kDefaultMaxRounds;
    options.per_thread_timeout_us = kDefaultPerThreadTimeoutUs;
    options.round_sleep_us = kDefaultRoundSleepUs;
    if (source != nullptr && source->struct_size >= sizeof(DartPlantLiveVmBootstrapOptions)) {
        if (source->max_rounds != 0) options.max_rounds = source->max_rounds;
        if (source->per_thread_timeout_us != 0) {
            options.per_thread_timeout_us = source->per_thread_timeout_us;
        }
        options.round_sleep_us = source->round_sleep_us;
    }
    return options;
}

}  // namespace
}  // namespace dartplant

extern "C" DartPlantStatus dartplant_live_vm_bootstrap_process(
    const DartPlantFlutterSnapshotInfo* snapshot, const DartPlantLiveVmBootstrapOptions* options,
    DartPlantLiveVmContext* out_context, DartPlantLiveVmBootstrapInfo* out_info) {
    if (snapshot == nullptr || out_context == nullptr ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_context->struct_size < sizeof(DartPlantLiveVmContext) ||
        (out_info != nullptr && out_info->struct_size < sizeof(DartPlantLiveVmBootstrapInfo)) ||
        (options != nullptr && options->struct_size < sizeof(DartPlantLiveVmBootstrapOptions))) {
        dartplant::SetLastError("live VM cold-bootstrap arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    std::lock_guard bootstrap_lock(dartplant::g_bootstrap_mutex);
#if !defined(__linux__) || !defined(__aarch64__)
    (void) options;
    (void) out_info;
    dartplant::SetLastError("live VM cold bootstrap currently requires Linux/Android ARM64");
    return DARTPLANT_UNSUPPORTED_ABI;
#else
    DartPlantLiveVmProfile selected_profile{};
    selected_profile.struct_size = sizeof(selected_profile);
    DartPlantStatus status = dartplant_live_vm_select_profile(snapshot, &selected_profile);
    if (status != DARTPLANT_OK) return status;

    const DartPlantLiveVmBootstrapOptions effective = dartplant::NormalizeOptions(options);
    DartPlantLiveVmBootstrapInfo info{};
    info.struct_size = sizeof(info);

    dartplant::ScopedSamplingSignal sampling_signal;
    if (!sampling_signal.Install()) {
        dartplant::SetLastError(
            "no unused ignore-by-default signal is available for live VM sampling");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    info.signal_number = static_cast<uint32_t>(sampling_signal.signal_number());

    for (uint32_t round = 0; round < effective.max_rounds; ++round) {
        info.rounds = round + 1;
        const std::vector<dartplant::ThreadSampleTarget> targets = dartplant::ListProcessThreads();
        for (const auto& target : targets) {
            const uint32_t activity_polls = target.priority >= 3 ? dartplant::kUiActivityPolls : 1;
            for (uint32_t poll = 0; poll < activity_polls; ++poll) {
                const bool is_ui_target = target.priority >= 3;
                if (is_ui_target) ++info.ui_activity_polls;
                const char thread_state = dartplant::ThreadState(target.tid);
                if (!is_ui_target && thread_state != 'R') continue;
                if (is_ui_target && thread_state == 'R') ++info.ui_activity_hits;

                ++info.sampled_threads;
                DartPlantLiveVmArm64Registers registers{};
                const dartplant::CaptureResult capture =
                    dartplant::CaptureThread(target.tid, sampling_signal.signal_number(),
                                             effective.per_thread_timeout_us, &registers);
                if (capture == dartplant::CaptureResult::kSignalFailed) {
                    ++info.signal_send_failures;
                    if (is_ui_target && poll + 1 < activity_polls) {
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(dartplant::kUiActivityPollSpacingUs));
                    }
                    continue;
                }
                if (capture == dartplant::CaptureResult::kTimedOut) {
                    ++info.sample_timeouts;
                    if (is_ui_target && poll + 1 < activity_polls) {
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(dartplant::kUiActivityPollSpacingUs));
                    }
                    continue;
                }
                ++info.captured_contexts;
                if (registers.thr != 0) {
                    ++info.nonzero_thr_samples;
                    info.last_candidate_tid = registers.tid;
                    info.last_candidate_pc = registers.pc;
                    info.last_candidate_thr = registers.thr;
                    info.last_candidate_pp = registers.pp;
                    info.last_candidate_heap_bits = registers.heap_bits;
                    info.last_candidate_null = registers.null_value;
                }

                const bool dart_pc = snapshot->isolate_instructions_size != 0 &&
                                     registers.pc >= snapshot->isolate_instructions_runtime &&
                                     registers.pc - snapshot->isolate_instructions_runtime <
                                         snapshot->isolate_instructions_size;
                if (dart_pc) ++info.dart_instruction_samples;

                DartPlantLiveVmContext candidate{};
                candidate.struct_size = sizeof(candidate);
                status = dartplant_live_vm_context_from_arm64_registers(snapshot, &registers,
                                                                        &candidate);
                if (status != DARTPLANT_OK) {
                    if (registers.thr != 0) {
                        std::snprintf(info.last_validation_error,
                                      sizeof(info.last_validation_error), "%s",
                                      dartplant_last_error());
                    }
                    if (is_ui_target && poll + 1 < activity_polls) {
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(dartplant::kUiActivityPollSpacingUs));
                    }
                    continue;
                }

                ++info.validated_candidates;
                info.selected_tid = registers.tid;
                info.selected_pc = registers.pc;
                info.last_validation_error[0] = '\0';
                *out_context = candidate;
                if (out_info != nullptr) *out_info = info;
                dartplant::ClearLastError();
                return DARTPLANT_OK;
            }
        }
        if (effective.round_sleep_us != 0 && round + 1 < effective.max_rounds) {
            std::this_thread::sleep_for(std::chrono::microseconds(effective.round_sleep_us));
        }
    }

    if (out_info != nullptr) *out_info = info;
    dartplant::SetLastError(
        "cold live VM bootstrap sampled threads but found no validated Dart context");
    return DARTPLANT_RUNTIME_NOT_READY;
#endif
}
