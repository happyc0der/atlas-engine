// SPDX-License-Identifier: GPL-3.0-or-later
//
// Camera and matrix work. Cheap, and on the path of every frame, so worth a number.
#include <atlas/math/camera.hpp>
#include <atlas/math/matrix.hpp>

#include "harness.hpp"

#include <vector>

namespace {

using atlas::bench::measure;
using atlas::bench::Result;
using atlas::math::Mat4;
using atlas::math::OrthoCamera;
using atlas::math::Vec2;
using atlas::math::Vec4;

[[nodiscard]] std::vector<Result> run() {
    std::vector<Result> results;

    {
        // Recomputing the view projection happens once per frame per camera, and whenever
        // the camera moves, which while panning is every frame.
        OrthoCamera camera;
        camera.set_viewport(1920.0F, 1080.0F);
        float offset = 0.0F;

        auto result = measure("camera/view_projection", "1 camera", 20000, 2000, [&] {
            offset += 0.01F;
            camera.set_centre(Vec2{offset, offset});
            volatile float sink = camera.view_projection().at(0, 0);
            (void)sink;
        });
        result.units_per_iteration = 1;
        result.unit_name = "updates";
        results.push_back(std::move(result));
    }

    {
        // Transforming points is what culling and picking do in bulk.
        constexpr std::size_t kPoints = 10000;
        std::vector<Vec4> points;
        points.reserve(kPoints);
        for (std::size_t i = 0; i < kPoints; ++i) {
            points.push_back(Vec4{static_cast<float>(i), static_cast<float>(i) * 0.5F, 0.0F, 1.0F});
        }

        OrthoCamera camera;
        camera.set_viewport(1920.0F, 1080.0F);
        const Mat4 projection = camera.view_projection();

        auto result = measure("math/transform_points", "10k points", 2000, 200, [&] {
            float accumulator = 0.0F;
            for (const auto& point : points) {
                accumulator += (projection * point).x;
            }
            volatile float sink = accumulator;
            (void)sink;
        });
        result.units_per_iteration = kPoints;
        result.unit_name = "points";
        results.push_back(std::move(result));
    }

    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("math", run);

}  // namespace
