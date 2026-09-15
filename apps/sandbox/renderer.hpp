// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The sandbox's triangle renderer.
///
/// Lives in the application rather than in an engine module on purpose: at M2 there is one
/// pipeline and one draw, and an engine-level renderer built around that would be an
/// abstraction shaped by a single example. The renderer module arrives in M3, when a camera
/// and batching give it two.

#include <atlas/core/result.hpp>
#include <atlas/rhi/device.hpp>

#include <string_view>

namespace atlas::sandbox {

/// Loads the cooked triangle shaders and draws them.
///
/// Owns its graphics resources and releases them in the destructor, before the device it
/// borrowed goes away. The device must outlive it.
class TriangleRenderer {
  public:
    [[nodiscard]] static Result<TriangleRenderer> create(rhi::Device& device,
                                                         std::string_view shader_directory);

    ~TriangleRenderer();

    TriangleRenderer(const TriangleRenderer&) = delete;
    TriangleRenderer& operator=(const TriangleRenderer&) = delete;
    TriangleRenderer(TriangleRenderer&& other) noexcept;
    TriangleRenderer& operator=(TriangleRenderer&& other) noexcept;

    /// Record the triangle into an open render pass.
    void draw(rhi::RenderPass& pass) const;

  private:
    TriangleRenderer() = default;
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    rhi::ShaderHandle m_vertex;
    rhi::ShaderHandle m_fragment;
    rhi::GraphicsPipelineHandle m_pipeline;
};

}  // namespace atlas::sandbox
