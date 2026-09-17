// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/artifact_cache.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace atlas::assets {
namespace {

constexpr log::Category kAssets{"assets"};

/// Pumps an asset may sit decoded before the registry says something about it.
///
/// Reaching `Ready` needs a finaliser — whoever owns the resource the bytes become — and the
/// registry cannot know whether one exists for a given type, because it deliberately knows
/// nothing about devices. So an asset type added without a finaliser decodes, arrives at
/// `Decoded`, and stays there forever with nothing logging and nothing failing.
///
/// That is not hypothetical. `AssetType::Shader` has been in this enumeration since M4 and
/// nothing in the tree has ever called `take_shader`: shaders come from the generated manifest
/// instead, which is a deliberate deferral recorded in the renderer. Anything requested as a
/// shader would climb `awaiting_finalisation` without bound and in silence.
///
/// Six hundred pumps is about ten seconds at sixty frames a second — long enough that a slow
/// finaliser, a paused debugger or a frame spike cannot trip it, short enough that a person
/// running an application sees the line before they stop looking.
constexpr std::uint32_t kStalledPumps = 600;

}  // namespace

namespace {

/// One asset's record.
///
/// The decoded payload lives here between a worker finishing and the main thread taking it,
/// which is the only moment the two threads share anything larger than a state enumerator.
struct Entry {
    AssetInfo info;
    std::optional<ImportedTexture> texture;
    std::optional<ImportedShader> shader;
    std::optional<ImportedAudio> audio;
    /// When the file was last read, for detecting a change on disk.
    std::optional<std::filesystem::file_time_type> loaded_at;
    /// Pumps this entry has spent decoded and unclaimed. See `kStalledPumps`.
    std::uint32_t decoded_pumps = 0;
    /// Whether the stall has already been reported, so it is said once and not every frame.
    bool stall_reported = false;
};

/// Work handed to a worker.
struct Job {
    AssetId id;
    VirtualPath path;
    AssetType type;
};

/// What a worker hands back.
struct Completion {
    AssetId id;
    std::optional<ImportedTexture> texture;
    std::optional<ImportedShader> shader;
    std::optional<ImportedAudio> audio;
    std::optional<std::filesystem::file_time_type> modified_at;
    std::string error;
    std::uint64_t bytes = 0;
    /// Whether the decoded data came from the artifact cache.
    bool from_cache = false;
};

}  // namespace

struct Registry::Impl {
    FileSystem* filesystem = nullptr;
    /// Shared with the workers by value: the cache is a directory and a counter, and copying
    /// the handle is how each worker gets its own view without a lock.
    std::optional<ArtifactCache> cache;
    std::size_t cache_hits = 0;
    std::size_t cache_misses = 0;
    /// Assets that have been decoded and unclaimed long enough to be reported. Never decreases.
    std::size_t stalled = 0;

    /// Guards the entry table. Held briefly: never across a file read or a decode.
    mutable std::mutex entries_mutex;
    std::unordered_map<AssetId, Entry> entries;

    /// The job queue, and the workers waiting on it.
    std::mutex queue_mutex;
    std::condition_variable queue_signal;
    std::deque<Job> queue;
    bool stopping = false;

    /// Finished work, waiting for the main thread.
    std::mutex completions_mutex;
    std::vector<Completion> completions;

    std::vector<std::jthread> workers;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() { stop(); }

    void stop() {
        {
            const std::scoped_lock lock{queue_mutex};
            stopping = true;
        }
        queue_signal.notify_all();
        // jthread joins on destruction; clearing here makes the ordering explicit rather
        // than depending on member declaration order.
        workers.clear();
    }

    /// Read and decode one asset. Runs on a worker, and touches no engine state.
    [[nodiscard]] Completion run(const Job& job) const {
        ATLAS_ZONE_NAMED("asset load");

        Completion completion;
        completion.id = job.id;

        auto bytes = filesystem->read(job.path);
        if (!bytes) {
            completion.error = bytes.error().to_string();
            return completion;
        }
        completion.bytes = bytes->size();

        if (auto modified = filesystem->modified_at(job.path)) {
            completion.modified_at = *modified;
        }

        switch (job.type) {
        case AssetType::Texture: {
            // The cache first, keyed by what is about to be decoded and by the importer that
            // would decode it. A hit is a read instead of a decode; a miss decodes and then
            // stores, so the next run hits.
            // The key needs the modification time, which was read above and may be missing;
            // without it the file is decoded and simply not cached.
            const bool cacheable = cache.has_value() && completion.modified_at.has_value();
            const std::uint64_t key =
                cacheable ? ArtifactCache::key_for(job.path.text(), bytes->size(),
                                                   *completion.modified_at, kTextureImporterVersion)
                          : 0;
            if (cacheable) {
                if (auto cached = cache->load_texture(key)) {
                    completion.texture = std::move(*cached);
                    completion.from_cache = true;
                    break;
                }
            }

            auto imported = import_texture(*bytes, job.path.text());
            if (!imported) {
                completion.error = imported.error().to_string();
                break;
            }
            if (cacheable) {
                // A failed store is logged and otherwise ignored: the decode succeeded, and
                // the only consequence is decoding again next time.
                if (auto status = cache->store_texture(key, *imported); !status) {
                    ATLAS_LOG_WARN(kAssets, "could not cache '{}': {}", job.path.text(),
                                   status.error());
                }
            }
            completion.texture = std::move(*imported);
            break;
        }
        case AssetType::Shader: {
            // A cooked shader is already compiled, so importing it is reading it. The
            // entry point and resource counts come from the caller, which got them from
            // the generated manifest.
            ImportedShader shader;
            shader.code = std::move(*bytes);
            completion.shader = std::move(shader);
            break;
        }
        case AssetType::AudioClip: {
            // Not cached. A decode is a copy of the samples with a conversion per sample, and
            // the artifact cache is texture-shaped end to end: its entry header is a width, a
            // height and a byte count, its filenames end in ".texture", and it carries one
            // importer-version constant. Widening all of that to save a copy nobody has
            // measured is the mistake M4 already made once with textures. Recorded in
            // docs/DEFERRED.md with the measurement that would change it.
            auto imported = import_audio(*bytes, job.path.text());
            if (!imported) {
                completion.error = imported.error().to_string();
                break;
            }
            completion.audio = std::move(*imported);
            break;
        }
        case AssetType::AnimationClip:
            // The type exists because a scene file records it: an asset identifier carries its
            // type in the hash, so the number had to be fixed when the scene format gained an
            // animator, one slice before the importer arrived. Requesting one in this build is
            // a clear failure rather than an asset that decodes into nothing.
            completion.error = std::format(
                "'{}' is an animation clip, which this build cannot import yet", job.path.text());
            break;
        case AssetType::Unknown:
            completion.error = std::format("'{}' has no importer for its type", job.path.text());
            break;
        }

        return completion;
    }

    void worker_loop(const std::stop_token& stop) {
        ATLAS_THREAD_NAME("asset worker");

        while (!stop.stop_requested()) {
            Job job{.id = {}, .path = *VirtualPath::parse("placeholder"), .type = {}};
            {
                std::unique_lock lock{queue_mutex};
                queue_signal.wait(lock, [this, &stop] {
                    return stopping || stop.stop_requested() || !queue.empty();
                });
                if (stopping || stop.stop_requested()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }

            {
                const std::scoped_lock lock{entries_mutex};
                if (const auto it = entries.find(job.id); it != entries.end()) {
                    it->second.info.state = AssetState::Loading;
                }
            }

            Completion completion = run(job);

            {
                const std::scoped_lock lock{completions_mutex};
                completions.push_back(std::move(completion));
            }
        }
    }
};

Registry::Registry(Registry&& other) noexcept : m_impl(std::move(other.m_impl)) {}

Registry& Registry::operator=(Registry&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Registry::~Registry() = default;

Result<Registry> Registry::create(FileSystem& filesystem, const Config& config) {
    if (config.worker_count == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  "the asset registry needs at least one worker; zero would queue work nothing "
                  "ever picks up"));
    }

    Registry registry;
    registry.m_impl = std::make_unique<Impl>();
    registry.m_impl->filesystem = &filesystem;

    // Opened before the workers start, so a cache that cannot be written is refused here,
    // once, with the path in the message, rather than failing on every store from a worker.
    if (!config.cache_directory.empty()) {
        auto cache = ArtifactCache::open(config.cache_directory);
        if (!cache) {
            return std::unexpected(std::move(cache).error().context("opening the artifact cache"));
        }
        registry.m_impl->cache = std::move(*cache);
    }

    registry.m_impl->workers.reserve(config.worker_count);
    for (std::uint32_t i = 0; i < config.worker_count; ++i) {
        registry.m_impl->workers.emplace_back(
            [impl = registry.m_impl.get()](const std::stop_token& stop) {
                impl->worker_loop(stop);
            });
    }

    ATLAS_LOG_INFO(kAssets, "asset registry ready with {} worker(s)", config.worker_count);
    return registry;
}

Result<AssetId> Registry::request(const VirtualPath& path, AssetType type) {
    if (m_impl == nullptr) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "request on an invalid registry"));
    }
    if (type == AssetType::Unknown) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("'{}' was requested without a type", path.text())));
    }

    const AssetId id = AssetId::from(path, type);

    {
        const std::scoped_lock lock{m_impl->entries_mutex};
        if (const auto it = m_impl->entries.find(id); it != m_impl->entries.end()) {
            // Already known. Asking again is not an error and does not load it twice.
            return id;
        }

        m_impl->entries.emplace(
            id, Entry{.info = AssetInfo{
                          .id = id, .path = path, .type = type, .state = AssetState::Queued}});
    }

    {
        const std::scoped_lock lock{m_impl->queue_mutex};
        m_impl->queue.push_back(Job{.id = id, .path = path, .type = type});
    }
    m_impl->queue_signal.notify_one();

    return id;
}

std::optional<AssetInfo> Registry::info(AssetId id) const {
    if (m_impl == nullptr) {
        return std::nullopt;
    }
    const std::scoped_lock lock{m_impl->entries_mutex};
    if (const auto it = m_impl->entries.find(id); it != m_impl->entries.end()) {
        return it->second.info;
    }
    return std::nullopt;
}

AssetState Registry::state(AssetId id) const {
    const auto found = info(id);
    return found ? found->state : AssetState::Unloaded;
}

std::optional<ImportedTexture> Registry::take_texture(AssetId id) {
    if (m_impl == nullptr) {
        return std::nullopt;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const std::scoped_lock lock{m_impl->entries_mutex};
    if (const auto it = m_impl->entries.find(id); it != m_impl->entries.end()) {
        return std::exchange(it->second.texture, std::nullopt);
    }
    return std::nullopt;
}

std::optional<ImportedShader> Registry::take_shader(AssetId id) {
    if (m_impl == nullptr) {
        return std::nullopt;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const std::scoped_lock lock{m_impl->entries_mutex};
    if (const auto it = m_impl->entries.find(id); it != m_impl->entries.end()) {
        return std::exchange(it->second.shader, std::nullopt);
    }
    return std::nullopt;
}

std::optional<ImportedAudio> Registry::take_audio(AssetId id) {
    if (m_impl == nullptr) {
        return std::nullopt;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const std::scoped_lock lock{m_impl->entries_mutex};
    if (const auto it = m_impl->entries.find(id); it != m_impl->entries.end()) {
        return std::exchange(it->second.audio, std::nullopt);
    }
    return std::nullopt;
}

std::size_t Registry::pump() {
    if (m_impl == nullptr) {
        return 0;
    }
    ATLAS_ZONE_NAMED("Registry::pump");
    ATLAS_ASSERT_MAIN_THREAD();

    std::vector<Completion> finished;
    {
        const std::scoped_lock lock{m_impl->completions_mutex};
        finished.swap(m_impl->completions);
    }

    // No early return on an empty list. A stall is exactly the case where nothing finished,
    // so returning here is how the first version of this check managed to never fire.
    const std::scoped_lock lock{m_impl->entries_mutex};
    for (auto& completion : finished) {
        const auto it = m_impl->entries.find(completion.id);
        if (it == m_impl->entries.end()) {
            continue;
        }
        Entry& entry = it->second;

        if (!completion.error.empty()) {
            entry.info.state = AssetState::Failed;
            entry.info.error = std::move(completion.error);
            // Once, at the moment it happens. An asset that fails every frame would
            // otherwise fill the log with the same line.
            ATLAS_LOG_ERROR(kAssets, "{} '{}' failed: {}", to_string(entry.info.type),
                            entry.info.path.text(), entry.info.error);
            continue;
        }

        entry.info.bytes = completion.bytes;
        entry.info.error.clear();
        entry.loaded_at = completion.modified_at;
        if (completion.texture.has_value()) {
            if (completion.from_cache) {
                ++m_impl->cache_hits;
            } else if (m_impl->cache.has_value()) {
                ++m_impl->cache_misses;
            }
        }
        entry.texture = std::move(completion.texture);
        entry.shader = std::move(completion.shader);
        entry.audio = std::move(completion.audio);

        // Decoded, not ready: a texture's pixels exist but its graphics resource does not,
        // and only the main thread may create one.
        entry.info.state = AssetState::Decoded;
        entry.decoded_pumps = 0;
        entry.stall_reported = false;
    }

    // Count how long anything has been waiting, and say so once when the wait stops being
    // plausible. Walked every pump rather than only when something changed state, because a
    // stall is precisely the case where nothing changes state.
    for (auto& [id, entry] : m_impl->entries) {
        if (entry.info.state != AssetState::Decoded) {
            continue;
        }
        if (entry.decoded_pumps < kStalledPumps) {
            ++entry.decoded_pumps;
            continue;
        }
        if (!entry.stall_reported) {
            entry.stall_reported = true;
            ++m_impl->stalled;
            ATLAS_LOG_WARN(kAssets,
                           "{} '{}' has been decoded and unclaimed for {} pumps; nothing "
                           "finalises this asset type, so it will never become ready",
                           to_string(entry.info.type), entry.info.path.text(), kStalledPumps);
        }
    }

    return finished.size();
}

std::vector<AssetId> Registry::pending_finalisation() const {
    std::vector<AssetId> pending;
    if (m_impl == nullptr) {
        return pending;
    }

    const std::scoped_lock lock{m_impl->entries_mutex};
    for (const auto& [id, entry] : m_impl->entries) {
        if (entry.info.state == AssetState::Decoded) {
            pending.push_back(id);
        }
    }

    // Sorted: the map's own order is arbitrary and would make the sequence of finalisations
    // differ between runs. See docs/DETERMINISM.md.
    std::ranges::sort(pending);
    return pending;
}

void Registry::mark_ready(AssetId id) {
    if (m_impl == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const std::scoped_lock lock{m_impl->entries_mutex};
    if (const auto it = m_impl->entries.find(id); it != m_impl->entries.end()) {
        it->second.info.state = AssetState::Ready;
        ++it->second.info.load_count;
        it->second.info.error.clear();
        ATLAS_LOG_DEBUG(kAssets, "{} '{}' ready ({} bytes)", to_string(it->second.info.type),
                        it->second.info.path.text(), it->second.info.bytes);
    }
}

void Registry::mark_failed(AssetId id, std::string_view reason) {
    if (m_impl == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const std::scoped_lock lock{m_impl->entries_mutex};
    if (const auto it = m_impl->entries.find(id); it != m_impl->entries.end()) {
        it->second.info.state = AssetState::Failed;
        it->second.info.error = std::string{reason};
        ATLAS_LOG_ERROR(kAssets, "{} '{}' failed: {}", to_string(it->second.info.type),
                        it->second.info.path.text(), it->second.info.error);
    }
}

std::vector<AssetId> Registry::reload_changed() {
    std::vector<AssetId> reloaded;
    if (m_impl == nullptr) {
        return reloaded;
    }
    ATLAS_ZONE_NAMED("Registry::reload_changed");
    ATLAS_ASSERT_MAIN_THREAD();

    std::vector<Job> to_queue;

    {
        const std::scoped_lock lock{m_impl->entries_mutex};
        for (auto& [id, entry] : m_impl->entries) {
            // Only settled assets are candidates: re-queueing something already in flight
            // would load it twice and race the two results.
            if (!is_settled(entry.info.state)) {
                continue;
            }

            auto modified = m_impl->filesystem->modified_at(entry.info.path);
            if (!modified) {
                continue;
            }
            if (entry.loaded_at.has_value() && *modified <= *entry.loaded_at) {
                continue;
            }

            entry.info.state = AssetState::Queued;
            to_queue.push_back(Job{.id = id, .path = entry.info.path, .type = entry.info.type});
            reloaded.push_back(id);
        }
    }

    if (!to_queue.empty()) {
        {
            const std::scoped_lock lock{m_impl->queue_mutex};
            for (auto& job : to_queue) {
                m_impl->queue.push_back(std::move(job));
            }
        }
        m_impl->queue_signal.notify_all();

        std::ranges::sort(reloaded);
        ATLAS_LOG_INFO(kAssets, "reloading {} changed asset(s)", reloaded.size());
    }

    return reloaded;
}

std::vector<AssetInfo> Registry::all() const {
    std::vector<AssetInfo> found;
    if (m_impl == nullptr) {
        return found;
    }

    {
        const std::scoped_lock lock{m_impl->entries_mutex};
        found.reserve(m_impl->entries.size());
        for (const auto& [id, entry] : m_impl->entries) {
            found.push_back(entry.info);
        }
    }

    // By path, so that a status panel does not reshuffle itself every frame.
    std::ranges::sort(found,
                      [](const AssetInfo& a, const AssetInfo& b) { return a.path < b.path; });
    return found;
}

RegistryStats Registry::stats() const {
    RegistryStats stats;
    if (m_impl == nullptr) {
        return stats;
    }
    stats.cache_hits = m_impl->cache_hits;
    stats.cache_misses = m_impl->cache_misses;
    stats.stalled = m_impl->stalled;

    const std::scoped_lock lock{m_impl->entries_mutex};
    stats.total = m_impl->entries.size();
    for (const auto& [id, entry] : m_impl->entries) {
        switch (entry.info.state) {
        case AssetState::Ready: ++stats.ready; break;
        case AssetState::Failed: ++stats.failed; break;
        case AssetState::Decoded:
            ++stats.in_progress;
            ++stats.awaiting_finalisation;
            break;
        case AssetState::Queued:
        case AssetState::Loading: ++stats.in_progress; break;
        case AssetState::Unloaded: break;
        }
    }
    return stats;
}

}  // namespace atlas::assets
