// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "host/signal_lease.h"

#include <signal.h>

#include <array>

#include "test_runner.h"

namespace {

void ExistingHandler(int) {}

void LeaseHandler(int, siginfo_t *, void *) {}

struct SignalActionRestore {
    int signal = 0;
    struct sigaction action{};
    ~SignalActionRestore() {
        if (signal != 0) sigaction(signal, &action, nullptr);
    }
};

}  // namespace

TEST_CASE(SignalLeaseSkipsHostOwnedSignalAndRestoresBorrowedDisposition) {
    struct sigaction original_winch{};
    struct sigaction original_urg{};
    EXPECT_EQ(0, sigaction(SIGWINCH, nullptr, &original_winch));
    EXPECT_EQ(0, sigaction(SIGURG, nullptr, &original_urg));
    SignalActionRestore restore_winch{SIGWINCH, original_winch};
    SignalActionRestore restore_urg{SIGURG, original_urg};

    struct sigaction occupied{};
    sigemptyset(&occupied.sa_mask);
    occupied.sa_handler = ExistingHandler;
    EXPECT_EQ(0, sigaction(SIGWINCH, &occupied, nullptr));
    struct sigaction default_action{};
    sigemptyset(&default_action.sa_mask);
    default_action.sa_handler = SIG_DFL;
    EXPECT_EQ(0, sigaction(SIGURG, &default_action, nullptr));

    {
        constexpr std::array candidates = {SIGWINCH, SIGURG};
        dartplant::ScopedSignalLease lease;
        EXPECT_TRUE(lease.Install(candidates, LeaseHandler, SA_RESTART));
        EXPECT_EQ(SIGURG, lease.signal_number());

        struct sigaction current_winch{};
        struct sigaction current_urg{};
        EXPECT_EQ(0, sigaction(SIGWINCH, nullptr, &current_winch));
        EXPECT_EQ(0, sigaction(SIGURG, nullptr, &current_urg));
        EXPECT_TRUE(current_winch.sa_handler == ExistingHandler);
        EXPECT_TRUE((current_urg.sa_flags & SA_SIGINFO) != 0);
    }

    struct sigaction after_winch{};
    struct sigaction after_urg{};
    EXPECT_EQ(0, sigaction(SIGWINCH, nullptr, &after_winch));
    EXPECT_EQ(0, sigaction(SIGURG, nullptr, &after_urg));
    EXPECT_TRUE(after_winch.sa_handler == ExistingHandler);
    EXPECT_TRUE(after_urg.sa_handler == SIG_DFL);
}

TEST_CASE(SignalLeaseFailsClosedWhenAllCandidatesAreOwned) {
    struct sigaction original_winch{};
    struct sigaction original_urg{};
    EXPECT_EQ(0, sigaction(SIGWINCH, nullptr, &original_winch));
    EXPECT_EQ(0, sigaction(SIGURG, nullptr, &original_urg));
    SignalActionRestore restore_winch{SIGWINCH, original_winch};
    SignalActionRestore restore_urg{SIGURG, original_urg};

    struct sigaction occupied{};
    sigemptyset(&occupied.sa_mask);
    occupied.sa_handler = ExistingHandler;
    EXPECT_EQ(0, sigaction(SIGWINCH, &occupied, nullptr));
    EXPECT_EQ(0, sigaction(SIGURG, &occupied, nullptr));

    constexpr std::array candidates = {SIGWINCH, SIGURG};
    dartplant::ScopedSignalLease lease;
    EXPECT_FALSE(lease.Install(candidates, LeaseHandler, SA_RESTART));
    EXPECT_EQ(0, lease.signal_number());
}

TEST_CASE(SignalLeaseDoesNotOverwriteHostHandlerInstalledDuringLease) {
    struct sigaction original_urg{};
    EXPECT_EQ(0, sigaction(SIGURG, nullptr, &original_urg));
    SignalActionRestore restore_urg{SIGURG, original_urg};

    struct sigaction default_action{};
    sigemptyset(&default_action.sa_mask);
    default_action.sa_handler = SIG_DFL;
    EXPECT_EQ(0, sigaction(SIGURG, &default_action, nullptr));
    {
        constexpr std::array candidates = {SIGURG};
        dartplant::ScopedSignalLease lease;
        EXPECT_TRUE(lease.Install(candidates, LeaseHandler, SA_RESTART));

        struct sigaction replacement{};
        sigemptyset(&replacement.sa_mask);
        replacement.sa_handler = ExistingHandler;
        EXPECT_EQ(0, sigaction(SIGURG, &replacement, nullptr));
    }

    struct sigaction after{};
    EXPECT_EQ(0, sigaction(SIGURG, nullptr, &after));
    EXPECT_TRUE(after.sa_handler == ExistingHandler);
}
