// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace atlas {
namespace {

/// Which thread was marked as main, and whether one was.
///
/// Atomics rather than a mutex because reading sits on paths that run every frame in a
/// debug build. Held in a function-local static rather than at namespace scope so that
/// there is no mutable global and no dependence on initialisation order between
/// translation units.
struct MainThread {
    std::atomic<bool> marked{false};
    std::atomic<std::thread::id> id;
};

[[nodiscard]] MainThread& main_thread() noexcept {
    static MainThread instance;
    return instance;
}

}  // namespace

void mark_main_thread() noexcept {
    auto& state = main_thread();

    // compare_exchange takes the expected value by reference and writes to it, so it cannot
    // be const however much a linter would like it to be.
    bool expected = false;
    if (state.marked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        state.id.store(std::this_thread::get_id(), std::memory_order_release);
    }
}

bool is_main_thread() noexcept {
    const auto& state = main_thread();
    if (!state.marked.load(std::memory_order_acquire)) {
        return false;
    }
    return state.id.load(std::memory_order_acquire) == std::this_thread::get_id();
}

namespace detail {

void assertion_failed(std::string_view expression, std::string_view message,
                      std::source_location where) {
    // Write to stderr directly as well as through the log. An assertion means the program
    // is in an unknown state, and the logging path may be part of what is broken.
    if (message.empty()) {
        std::fprintf(stderr, "\nAtlas assertion failed: %.*s\n  at %s:%u in %s\n\n",
                     static_cast<int>(expression.size()), expression.data(), where.file_name(),
                     where.line(), where.function_name());
    } else {
        std::fprintf(stderr, "\nAtlas assertion failed: %.*s\n  %.*s\n  at %s:%u in %s\n\n",
                     static_cast<int>(expression.size()), expression.data(),
                     static_cast<int>(message.size()), message.data(), where.file_name(),
                     where.line(), where.function_name());
    }
    std::fflush(stderr);

    if (message.empty()) {
        ATLAS_LOG(log::category::kCore, log::Severity::Fatal, "assertion failed: {}", expression);
    } else {
        ATLAS_LOG(log::category::kCore, log::Severity::Fatal, "assertion failed: {} ({})",
                  expression, message);
    }

    // abort() rather than exit(): a core dump is the most useful artifact here, and no
    // destructor should run while the program's invariants are known to be violated.
    std::abort();
}

}  // namespace detail
}  // namespace atlas
