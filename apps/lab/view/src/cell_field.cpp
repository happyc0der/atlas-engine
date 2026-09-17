// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/lab/cell_field.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace atlas::lab {
namespace {

constexpr log::Category kLab{"lab"};

[[nodiscard]] rhi::Colour hsv(float hue, float saturation, float value) noexcept {
    const float h = std::fmod(hue, 1.0F) * 6.0F;
    const auto sector = static_cast<int>(h);
    const float f = h - static_cast<float>(sector);
    const float p = value * (1.0F - saturation);
    const float q = value * (1.0F - (saturation * f));
    const float t = value * (1.0F - (saturation * (1.0F - f)));
    switch (sector % 6) {
    case 0: return {.r = value, .g = t, .b = p, .a = 1.0F};
    case 1: return {.r = q, .g = value, .b = p, .a = 1.0F};
    case 2: return {.r = p, .g = value, .b = t, .a = 1.0F};
    case 3: return {.r = p, .g = q, .b = value, .a = 1.0F};
    case 4: return {.r = t, .g = p, .b = value, .a = 1.0F};
    default: return {.r = value, .g = p, .b = q, .a = 1.0F};
    }
}

[[nodiscard]] Palette make_hue_wheel() {
    Palette palette;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        palette[i] = hsv(static_cast<float>(i) / 256.0F, 0.75F, 0.85F);
    }
    return palette;
}

/// Distinct hues spaced by the golden angle, so neighbours in index are far apart in hue.
[[nodiscard]] Palette make_categorical(std::size_t categories) {
    Palette palette;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const auto category = static_cast<float>(i % categories);
        palette[i] = hsv(category * 0.618'034F, 0.65F, 0.9F);
    }
    return palette;
}

[[nodiscard]] Palette make_heat() {
    Palette palette;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const float t = static_cast<float>(i) / 255.0F;
        // Black, red, yellow, white.
        palette[i] = {.r = std::min(1.0F, t * 3.0F),
                      .g = std::clamp((t * 3.0F) - 1.0F, 0.0F, 1.0F),
                      .b = std::clamp((t * 3.0F) - 2.0F, 0.0F, 1.0F),
                      .a = 1.0F};
    }
    return palette;
}

struct Palettes {
    Palette region = make_hue_wheel();
    Palette owner = make_categorical(kOwnerCount);
    Palette population = make_heat();
    Palette colour = make_categorical(kColorCount);
};

/// The same palettes packed as red-green-blue-alpha bytes, which is what a cell instance is.
/// Built once; the inner loop becomes a byte load and a four-byte store.
using PackedPalette = std::array<std::uint32_t, 256>;

[[nodiscard]] std::uint32_t pack(const rhi::Colour& colour) noexcept {
    const auto channel = [](float value) {
        // lround rather than a cast of value plus a half: the two agree for everything in
        // range here, and the idiom they disagree about is the one clang-tidy is right to
        // distrust.
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return channel(colour.r) | (channel(colour.g) << 8U) | (channel(colour.b) << 16U) |
           (channel(colour.a) << 24U);
}

[[nodiscard]] PackedPalette pack_palette(const Palette& palette) {
    PackedPalette packed{};
    for (std::size_t i = 0; i < packed.size(); ++i) {
        packed[i] = pack(palette[i]);
    }
    return packed;
}

const Palettes& palettes() {
    static const Palettes kPalettes;
    return kPalettes;
}

}  // namespace

namespace {

struct PackedPalettes {
    PackedPalette region = pack_palette(palettes().region);
    PackedPalette owner = pack_palette(palettes().owner);
    PackedPalette population = pack_palette(palettes().population);
    PackedPalette colour = pack_palette(palettes().colour);
};

[[nodiscard]] const PackedPalette& packed_palette_for(MapMode mode) {
    static const PackedPalettes kPacked;
    switch (mode) {
    case MapMode::RegionValue: return kPacked.region;
    case MapMode::OwnerIndex: return kPacked.owner;
    case MapMode::PopulationValue: return kPacked.population;
    case MapMode::ColorIndex:
    case MapMode::Count: break;
    }
    return kPacked.colour;
}

}  // namespace

const Palette& palette_for(MapMode mode) noexcept {
    switch (mode) {
    case MapMode::RegionValue: return palettes().region;
    case MapMode::OwnerIndex: return palettes().owner;
    case MapMode::PopulationValue: return palettes().population;
    case MapMode::ColorIndex:
    case MapMode::Count: break;
    }
    return palettes().colour;
}

Result<CellField> CellField::create(rhi::Device& device, const Config& config) {
    if (!(config.cell_size > 0.0F)) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "a cell needs a positive size in world units"));
    }
    CellField field;
    field.m_device = &device;
    field.m_cell_size = config.cell_size;

    auto renderer = CellRenderer::create(device, {.shader_directory = config.shader_directory});
    if (!renderer) {
        return std::unexpected(std::move(renderer).error().context("creating the cell renderer"));
    }
    field.m_renderer = std::move(*renderer);
    return field;
}

CellField::~CellField() {
    release();
}

CellField::CellField(CellField&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_renderer(std::move(other.m_renderer)),
      m_layout(other.m_layout), m_cell_size(other.m_cell_size), m_camera(other.m_camera),
      m_pixel_width(other.m_pixel_width), m_pixel_height(other.m_pixel_height),
      m_colours(std::move(other.m_colours)), m_visible(std::move(other.m_visible)),
      m_runs(std::move(other.m_runs)), m_camera_controls(other.m_camera_controls),
      m_camera_controller(other.m_camera_controller) {}

CellField& CellField::operator=(CellField&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_renderer = std::move(other.m_renderer);
        m_layout = other.m_layout;
        m_cell_size = other.m_cell_size;
        m_camera = other.m_camera;
        m_pixel_width = other.m_pixel_width;
        m_pixel_height = other.m_pixel_height;
        m_colours = std::move(other.m_colours);
        m_visible = std::move(other.m_visible);
        m_runs = std::move(other.m_runs);
        m_camera_controls = other.m_camera_controls;
        m_camera_controller = other.m_camera_controller;
    }
    return *this;
}

void CellField::release() noexcept {
    // The renderer owns the only device resources now: there is no per-cell geometry and no
    // white texel to multiply a colour by.
    m_device = nullptr;
}

Status CellField::set_layout(const GridLayout& layout) {
    m_layout = layout;
    if (auto status = m_renderer.set_layout(layout); !status) {
        return status;
    }
    // Sized for the whole grid, because the whole grid can be visible. Sized once so that a
    // frame neither allocates nor grows; there is no per-cell geometry to build.
    m_colours.assign(layout.cell_count(), 0);
    m_visible.clear();
    m_visible.reserve(layout.chunk_count());
    m_runs.clear();
    m_runs.reserve(layout.chunk_count());
    reset_camera();
    ATLAS_LOG_INFO(kLab, "cell field: {}x{} cells in {} chunks of {}, {} KiB of instance data",
                   layout.width(), layout.height(), layout.chunk_count(), layout.chunk_size(),
                   (static_cast<std::uint64_t>(layout.cell_count()) * bytes_per_cell()) / 1024);
    return ok();
}

void CellField::resize(std::uint32_t pixel_width, std::uint32_t pixel_height) {
    m_pixel_width = std::max(1U, pixel_width);
    m_pixel_height = std::max(1U, pixel_height);
    m_camera.set_viewport(static_cast<float>(m_pixel_width), static_cast<float>(m_pixel_height));
}

void CellField::reset_camera() noexcept {
    const float grid_w = static_cast<float>(m_layout.width()) * m_cell_size;
    const float grid_h = static_cast<float>(m_layout.height()) * m_cell_size;
    m_camera.set_centre({grid_w * 0.5F, grid_h * 0.5F});
    if (grid_w > 0.0F && grid_h > 0.0F) {
        const float fit = std::min(static_cast<float>(m_pixel_width) / grid_w,
                                   static_cast<float>(m_pixel_height) / grid_h);
        m_camera.set_zoom(fit * 0.95F);
    }
}

void CellField::update(const platform::InputState& input, std::span<const platform::Event> events,
                       float display_scale, float dt_seconds, bool mouse_allowed) {
    // One controller rather than a fourth copy of this: the same twenty lines lived in
    // three files, and M9 fixed one bug in four places because of it.
    m_camera_controller.apply(
        m_camera, app::sample_camera_input(input, events, m_camera_controls, mouse_allowed),
        display_scale, dt_seconds);
}

math::Rect CellField::chunk_bounds(std::uint32_t chunk) const noexcept {
    const CellCoords origin = m_layout.chunk_origin(chunk);
    const float extent = static_cast<float>(m_layout.chunk_size()) * m_cell_size;
    return {.position = {static_cast<float>(origin.x) * m_cell_size,
                         static_cast<float>(origin.y) * m_cell_size},
            .size = {extent, extent}};
}

void CellField::cull(std::vector<std::uint32_t>& out) const {
    out.clear();
    const math::Rect visible = m_camera.visible_bounds();
    for (std::uint32_t chunk = 0; chunk < m_layout.chunk_count(); ++chunk) {
        if (chunk_bounds(chunk).overlaps(visible)) {
            out.push_back(chunk);
        }
    }
}

CellField::DrawStats CellField::draw(rhi::RenderPass& pass, const CellSnapshot& snapshot,
                                     MapMode mode) {
    ATLAS_ZONE_NAMED("cell field draw");
    DrawStats stats;
    m_visible.clear();
    m_runs.clear();
    if (snapshot.layout != m_layout || m_layout.empty()) {
        return stats;
    }

    const auto values = band(snapshot, mode);
    const PackedPalette& palette = packed_palette_for(mode);
    cull(m_visible);

    // Consecutive chunks become one run, which is one contiguous range of cells and therefore
    // one draw. Fitted to the whole grid this is a single run over every chunk.
    for (const std::uint32_t chunk : m_visible) {
        if (!m_runs.empty() && m_runs.back().first_chunk + m_runs.back().chunk_count == chunk) {
            ++m_runs.back().chunk_count;
        } else {
            m_runs.push_back(ChunkRun{.first_chunk = chunk, .chunk_count = 1});
        }
    }

    // Written through a pointer into storage that is already the grid's size, rather than
    // pushed onto a vector: push_back's capacity check per cell cost more than the upload it
    // was feeding. The buffer is sized once in set_layout and never grows here.
    const std::uint32_t per_chunk = m_layout.cells_per_chunk();
    std::uint32_t used = 0;
    std::uint32_t* out = m_colours.data();
    for (const ChunkRun& run : m_runs) {
        const std::uint8_t* source = values.data() + m_layout.chunk_first_cell(run.first_chunk);
        const std::uint32_t cells = run.chunk_count * per_chunk;
        for (std::uint32_t i = 0; i < cells; ++i) {
            out[used + i] = palette[source[i]];
        }
        used += cells;
    }

    if (auto status =
            m_renderer.draw(pass, m_camera.view_projection(), m_layout, m_cell_size,
                            std::span<const std::uint32_t>{m_colours.data(), used}, m_runs);
        !status) {
        ATLAS_LOG_ERROR(kLab, "drawing the cell field failed: {}", status.error());
        return stats;
    }

    stats.cells = m_renderer.stats();
    stats.visible_chunks = static_cast<std::uint32_t>(m_visible.size());
    stats.visible_cells = stats.cells.cells;
    return stats;
}

std::optional<std::uint32_t> CellField::cell_at_screen(math::Vec2 screen) const noexcept {
    const math::Vec2 world = m_camera.screen_to_world(screen);
    if (world.x < 0.0F || world.y < 0.0F) {
        return std::nullopt;
    }
    const auto x = static_cast<std::uint64_t>(world.x / m_cell_size);
    const auto y = static_cast<std::uint64_t>(world.y / m_cell_size);
    if (x >= m_layout.width() || y >= m_layout.height()) {
        return std::nullopt;
    }
    return m_layout.cell_index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
}

}  // namespace atlas::lab
