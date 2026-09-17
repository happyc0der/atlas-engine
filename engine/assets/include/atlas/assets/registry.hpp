// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The asset registry: what exists, what state it is in, and how it gets loaded.
///
/// **Threading.** Reading and decoding happen on a small worker pool owned by this module,
/// not on `atlas::tasks`. That pool exists since M8, so this is a choice rather than a
/// limitation: the specification wants asset work kept separate from deterministic
/// simulation scheduling, and a blocking file read on a simulation worker would stall a
/// tick. Workers touch only
/// bytes: they never see the window, the graphics device, or any engine state. Finished work
/// comes back through a queue that the main thread drains, and only the main thread turns
/// decoded data into a graphics resource.
///
/// **Failure.** A missing or broken asset is not fatal. It is recorded, reported once, and
/// resolves to a fallback so that the rest of the frame carries on. An engine that stops
/// because one texture is corrupt is much harder to work on than one that draws a magenta
/// square and says why.

#include <atlas/assets/asset_id.hpp>
#include <atlas/assets/asset_state.hpp>
#include <atlas/assets/filesystem.hpp>
#include <atlas/assets/importer.hpp>
#include <atlas/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atlas::assets {

/// What the registry knows about one asset.
struct AssetInfo {
    AssetId id;
    VirtualPath path;
    AssetType type = AssetType::Unknown;
    AssetState state = AssetState::Unloaded;
    /// Why it failed, when it did.
    std::string error;
    /// How many times it has finished loading. Two or more means it was reloaded.
    std::uint32_t load_count = 0;
    std::uint64_t bytes = 0;
};

/// Counts for the status panel and for tests.
struct RegistryStats {
    std::size_t total = 0;
    std::size_t ready = 0;
    std::size_t failed = 0;
    /// Loads served from the artifact cache rather than decoded. Zero without a cache.
    std::size_t cache_hits = 0;
    /// Loads that decoded because the cache had no valid entry.
    std::size_t cache_misses = 0;
    std::size_t in_progress = 0;
    /// Assets waiting for the main thread to finish them.
    std::size_t awaiting_finalisation = 0;
};

class Registry {
  public:
    struct Config {
        /// Threads that read and decode. Two is enough to keep loading off the main thread
        /// without turning a background job into a scheduling problem; the number becomes
        /// worth measuring when there is something to measure.
        std::uint32_t worker_count = 2;

        /// Where decoded assets are kept between runs. Empty means no cache, which is what
        /// every run before M7 had.
        std::filesystem::path cache_directory;
    };

    [[nodiscard]] static Result<Registry> create(FileSystem& filesystem, const Config& config);

    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&& other) noexcept;
    Registry& operator=(Registry&& other) noexcept;

    /// Register an asset and begin loading it.
    ///
    /// Returns the identifier immediately; the asset itself arrives later. Requesting the
    /// same asset twice returns the same identifier and does not load it twice.
    [[nodiscard]] Result<AssetId> request(const VirtualPath& path, AssetType type);

    /// Everything a caller can ask about an asset.
    [[nodiscard]] std::optional<AssetInfo> info(AssetId id) const;
    [[nodiscard]] AssetState state(AssetId id) const;

    /// Decoded pixels for a texture that has finished decoding but not yet been finalised.
    ///
    /// Only the main thread should call this, and only from a finaliser: the data is moved
    /// out, so a second call returns nothing.
    [[nodiscard]] std::optional<ImportedTexture> take_texture(AssetId id);

    /// Compiled code for a shader that has finished loading.
    [[nodiscard]] std::optional<ImportedShader> take_shader(AssetId id);

    /// Bring finished work into the registry.
    ///
    /// Called once per frame on the main thread. Returns how many assets changed state,
    /// which is what tells a caller whether anything needs finalising.
    std::size_t pump();

    /// Assets that have decoded and are waiting for the main thread.
    [[nodiscard]] std::vector<AssetId> pending_finalisation() const;

    /// Record that the main thread has turned a decoded asset into a resource.
    void mark_ready(AssetId id);

    /// Record that finalising failed.
    void mark_failed(AssetId id, std::string_view reason);

    /// Re-read anything whose file has changed since it was loaded.
    ///
    /// Polling rather than watching the filesystem: three platforms have three different
    /// notification interfaces, and this is a development convenience whose cost is a stat
    /// call per asset. Returns the assets that were re-queued.
    std::vector<AssetId> reload_changed();

    /// Everything known, ordered by path.
    ///
    /// Ordered rather than in whatever order a hash map produced, because this feeds a
    /// status panel that would otherwise reshuffle itself every frame.
    [[nodiscard]] std::vector<AssetInfo> all() const;

    [[nodiscard]] RegistryStats stats() const;

  private:
    Registry() = default;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::assets
