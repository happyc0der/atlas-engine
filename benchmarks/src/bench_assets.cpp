// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asset import: how long a texture takes to decode, and how long the same texture takes to
// come back from the artifact cache. The scenario docs/PERFORMANCE.md has listed since M4.
//
// The cache was deferred in M4 on the grounds that nothing took long enough to import to
// justify one. This is the measurement that decides whether that was true, and it is written
// to be able to say "no" as readily as "yes": both numbers are reported as they are.
//
// The image is generated here rather than committed, because a two-thousand-pixel-square
// noise image is sixteen megabytes and does not belong in a repository. Noise rather than a
// flat colour, so it does not compress to nothing and the decode has real work to do.

#include <atlas/assets/artifact_cache.hpp>
#include <atlas/assets/importer.hpp>
#include <atlas/simulation/rng.hpp>

#include "harness.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace {

using atlas::bench::Result;

/// Collects the encoder's output rather than letting it write a file.
void collect(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::byte>*>(context);
    const auto* bytes = static_cast<const std::byte*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// A PNG of pseudo-random pixels at the given size.
[[nodiscard]] std::vector<std::byte> make_png(std::uint32_t side) {
    // The engine's own generator, seeded, so the image is the same every run and the numbers
    // are comparable across runs.
    atlas::sim::RngStream rng{0xA55E75, atlas::sim::stream_id("bench image"), 0};
    std::vector<unsigned char> pixels(static_cast<std::size_t>(side) * side * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const std::uint32_t v = rng.next_u32();
        pixels[i + 0] = static_cast<unsigned char>(v & 0xFFU);
        pixels[i + 1] = static_cast<unsigned char>((v >> 8U) & 0xFFU);
        pixels[i + 2] = static_cast<unsigned char>((v >> 16U) & 0xFFU);
        pixels[i + 3] = 255;
    }

    std::vector<std::byte> encoded;
    stbi_write_png_to_func(collect, &encoded, static_cast<int>(side), static_cast<int>(side), 4,
                           pixels.data(), static_cast<int>(side) * 4);
    return encoded;
}

[[nodiscard]] std::vector<Result> run() {
    std::vector<Result> results;

    std::error_code ec;
    const auto cache_dir = std::filesystem::temp_directory_path(ec) / "atlas-bench-artifact-cache";
    std::filesystem::remove_all(cache_dir, ec);

    auto cache = atlas::assets::ArtifactCache::open(cache_dir);
    if (!cache) {
        std::printf("assets: skipped, cannot open a cache directory: %s\n",
                    cache.error().to_string().c_str());
        return results;
    }

    for (const std::uint32_t side : {512U, 2048U}) {
        const std::vector<std::byte> png = make_png(side);
        const auto label = std::format("{}x{} png, {} KiB", side, side, png.size() / 1024);
        const auto when = std::filesystem::file_time_type{} + std::chrono::seconds{1'000};
        const std::uint64_t key = atlas::assets::ArtifactCache::key_for(
            "bench.png", png.size(), when, atlas::assets::kTextureImporterVersion);

        // Cold: decode. Fewer iterations at the large size, since each is real work.
        const std::uint64_t iterations = side >= 2048 ? 12 : 40;
        auto cold = atlas::bench::measure("assets/import_cold", label, iterations, 2, [&] {
            auto imported = atlas::assets::import_texture(png, "bench");
            if (!imported) {
                std::printf("assets: decode failed: %s\n", imported.error().to_string().c_str());
            }
        });
        cold.units_per_iteration = static_cast<std::uint64_t>(side) * side;
        cold.unit_name = "pixels";
        results.push_back(cold);

        // Populate the cache once, outside any timing.
        auto decoded = atlas::assets::import_texture(png, "bench");
        if (!decoded || !cache->store_texture(key, *decoded)) {
            std::printf("assets: could not populate the cache for %s\n", label.c_str());
            continue;
        }

        // Warm: read the cached entry. The same key the registry would compute.
        auto warm = atlas::bench::measure("assets/import_warm", label, iterations, 2, [&] {
            auto loaded = cache->load_texture(key);
            if (!loaded) {
                std::printf("assets: cache miss where a hit was expected\n");
            }
        });
        warm.units_per_iteration = static_cast<std::uint64_t>(side) * side;
        warm.unit_name = "pixels";
        results.push_back(warm);

        // Hashing the source is part of every cache lookup and is paid on hits and misses
        // alike, so it is reported on its own.
        auto hashing = atlas::bench::measure("assets/cache_key", label, iterations * 4, 4, [&] {
            volatile std::uint64_t sink = atlas::assets::ArtifactCache::key_for(
                "bench.png", png.size(), when, atlas::assets::kTextureImporterVersion);
            (void)sink;
        });
        hashing.units_per_iteration = png.size();
        hashing.unit_name = "bytes";
        results.push_back(hashing);
    }

    std::filesystem::remove_all(cache_dir, ec);
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("assets", run);

}  // namespace
