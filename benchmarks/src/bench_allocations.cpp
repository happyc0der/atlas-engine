// SPDX-License-Identifier: GPL-3.0-or-later
//
// Does a steady-state frame allocate?
//
// M3's exit criteria require no unbounded per-frame allocation after warm-up in the
// demonstrated path. That is a claim about behaviour, so it is measured rather than
// asserted: global operator new and delete are replaced with counting versions, a frame is
// run until it settles, and then the allocations of further identical frames are counted.
//
// "No unbounded allocation" rather than "no allocation": a fixed number of allocations per
// frame that does not grow with the scene is survivable. What is not survivable is a count
// that rises with the number of quads, because then a larger scene costs more per frame for
// ever.
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/rhi/device.hpp>

#include "harness.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace atlas::bench {

/// Counters the replaced allocation functions write to.
///
/// Namespace-scope and atomic because the replaced operators are global and may be called
/// from any thread. Relaxed ordering: these are counters, not synchronisation.
namespace {

struct AllocationCounters {
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<bool> counting{false};
};

AllocationCounters& allocation_counters() {
    static AllocationCounters instance;
    return instance;
}

}  // namespace

}  // namespace atlas::bench

// Replacing the global allocation functions is the only way to see every allocation,
// including those inside the standard library and inside the graphics library. The
// replacements must be as simple as possible: anything that itself allocated would recurse.
void* operator new(std::size_t size) {
    auto& counters = atlas::bench::allocation_counters();
    if (counters.counting.load(std::memory_order_relaxed)) {
        counters.allocations.fetch_add(1, std::memory_order_relaxed);
        counters.bytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

namespace {

using atlas::bench::allocation_counters;
using atlas::bench::Result;
using atlas::math::Mat4;
using atlas::math::Rect;
using atlas::platform::Platform;
using atlas::renderer::Quad;
using atlas::renderer::QuadBatch;
using atlas::rhi::Device;

struct Counted {
    std::uint64_t allocations = 0;
    std::uint64_t bytes = 0;
};

/// Run `body` with allocation counting on.
[[nodiscard]] Counted count_allocations(const std::function<void()>& body) {
    auto& counters = allocation_counters();
    counters.allocations.store(0, std::memory_order_relaxed);
    counters.bytes.store(0, std::memory_order_relaxed);
    counters.counting.store(true, std::memory_order_relaxed);

    body();

    counters.counting.store(false, std::memory_order_relaxed);
    return Counted{.allocations = counters.allocations.load(std::memory_order_relaxed),
                   .bytes = counters.bytes.load(std::memory_order_relaxed)};
}

[[nodiscard]] std::vector<Quad> make_quads(std::size_t count) {
    std::vector<Quad> quads;
    quads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto x = static_cast<float>(i % 1000) * 4.0F;
        const std::size_t row = i / 1000;  // deliberately integral: it is a row index
        const auto y = static_cast<float>(row) * 4.0F;
        quads.push_back(Quad{.bounds = Rect{.position = {x, y}, .size = {3.0F, 3.0F}}});
    }
    return quads;
}

[[nodiscard]] std::vector<Result> run() {
    std::vector<Result> results;

    auto platform = Platform::create({.video = true});
    if (!platform) {
        std::printf("allocations: skipped, no platform\n");
        return results;
    }
    auto window = platform->create_window(
        {.title = "Atlas allocations", .width = 640, .height = 480, .resizable = false});
    if (!window) {
        std::printf("allocations: skipped, no window\n");
        return results;
    }
    auto device = Device::create({.debug = false}, *window);
    if (!device) {
        std::printf("allocations: skipped, no graphics device\n");
        return results;
    }

    auto texture = device->create_texture({.width = 1,
                                           .height = 1,
                                           .format = atlas::rhi::TextureFormat::Rgba8Unorm,
                                           .debug_name = "alloc white"});
    const std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255},
                                         std::byte{255}};
    if (!texture || !device->upload_texture(*texture, white)) {
        std::printf("allocations: skipped, no texture\n");
        return results;
    }
    auto sampler = device->create_sampler({.debug_name = "alloc sampler"});
    if (!sampler) {
        std::printf("allocations: skipped, no sampler\n");
        return results;
    }

    auto batch = QuadBatch::create(*device, {.capacity = 65536});
    if (!batch) {
        std::printf("allocations: skipped, %s\n", batch.error().to_string().c_str());
        return results;
    }

    const auto draw_frame = [&](const std::vector<Quad>& quads) {
        auto frame = device->begin_frame();
        if (!frame || !frame->has_swapchain_target()) {
            if (frame) {
                (void)device->end_frame(std::move(*frame));
            }
            return false;
        }
        auto pass = frame->begin_render_pass({});
        if (!pass) {
            (void)device->end_frame(std::move(*frame));
            return false;
        }
        batch->begin(*pass, Mat4::identity());
        batch->set_texture(*texture, *sampler);
        batch->add(quads);
        (void)batch->end();
        pass->end();
        (void)device->end_frame(std::move(*frame));
        return true;
    };

    // Two scene sizes, so that an allocation count which grows with the scene is visible.
    // A fixed count is survivable; one proportional to the quads is not.
    std::printf("\nsteady-state allocations per frame\n");
    std::printf("%-16s %14s %14s\n", "scene", "allocations", "bytes");
    std::printf("%s\n", std::string(48, '-').c_str());

    std::uint64_t small_allocations = 0;
    std::uint64_t large_allocations = 0;

    for (const std::size_t count : {std::size_t{1000}, std::size_t{50000}}) {
        const auto quads = make_quads(count);

        // Warm up: the first frames grow buffers, fault in pages and populate caches. That
        // is startup cost, and counting it would measure the wrong thing.
        bool ready = false;
        for (int i = 0; i < 40; ++i) {
            ready = draw_frame(quads) || ready;
        }
        if (!ready) {
            std::printf("allocations: skipped %zu, no swapchain image\n", count);
            continue;
        }

        // Several frames, so that one unlucky frame does not stand for all of them.
        constexpr int kFrames = 20;
        const auto counted = count_allocations([&] {
            for (int i = 0; i < kFrames; ++i) {
                (void)draw_frame(quads);
            }
        });

        const std::uint64_t per_frame = counted.allocations / kFrames;
        const std::uint64_t bytes_per_frame = counted.bytes / kFrames;
        std::printf("%-16s %14llu %14llu\n", std::format("{} quads", count).c_str(),
                    static_cast<unsigned long long>(per_frame),
                    static_cast<unsigned long long>(bytes_per_frame));

        if (count == 1000) {
            small_allocations = per_frame;
        } else {
            large_allocations = per_frame;
        }

        Result result;
        result.name = "renderer/allocations_per_frame";
        result.parameters = std::format("{} quads", count);
        result.iterations = kFrames;
        result.warmup_iterations = 40;
        // Recorded in the same fields so that the baseline tool can watch these for
        // regressions without a second mechanism, and marked as a count so that nothing
        // reports eight allocations as a duration.
        result.median_ns = per_frame;
        result.p90_ns = per_frame;
        result.p99_ns = per_frame;
        result.units_per_iteration = 0;
        result.metric = atlas::bench::Metric::Count;
        result.count_name = "allocs/frame";
        results.push_back(std::move(result));
    }

    // The claim under test. Fifty times the quads must not mean fifty times the
    // allocations; a fixed overhead is fine.
    if (small_allocations > 0 || large_allocations > 0) {
        const bool bounded = large_allocations <= small_allocations + 8;
        std::printf("\n%s: %llu allocations per frame at 1k quads, %llu at 50k\n",
                    bounded ? "bounded" : "NOT BOUNDED",
                    static_cast<unsigned long long>(small_allocations),
                    static_cast<unsigned long long>(large_allocations));
        if (!bounded) {
            std::printf("  Allocation count grows with the scene, so a larger scene pays\n"
                        "  more every frame for ever. See docs/PERFORMANCE.md.\n");
        }
    }

    device->destroy_sampler(*sampler);
    device->destroy_texture(*texture);
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("allocations", run);

}  // namespace
