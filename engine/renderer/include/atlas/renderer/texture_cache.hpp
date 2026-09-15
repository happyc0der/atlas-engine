// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Turning decoded images into graphics textures, on the thread allowed to do it.
///
/// The asset registry's workers decode images into memory and stop there, because creating a
/// graphics resource is main-thread work. This is the other half: it drains what the registry
/// has decoded and creates the textures, once per frame.
///
/// It also owns the fallback. An asset that is missing, corrupt or still loading resolves to
/// a visibly wrong magenta checkerboard rather than to nothing, so a frame draws and the
/// problem is obvious rather than invisible.

#include <atlas/assets/registry.hpp>
#include <atlas/core/result.hpp>
#include <atlas/rhi/device.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace atlas::renderer {

class TextureCache {
  public:
    [[nodiscard]] static Result<TextureCache> create(rhi::Device& device);

    /// An empty cache that owns nothing.
    ///
    /// The state a moved-from cache is left in, and what a member holds before its cache is
    /// created. Every operation on one is a no-op, and `texture_for` returns a null handle
    /// rather than a fallback it does not have.
    TextureCache() = default;

    ~TextureCache();

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    TextureCache(TextureCache&& other) noexcept;
    TextureCache& operator=(TextureCache&& other) noexcept;

    /// Create graphics textures for everything the registry has decoded.
    ///
    /// Called once per frame on the main thread. Returns how many were created, which for a
    /// steady frame is zero.
    std::size_t finalise_pending(assets::Registry& registry);

    /// The texture for an asset, or the fallback when it is missing, failed, or not ready.
    ///
    /// Always returns something drawable. A caller that wants to know the difference asks
    /// the registry for the state.
    [[nodiscard]] rhi::TextureHandle texture_for(assets::AssetId id) const;

    /// The sampler to use with any texture from this cache.
    [[nodiscard]] rhi::SamplerHandle sampler() const noexcept { return m_sampler; }

    /// The deliberately ugly stand-in for anything not available.
    [[nodiscard]] rhi::TextureHandle fallback() const noexcept { return m_fallback; }

    [[nodiscard]] std::size_t resident() const noexcept { return m_textures.size(); }

    /// Release the graphics texture for an asset, so a reload can replace it.
    void release(assets::AssetId id);

  private:
    void release_all() noexcept;

    rhi::Device* m_device = nullptr;
    rhi::TextureHandle m_fallback;
    rhi::SamplerHandle m_sampler;

    /// Looked up by identifier, never iterated where the order is observable.
    std::unordered_map<assets::AssetId, rhi::TextureHandle> m_textures;
};

}  // namespace atlas::renderer
