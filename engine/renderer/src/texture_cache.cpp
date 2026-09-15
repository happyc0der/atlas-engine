// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/renderer/texture_cache.hpp>

#include <array>
#include <format>
#include <utility>

namespace atlas::renderer {
namespace {

constexpr log::Category kRenderer{"renderer"};

/// The fallback: an eight-by-eight magenta and black checkerboard.
///
/// Deliberately hideous. A fallback that looked plausible would let a missing texture ship
/// unnoticed; this one is impossible to mistake for artwork, and it is a checkerboard rather
/// than a flat colour so that texture coordinates are visibly working even when the texture
/// is not.
constexpr std::uint32_t kFallbackSize = 8;

[[nodiscard]] std::vector<std::byte> make_fallback_pixels() {
    std::vector<std::byte> pixels(static_cast<std::size_t>(kFallbackSize) * kFallbackSize * 4);
    for (std::uint32_t y = 0; y < kFallbackSize; ++y) {
        for (std::uint32_t x = 0; x < kFallbackSize; ++x) {
            const bool magenta = ((x / 2) + (y / 2)) % 2 == 0;
            const auto level = static_cast<std::uint8_t>(magenta ? 255 : 0);
            const std::size_t index = ((static_cast<std::size_t>(y) * kFallbackSize) + x) * 4;
            pixels[index + 0] = std::byte{level};
            pixels[index + 1] = std::byte{0};
            pixels[index + 2] = std::byte{level};
            pixels[index + 3] = std::byte{255};
        }
    }
    return pixels;
}

}  // namespace

Result<TextureCache> TextureCache::create(rhi::Device& device) {
    ATLAS_ASSERT_MAIN_THREAD();

    TextureCache cache;
    cache.m_device = &device;

    auto fallback = device.create_texture({.width = kFallbackSize,
                                           .height = kFallbackSize,
                                           .format = rhi::TextureFormat::Rgba8Unorm,
                                           .debug_name = "fallback"});
    if (!fallback) {
        return std::unexpected(
            std::move(fallback).error().context("creating the fallback texture"));
    }
    cache.m_fallback = *fallback;

    const auto pixels = make_fallback_pixels();
    if (auto status = device.upload_texture(cache.m_fallback, pixels); !status) {
        return std::unexpected(std::move(status).error().context("uploading the fallback texture"));
    }

    // Nearest filtering, so the fallback reads as sharp squares at any size rather than as
    // a pink smear that might be mistaken for a real texture.
    auto sampler = device.create_sampler({.min_filter = rhi::Filter::Nearest,
                                          .mag_filter = rhi::Filter::Nearest,
                                          .address_u = rhi::AddressMode::Repeat,
                                          .address_v = rhi::AddressMode::Repeat,
                                          .debug_name = "texture cache"});
    if (!sampler) {
        return std::unexpected(std::move(sampler).error().context("creating the sampler"));
    }
    cache.m_sampler = *sampler;

    ATLAS_LOG_INFO(kRenderer, "texture cache ready with a {}x{} fallback", kFallbackSize,
                   kFallbackSize);
    return cache;
}

void TextureCache::release_all() noexcept {
    if (m_device == nullptr) {
        return;
    }
    for (auto& [id, handle] : m_textures) {
        m_device->destroy_texture(handle);
    }
    m_textures.clear();
    m_device->destroy_sampler(m_sampler);
    m_device->destroy_texture(m_fallback);
    m_device = nullptr;
}

TextureCache::~TextureCache() {
    release_all();
}

TextureCache::TextureCache(TextureCache&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_fallback(std::exchange(other.m_fallback, {})),
      m_sampler(std::exchange(other.m_sampler, {})), m_textures(std::move(other.m_textures)) {}

TextureCache& TextureCache::operator=(TextureCache&& other) noexcept {
    if (this != &other) {
        release_all();
        m_device = std::exchange(other.m_device, nullptr);
        m_fallback = std::exchange(other.m_fallback, {});
        m_sampler = std::exchange(other.m_sampler, {});
        m_textures = std::move(other.m_textures);
    }
    return *this;
}

std::size_t TextureCache::finalise_pending(assets::Registry& registry) {
    if (m_device == nullptr) {
        return 0;
    }
    ATLAS_ZONE_NAMED("TextureCache::finalise_pending");
    ATLAS_ASSERT_MAIN_THREAD();

    std::size_t created = 0;

    for (const auto id : registry.pending_finalisation()) {
        if (id.type() != assets::AssetType::Texture) {
            continue;
        }

        auto decoded = registry.take_texture(id);
        if (!decoded) {
            continue;
        }

        auto texture = m_device->create_texture({.width = decoded->width,
                                                 .height = decoded->height,
                                                 .format = rhi::TextureFormat::Rgba8Unorm,
                                                 .debug_name = "asset"});
        if (!texture) {
            registry.mark_failed(id, texture.error().to_string());
            continue;
        }

        if (auto status = m_device->upload_texture(*texture, decoded->pixels); !status) {
            m_device->destroy_texture(*texture);
            registry.mark_failed(id, status.error().to_string());
            continue;
        }

        // A reload replaces an existing texture, so the old one has to go or it leaks.
        if (const auto existing = m_textures.find(id); existing != m_textures.end()) {
            m_device->destroy_texture(existing->second);
            existing->second = *texture;
        } else {
            m_textures.emplace(id, *texture);
        }

        registry.mark_ready(id);
        ++created;
    }

    return created;
}

rhi::TextureHandle TextureCache::texture_for(assets::AssetId id) const {
    if (const auto found = m_textures.find(id); found != m_textures.end()) {
        return found->second;
    }
    // Missing, failed, or not ready yet. All three draw the fallback, which is visibly wrong
    // rather than invisible.
    return m_fallback;
}

void TextureCache::release(assets::AssetId id) {
    if (m_device == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    if (const auto found = m_textures.find(id); found != m_textures.end()) {
        m_device->destroy_texture(found->second);
        m_textures.erase(found);
    }
}

}  // namespace atlas::renderer
