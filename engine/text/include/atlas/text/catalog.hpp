// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The strings an interface shows, looked up by key (ADR-0016).
///
/// **A missing key returns the key.** Never an empty string, never an error. A panel reading
/// `ui.controls.pause` where it meant "Pause" is visibly wrong and still usable, which is the
/// right failure for presentation — the same instinct as a missing asset resolving to a
/// fallback rather than stopping the engine. It also means a caller needs no null check and no
/// error path, which is why the overlay can hold a catalog that does not exist yet.
///
/// Thread affinity: **main thread only**, and not merely by convention. `lookup` is `const` and
/// still records what it could not find, so two threads calling it at once would race on the
/// miss bookkeeping. Nothing about a string table wants a worker.

#include <atlas/core/result.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace atlas::text {
namespace detail {

/// Hashing that lets a `std::string_view` find a `std::string` key without building one.
///
/// Load-bearing rather than tidy. `lookup` runs once per visible string per frame, and without
/// a transparent hash every call would allocate a `std::string` just to throw it away — a
/// per-frame allocation in a hot path, which this project forbids.
struct StringHash {
    // The standard spells this member exactly this way; it is the opt-in for heterogeneous
    // lookup and cannot be renamed to match the project's convention.
    // NOLINTNEXTLINE(readability-identifier-naming)
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

}  // namespace detail

/// Keys to strings, for one locale.
class Catalog {
  public:
    /// Most entries a catalog will hold.
    ///
    /// The interface has about a hundred and forty strings; a game that linked the engine would
    /// have more, and this is the bound at which a table stops looking like a mistake. It is
    /// enforced here as well as in the importer, because a catalog can be filled in code.
    static constexpr std::size_t kMaxStrings = 16'384;

    /// Distinct missing keys remembered, so each is logged exactly once.
    ///
    /// Bounded because remembering is what makes "once per key" possible, and a caller that
    /// built keys in a loop would otherwise grow this without limit — a leak that only appears
    /// in the one situation where something is already going wrong. Past this, misses are still
    /// counted and the overflow is logged once.
    static constexpr std::size_t kMaxRecordedMisses = 256;

    /// Add one entry.
    ///
    /// Fails with `MalformedData` on a duplicate key, on an empty key, and past `kMaxStrings`.
    /// A duplicate is refused rather than resolved last-wins, because which of two identical
    /// keys should win is not a question a translator can be expected to have an answer to.
    [[nodiscard]] Status insert(std::string_view key, std::string_view value);

    /// The string for `key`, or `key` itself when there is none.
    ///
    /// **The lifetime of the result depends on which of those happened.** On a hit it points
    /// into this catalog and stays valid until the entry is replaced or `clear` is called; the
    /// container is node-based, so inserting more does not invalidate it. On a miss it is
    /// `key` — the caller's own bytes, with the caller's own lifetime. A caller that stores the
    /// result for longer than it stores the key has a dangling view on exactly the path that is
    /// already going wrong.
    ///
    /// A miss is counted and remembered. **It is not logged here.** Logging formats, and a
    /// key that is missing is missing on every frame that draws it, so naming it here would
    /// bury the build's output in one mistake. `log_new_misses` says so instead, at a point
    /// somebody chose rather than in the middle of drawing.
    ///
    /// **The hit path allocates nothing**, which is the property that matters: it runs once
    /// per visible string per frame, and the transparent hash above is what lets a
    /// `string_view` find a `std::string` key without building one. The miss path allocates
    /// once for each key it has not seen before, and allocation failure is fatal by ADR-0005.
    /// That is why this is not `noexcept`: a function that allocates and says otherwise is
    /// making a promise it has not got.
    [[nodiscard]] std::string_view lookup(std::string_view key) const;

    /// Log every key that has been missed since the last call, and return how many.
    ///
    /// Each key is named exactly once for the lifetime of the catalog, however many frames
    /// asked for it. Call it once a frame, or once at the end of a check; calling it never is
    /// allowed and merely means nothing is reported.
    ///
    /// Not `noexcept`, for the reason `lookup` is: this is where the formatting happens.
    std::size_t log_new_misses() const;

    [[nodiscard]] std::size_t size() const noexcept { return m_strings.size(); }

    /// Total misses since construction, including repeats of the same key.
    [[nodiscard]] std::size_t misses() const noexcept { return m_misses; }

    /// Distinct missing keys, which is what `--text-check` reports.
    [[nodiscard]] std::size_t distinct_misses() const noexcept { return m_missing.size(); }

    /// Drop every entry, and every memory of what was missing.
    ///
    /// The miss bookkeeping goes too: after a reload the keys that were absent may not be, and
    /// a key logged against the old table should be logged again against the new one.
    void clear() noexcept;

  private:
    /// Looked up by key and never iterated, which is what makes an unordered container
    /// permitted here: nothing this produces is ordered, hashed, or written down.
    std::unordered_map<std::string, std::string, detail::StringHash, std::equal_to<>> m_strings;

    /// Mutable because `lookup` is const and a caller holding a `const Catalog*` must still be
    /// able to find out what it asked for and did not get. This is the whole reason the class
    /// is main-thread only.
    mutable std::unordered_set<std::string, detail::StringHash, std::equal_to<>> m_missing;

    /// Keys recorded but not yet named in the log, as views into `m_missing`'s own nodes.
    ///
    /// Views rather than copies because the set is node-based and its keys do not move; this
    /// is drained by `log_new_misses` and is empty in the steady state.
    mutable std::vector<std::string_view> m_unlogged;

    mutable std::size_t m_misses = 0;
    mutable bool m_miss_overflow_logged = false;
};

}  // namespace atlas::text
