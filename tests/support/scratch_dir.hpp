// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A temporary directory that belongs to one test and to nothing else.
///
/// CTest runs every Catch2 case as its own process, many at once, and two build trees or two
/// worktrees can run the same case at the same moment. A name drawn from a counter inside the
/// process is therefore the same name in every process: cases running side by side were each
/// handed `atlas-assets-0`, and whichever finished first removed the other's files. On
/// 2026-09-24 that failed "a lower-priority mount still supplies what the higher one lacks" and
/// "many assets load concurrently" once each under `-j8`, and both passed on a rerun.
///
/// So a name here is random, and it is only a candidate. The directory is **claimed by creating
/// it**, which the file system does atomically: `create_directory` reports whether it made the
/// directory or found one already there, and one found already there belongs to somebody else
/// and is neither used nor removed. Distinctness therefore rests on the claim rather than on the
/// quality of the random source; the randomness only keeps retries rare.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <string_view>
#include <system_error>

namespace atlas::test {

/// A fresh, empty directory under the system's temporary directory, removed with everything in
/// it when this object is destroyed.
///
/// Ownership: the object owns the directory it created and nothing else. It is never handed a
/// directory that already existed, so its destructor cannot remove another test's files.
/// Lifetime: the directory exists from construction until destruction. Not copyable or movable,
/// because two owners of one directory is the defect this exists to remove.
/// Threads: any thread; distinct objects never share a directory, in or across processes.
/// Failure: if no directory can be created the constructor fails the running test case with the
/// path and the system's reason. A failure to remove it afterwards is ignored, since a destructor
/// must neither throw nor log; what remains is a directory with a random name in the system's
/// temporary directory.
class ScratchDir {
  public:
    /// Create `<temp>/<prefix>-<16 hex digits>`. The prefix says which test left a directory
    /// behind if a process is killed before its destructor runs.
    explicit ScratchDir(std::string_view prefix) {
        std::error_code error;
        const std::filesystem::path parent = std::filesystem::temp_directory_path(error);
        if (error) {
            FAIL(std::format("no temporary directory for '{}': {}", prefix, error.message()));
        }

        // The clock is mixed in so that a random_device which is deterministic — permitted by
        // the standard, and once the case on MinGW — still differs between processes started at
        // different moments. Neither source is needed for correctness; the claim below is.
        std::random_device device;
        const auto clock =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

        constexpr int kAttempts = 64;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            const std::uint64_t value =
                ((static_cast<std::uint64_t>(device()) << 32U) | device()) ^ clock;
            auto candidate = parent / std::format("{}-{:016x}", prefix, value);
            if (std::filesystem::create_directory(candidate, error)) {
                m_path = std::move(candidate);
                return;
            }
            if (error) {
                FAIL(std::format("could not create scratch directory {}: {}", candidate.string(),
                                 error.message()));
            }
            // Already there: somebody else's. Draw again rather than share it.
        }
        FAIL(std::format("no unused scratch directory name for '{}' in {} after {} attempts",
                         prefix, parent.string(), kAttempts));
    }

    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

  private:
    std::filesystem::path m_path;
};

}  // namespace atlas::test
