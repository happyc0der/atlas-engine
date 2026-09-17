// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A fixed pool of worker threads, and a parallel loop over a range.
///
/// **What this deliberately is not.** There is no task graph, no dependencies between tasks, no
/// futures, and no work stealing. The charter defers a work-stealing scheduler until a measured
/// limitation justifies it, and the only caller in sight is a simulation phase that wants a
/// range split across threads. A general scheduler built now would be built against imagination.
///
/// **Determinism.** `parallel_for` splits its range into chunks that depend only on the count
/// and the grain, never on the worker count or on timing. Which worker runs which chunk does
/// vary; what each chunk computes does not. So a body that writes only to its own indices, and
/// reads only what it does not write, produces byte-identical results at any worker count —
/// which is what M8's exit criterion asks for, and why the partitioning is fixed rather than
/// stolen from.
///
/// **Exceptions.** ADR-0005 forbids exceptions crossing a thread boundary. A body that throws
/// would have nowhere to be caught, so the worker loop catches, reports, and ends the process
/// rather than letting the exception unwind out of a thread. That is the same treatment
/// allocation failure gets: a bug to be seen, not an error to be handled.
///
/// Thread affinity: create, destroy and call `parallel_for` from one thread. The pool does not
/// support being driven from several threads at once, and nothing needs that yet.

#include <atlas/core/result.hpp>

#include <cstddef>
#include <functional>
#include <memory>

namespace atlas::tasks {

/// How a range is divided. A pure function of the range and the grain, exposed because the
/// tests assert on it and because a caller choosing a grain deserves to see what it buys.
struct Partition {
    std::size_t chunks = 0;
    std::size_t chunk_size = 0;
};

/// The chunking `parallel_for` will use. `grain` is the smallest chunk worth handing to a
/// worker; a grain of zero means one chunk per worker-sized slice, which is the usual choice.
[[nodiscard]] Partition partition_of(std::size_t count, std::size_t grain) noexcept;

class WorkerPool {
  public:
    /// The most workers this will create, whatever is asked for. A pool larger than the
    /// machine is slower, not faster, and an unbounded count from a configuration file would
    /// be a denial of service on the machine it ran on.
    static constexpr std::size_t kMaxWorkers = 64;

    /// `workers` is the number of *additional* threads. Zero is legal and means everything
    /// runs on the calling thread, which is the configuration the determinism tests compare
    /// everything else against.
    ///
    /// Failure: InvalidArgument above kMaxWorkers.
    [[nodiscard]] static Result<WorkerPool> create(std::size_t workers);

    /// One worker per hardware thread beyond the calling one, which is what a machine can
    /// actually run while the caller also works. Never zero.
    [[nodiscard]] static std::size_t default_worker_count() noexcept;

    WorkerPool() = default;
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&& other) noexcept;
    WorkerPool& operator=(WorkerPool&& other) noexcept;

    /// Additional threads. Zero means the calling thread does everything.
    [[nodiscard]] std::size_t worker_count() const noexcept;

    /// Run `body(begin, end)` over every chunk of [0, count), and return when all have run.
    ///
    /// The calling thread takes chunks too, so no thread idles waiting for the others. `body`
    /// is called once per chunk, possibly on several threads at once, and must not assume
    /// which. Chunks are claimed in whatever order threads reach them.
    void parallel_for(std::size_t count, std::size_t grain,
                      const std::function<void(std::size_t, std::size_t)>& body);

  private:
    struct Impl;
    explicit WorkerPool(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::tasks
