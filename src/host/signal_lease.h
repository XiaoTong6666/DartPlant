// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_HOST_SIGNAL_LEASE_H_
#define DARTPLANT_HOST_SIGNAL_LEASE_H_

#include <signal.h>

#include <span>

namespace dartplant {

// Temporarily borrows the first signal whose disposition is still SIG_DFL.
// Installation re-checks the action returned by sigaction() and backs out if a
// host claimed the signal between the initial probe and install. Destruction
// restores the previous action only while DartPlant's own handler is still
// installed. A process-wide external sigaction() racing these operations cannot
// be made atomic with this lease, so callers must not treat it as ownership.
class ScopedSignalLease final {
public:
    ScopedSignalLease() = default;
    ScopedSignalLease(const ScopedSignalLease&) = delete;
    ScopedSignalLease& operator=(const ScopedSignalLease&) = delete;
    ~ScopedSignalLease();

    bool Install(std::span<const int> candidates, void (*handler)(int, siginfo_t*, void*),
                 int flags);
    int signal_number() const { return signal_; }
    bool installed() const { return installed_; }

private:
    bool installed_ = false;
    int signal_ = 0;
    void (*handler_)(int, siginfo_t*, void*) = nullptr;
    struct sigaction old_action_{};
};

}  // namespace dartplant

#endif  // DARTPLANT_HOST_SIGNAL_LEASE_H_
