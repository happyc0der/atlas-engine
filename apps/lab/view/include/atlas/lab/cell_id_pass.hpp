// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Picking through an integer-identifier target.
///
/// One draw per visible chunk into an R32Uint texture the size of the window, with no vertex
/// buffer at all: the shader derives each cell's rectangle from its instance index and a
/// per-chunk uniform, and its identifier from the instance index plus the chunk's first cell.
/// The target clears to zero and identifiers start at one, so zero means "nothing here".
///
/// Order matters. The pass is recorded into a frame; the readback is requested only after
/// that frame has been submitted, so the copy is queued behind the draw. The caller owns that
/// sequence, which is why render() and request_pixel() are separate.
///
/// Thread affinity: main thread; it owns device resources.

#include <atlas/core/result.hpp>
#include <atlas/lab/cell_field.hpp>
#include <atlas/rhi/device.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace atlas::lab {

class CellIdPass {
  public:
    struct Config {
        std::string_view shader_directory = "assets/cooked/shaders";
    };

    [[nodiscard]] static Result<CellIdPass> create(rhi::Device& device, const Config& config);

    CellIdPass() = default;
    ~CellIdPass();
    CellIdPass(const CellIdPass&) = delete;
    CellIdPass& operator=(const CellIdPass&) = delete;
    CellIdPass(CellIdPass&& other) noexcept;
    CellIdPass& operator=(CellIdPass&& other) noexcept;

    /// (Re)create the target at the window's pixel size, so screen coordinates map onto it
    /// one to one. Failure: whatever create_texture reports; the old target is kept.
    [[nodiscard]] Status resize(std::uint32_t pixel_width, std::uint32_t pixel_height);

    /// Record one pass drawing `chunks` of `field` into the target.
    /// Failure: whatever begin_render_pass reports.
    [[nodiscard]] Status render(rhi::Frame& frame, const CellField& field,
                                std::span<const std::uint32_t> chunks);

    /// A 1x1 readback at a pixel. Call after the frame that rendered has been submitted.
    /// Failure: OutOfRange for a pixel outside the target, else whatever request_readback says.
    [[nodiscard]] Result<rhi::ReadbackHandle> request_pixel(std::uint32_t x, std::uint32_t y);

    /// The cell a readback names, or none if it read zero.
    [[nodiscard]] static std::optional<std::uint32_t> decode(const rhi::Device::Readback& pixel);

    [[nodiscard]] rhi::TextureHandle target() const noexcept { return m_target; }

    [[nodiscard]] rhi::Extent2D target_extent() const noexcept { return m_extent; }

  private:
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    rhi::ShaderHandle m_vertex;
    rhi::ShaderHandle m_fragment;
    rhi::GraphicsPipelineHandle m_pipeline;
    rhi::TextureHandle m_target;
    rhi::Extent2D m_extent;
};

}  // namespace atlas::lab
