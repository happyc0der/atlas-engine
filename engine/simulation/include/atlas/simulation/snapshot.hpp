// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Handing simulation state to the renderer without sharing it.
///
/// The renderer must never read authoritative state. Not because reading is slow, but
/// because a frame drawn halfway through a tick shows a world that never existed, and
/// because the moment the renderer holds a reference into live state, moving the simulation
/// onto another thread stops being a scheduling change and becomes a locking problem.
///
/// So the simulation publishes: after a tick it builds an immutable snapshot of only what is
/// worth drawing, and hands over a shared pointer to it. The renderer takes the latest one at
/// the start of a frame and holds it for the whole frame, so what it draws is one consistent
/// moment however long the frame takes.
///
/// **Latest wins.** If the simulation publishes three snapshots while a frame is being drawn,
/// the two older ones are dropped. There is no queue, because a renderer that fell behind
/// would be drawing history and the backlog would only grow.
///
/// **Snapshots are presentation data, not state.** Positions and colours, not the tables
/// they came from. Copying the authoritative tables would make the copy cost scale with the
/// world rather than with what is on screen. What goes in one is the application's decision;
/// this only carries it.
///
/// Thread affinity: one producer, one consumer, either order, any threads. The shared pointer
/// operations are atomic, which is why this was built this way in M6 rather than later: it
/// needed no change when M8 moved the compute phase onto workers.

#include <atlas/core/time.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace atlas::sim {

/// What every snapshot carries, whatever else the application puts in it.
struct SnapshotHeader {
    /// The tick this state is from. The renderer interpolates between two of these.
    Tick tick = 0;

    /// The state hash at that tick, so a picture on screen can be tied to a hash in a log.
    std::uint64_t state_hash = 0;

    /// Increases by one per publication. A consumer can tell a new snapshot from the same
    /// one handed back, and can see how many it skipped.
    std::uint64_t generation = 0;
};

/// A single-producer, single-consumer, latest-wins handoff.
///
/// `T` must be immutable once published. The channel holds `shared_ptr<const T>`, so this is
/// enforced by the type rather than by agreement.
template <typename T> class SnapshotChannel {
  public:
    using Pointer = std::shared_ptr<const T>;

    /// Make a snapshot available, replacing whatever was there.
    ///
    /// The previous one is not destroyed here if a consumer still holds it; it goes when the
    /// last holder drops it. That is the reason for the shared pointer: the renderer can
    /// keep drawing from a snapshot the simulation has already replaced.
    void publish(Pointer snapshot) {
        {
            const std::scoped_lock guard(m_mutex);
            m_latest = std::move(snapshot);
        }
        m_published.fetch_add(1, std::memory_order_relaxed);
    }

    /// The most recent snapshot, or null before anything was published.
    ///
    /// Returns a shared pointer rather than a reference deliberately: the caller holds the
    /// snapshot alive for as long as it is using it, which is what makes a frame consistent.
    [[nodiscard]] Pointer latest() const {
        const std::scoped_lock guard(m_mutex);
        return m_latest;
    }

    /// How many snapshots have been published. Counts the ones nobody looked at.
    [[nodiscard]] std::uint64_t published() const noexcept {
        return m_published.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool empty() const { return latest() == nullptr; }

    void clear() {
        const std::scoped_lock guard(m_mutex);
        m_latest = nullptr;
    }

  private:
    /// Guards the pointer, not the snapshot.
    ///
    /// `std::atomic<std::shared_ptr<T>>` would be the obvious choice and is not available:
    /// libc++ on the development platform does not define
    /// `__cpp_lib_atomic_shared_ptr`, checked rather than assumed. The deprecated
    /// `std::atomic_load` overloads for shared_ptr are removed in C++26, so they are not a
    /// way forward either.
    ///
    /// The cost is a mutex held for the length of a pointer copy, once per tick to publish
    /// and once per frame to take. That is not a contended lock and it is not on any path
    /// where it could become one. If the standard-library support arrives, this becomes an
    /// internal change with no effect on callers.
    mutable std::mutex m_mutex;
    Pointer m_latest;

    /// Kept outside the lock: a counter nobody makes decisions on does not need to be
    /// consistent with the pointer, and reading it should not contend with publishing.
    std::atomic<std::uint64_t> m_published{0};
};

}  // namespace atlas::sim
