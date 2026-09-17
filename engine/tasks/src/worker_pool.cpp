// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/tasks/worker_pool.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace atlas::tasks {
namespace {

constexpr log::Category kTasks{"tasks"};

/// One parallel_for's worth of work. Lives on the calling thread's stack for the duration of
/// the call; workers only ever see it through a pointer the mutex protects.
struct Job {
    const std::function<void(std::size_t, std::size_t)>* body = nullptr;
    std::size_t count = 0;
    std::size_t chunk_size = 0;
    std::size_t chunks = 0;
    std::atomic<std::size_t> next_chunk{0};
    std::atomic<std::size_t> chunks_done{0};
    /// Workers that have taken this job and may still be reading it.
    ///
    /// Not redundant with chunks_done, which is what the first version used on its own and what
    /// the thread sanitizer rejected. A worker leaves its claim loop by reading `chunks` and
    /// finding none left, and that read happens after the last chunk was counted — so the
    /// calling thread could already have woken, returned, and destroyed this object on its
    /// stack. Waiting for this to reach zero waits for workers to be out, which is the thing
    /// that has to be true before the job dies.
    std::atomic<std::size_t> workers_present{0};
};

/// The one place an exception may not leave. ADR-0005 keeps exceptions inside a thread, and
/// there is nowhere for one to go from here: the caller is blocked in a different frame and the
/// standard would call std::terminate anyway. Reporting first makes the difference between a
/// diagnosable crash and a silent one.
void run_chunk(const Job& job, std::size_t chunk) noexcept {
    const std::size_t begin = chunk * job.chunk_size;
    const std::size_t end = std::min(begin + job.chunk_size, job.count);
    try {
        (*job.body)(begin, end);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "atlas::tasks: a parallel_for body threw: %s\n", error.what());
        std::abort();
    } catch (...) {
        std::fprintf(stderr, "atlas::tasks: a parallel_for body threw an unknown exception\n");
        std::abort();
    }
}

}  // namespace

Partition partition_of(std::size_t count, std::size_t grain) noexcept {
    if (count == 0) {
        return {};
    }
    const std::size_t size = std::max<std::size_t>(1, grain);
    return Partition{.chunks = (count + size - 1) / size, .chunk_size = size};
}

struct WorkerPool::Impl {
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable work_ready;
    std::condition_variable chunk_done;
    Job* job = nullptr;
    /// Bumped for every parallel_for, so a worker waking up can tell a new job from the one it
    /// already finished without comparing pointers that may have been reused.
    std::uint64_t generation = 0;
    bool stopping = false;
};

WorkerPool::WorkerPool(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

std::size_t WorkerPool::default_worker_count() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 1) {
        return 0;
    }
    return std::min<std::size_t>(kMaxWorkers, hardware - 1);
}

Result<WorkerPool> WorkerPool::create(std::size_t workers) {
    if (workers > kMaxWorkers) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("{} workers requested; the limit is {}", workers, kMaxWorkers)));
    }

    auto impl = std::make_unique<Impl>();
    impl->workers.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        impl->workers.emplace_back([raw = impl.get()] {
            ATLAS_THREAD_NAME("atlas worker");
            std::uint64_t seen = 0;
            while (true) {
                Job* job = nullptr;
                {
                    std::unique_lock<std::mutex> lock(raw->mutex);
                    raw->work_ready.wait(
                        lock, [raw, seen] { return raw->stopping || raw->generation != seen; });
                    if (raw->stopping) {
                        return;
                    }
                    seen = raw->generation;
                    job = raw->job;
                    if (job != nullptr) {
                        // Registered while the lock is held, so the calling thread cannot
                        // conclude that every worker has left between this read and this
                        // increment.
                        job->workers_present.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (job == nullptr) {
                    continue;
                }
                // Claimed without the lock: the only shared state is an atomic counter, and
                // holding a mutex for the length of a chunk would serialise the whole point.
                while (true) {
                    const std::size_t chunk =
                        job->next_chunk.fetch_add(1, std::memory_order_relaxed);
                    if (chunk >= job->chunks) {
                        break;
                    }
                    run_chunk(*job, chunk);
                    job->chunks_done.fetch_add(1, std::memory_order_acq_rel);
                }
                {
                    // Leaving is what the caller waits for, so it is announced under the lock.
                    const std::scoped_lock guard(raw->mutex);
                    job->workers_present.fetch_sub(1, std::memory_order_release);
                    raw->chunk_done.notify_all();
                }
            }
        });
    }

    if (workers > 0) {
        ATLAS_LOG_INFO(kTasks, "worker pool ready: {} worker thread(s) beside the caller", workers);
    }
    return WorkerPool{std::move(impl)};
}

WorkerPool::~WorkerPool() {
    if (m_impl == nullptr) {
        return;
    }
    {
        const std::scoped_lock guard(m_impl->mutex);
        m_impl->stopping = true;
    }
    m_impl->work_ready.notify_all();
    for (std::thread& worker : m_impl->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

WorkerPool::WorkerPool(WorkerPool&& other) noexcept = default;

WorkerPool& WorkerPool::operator=(WorkerPool&& other) noexcept {
    if (this != &other) {
        // The current pool's threads are joined before its state goes away, which is what the
        // destructor is for; calling it explicitly here would leave a moved-from Impl behind.
        const WorkerPool old(std::move(*this));
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

std::size_t WorkerPool::worker_count() const noexcept {
    return m_impl == nullptr ? 0 : m_impl->workers.size();
}

void WorkerPool::parallel_for(std::size_t count, std::size_t grain,
                              const std::function<void(std::size_t, std::size_t)>& body) {
    ATLAS_ZONE_NAMED("tasks::parallel_for");
    const Partition partition = partition_of(count, grain);
    if (partition.chunks == 0) {
        return;
    }

    Job job;
    job.body = &body;
    job.count = count;
    job.chunk_size = partition.chunk_size;
    job.chunks = partition.chunks;

    // One chunk, or no workers: run it here. Waking a thread to do what this one could have
    // done in the time it took to wake is the classic way to make a parallel loop slower.
    if (m_impl == nullptr || m_impl->workers.empty() || partition.chunks == 1) {
        for (std::size_t chunk = 0; chunk < partition.chunks; ++chunk) {
            run_chunk(job, chunk);
        }
        return;
    }

    {
        const std::scoped_lock guard(m_impl->mutex);
        m_impl->job = &job;
        ++m_impl->generation;
    }
    m_impl->work_ready.notify_all();

    // The calling thread takes chunks too, rather than blocking immediately: it is a whole
    // thread, and on a two-core machine it is half of them.
    while (true) {
        const std::size_t chunk = job.next_chunk.fetch_add(1, std::memory_order_relaxed);
        if (chunk >= job.chunks) {
            break;
        }
        run_chunk(job, chunk);
        job.chunks_done.fetch_add(1, std::memory_order_acq_rel);
    }

    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        // Both conditions, and the second is the one that matters for lifetime: every chunk has
        // run, and no worker is still inside this job, which lives on this stack frame.
        m_impl->chunk_done.wait(lock, [&job] {
            return job.chunks_done.load(std::memory_order_acquire) == job.chunks &&
                   job.workers_present.load(std::memory_order_acquire) == 0;
        });
        // Cleared while the lock is held and before `job` leaves scope, so no worker can still
        // be holding a pointer to a dead stack frame.
        m_impl->job = nullptr;
    }
}

}  // namespace atlas::tasks
