// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "host/signal_lease.h"

namespace dartplant {

bool ScopedSignalLease::Install(std::span<const int> candidates,
                                void (*handler)(int, siginfo_t *, void *), int flags) {
    if (installed_ || handler == nullptr) return false;
    for (const int candidate : candidates) {
        struct sigaction current{};
        if (candidate <= 0 || sigaction(candidate, nullptr, &current) != 0 ||
            current.sa_handler != SIG_DFL) {
            continue;
        }

        struct sigaction action{};
        sigemptyset(&action.sa_mask);
        action.sa_sigaction = handler;
        action.sa_flags = flags | SA_SIGINFO;
        if (sigaction(candidate, &action, &old_action_) == 0) {
            // The disposition can change between the probe above and this
            // sigaction(). The returned old action is authoritative; if a host
            // claimed the signal in that window, restore it immediately and
            // do not treat the signal as leased.
            if (old_action_.sa_handler != SIG_DFL) {
                sigaction(candidate, &old_action_, nullptr);
                continue;
            }
            signal_ = candidate;
            handler_ = handler;
            installed_ = true;
            return true;
        }
    }
    return false;
}

ScopedSignalLease::~ScopedSignalLease() {
    if (!installed_) return;
    struct sigaction current{};
    if (sigaction(signal_, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) != 0 &&
        current.sa_sigaction == handler_) {
        sigaction(signal_, &old_action_, nullptr);
    }
}

}  // namespace dartplant
