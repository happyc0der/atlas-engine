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

const Palettes& palettes() {
    static const Palettes kPalettes;
    return kPalettes;
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

    auto batch = renderer::QuadBatch::create(device, {.shader_directory = config.shader_directory});
    if (!batch) {
        return std::unexpected(std::move(batch).error().context("creating the cell batch"));
    }
    field.m_batch = std::move(*batch);

    // One white texel: the sprite shader multiplies the texture by the instance colour, so a
    // white texture makes the colour the whole story.
    auto white = device.create_texture({.width = 1, .height = 1, .debug_name = "cell white"});
    if (!white) {
        return std::unexpected(std::move(white).error());
    }
    field.m_white = *white;
    constexpr std::array<std::byte, 4> kWhite{std::byte{255}, std::byte{255}, std::byte{255},
                                              std::byte{255}};
    if (auto status = device.upload_texture(field.m_white, kWhite); !status) {
        return std::unexpected(std::move(status).error().context("uploading the white texel"));
    }
    auto sampler = device.create_sampler({.min_filter = rhi::Filter::Nearest,
                                          .mag_filter = rhi::Filter::Nearest,
                                          .debug_name = "cell sampler"});
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    field.m_sampler = *sampler;
    return field;
}

CellField::~CellField() {
    release();
}

CellField::CellField(CellField&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_batch(std::move(other.m_batch)),
      m_white(std::exchange(other.m_white, {})), m_sampler(std::exchange(other.m_sampler, {})),
      m_layout(other.m_layout), m_cell_size(other.m_cell_size), m_camera(other.m_camera),
      m_pixel_width(other.m_pixel_width), m_pixel_height(other.m_pixel_height),
      m_quads(std::move(other.m_quads)), m_scratch(std::move(other.m_scratch)),
      m_visible(std::move(other.m_visible)), m_dragging(other.m_dragging) {}

CellField& CellField::operator=(CellField&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_batch = std::move(other.m_batch);
        m_white = std::exchange(other.m_white, {});
        m_sampler = std::exchange(other.m_sampler, {});
        m_layout = other.m_layout;
        m_cell_size = other.m_cell_size;
        m_camera = other.m_camera;
        m_pixel_width = other.m_pixel_width;
        m_pixel_height = other.m_pixel_height;
        m_quads = std::move(other.m_quads);
        m_scratch = std::move(other.m_scratch);
        m_visible = std::move(other.m_visible);
        m_dragging = other.m_dragging;
    }
    return *this;
}

void CellField::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    m_device->destroy_sampler(m_sampler);
    m_device->destroy_texture(m_white);
    m_device = nullptr;
}

void CellField::set_layout(const GridLayout& layout) {
    ATLAS_ZONE_NAMED("cell field geometry");
    m_layout = layout;
    m_quads.resize(layout.cell_count());
    for (std::uint32_t i = 0; i < layout.cell_count(); ++i) {
        const CellCoords at = layout.cell_coords(i);
        m_quads[i].bounds = {
            .position = {static_cast<float>(at.x) * m_cell_size,
                         static_cast<float>(at.y) * m_cell_size},
            .size = {m_cell_size, m_cell_size},
        };
    }
    m_scratch.resize(layout.cells_per_chunk());
    m_visible.clear();
    m_visible.reserve(layout.chunk_count());
    reset_camera();
    ATLAS_LOG_INFO(kLab, "cell field: {}x{} cells in {} chunks of {}, {} KiB of geometry",
                   layout.width(), layout.height(), layout.chunk_count(), layout.chunk_size(),
                   (m_quads.size() * sizeof(renderer::Quad)) / 1024);
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
                       float display_scale) {
    if (input.was_pressed(platform::MouseButton::Right)) {
        m_dragging = true;
    }
    if (input.was_released(platform::MouseButton::Right)) {
        m_dragging = false;
    }
    if (m_dragging) {
        const float zoom = m_camera.zoom();
        m_camera.pan({-input.mouse_delta_x() * display_scale / zoom,
                      -input.mouse_delta_y() * display_scale / zoom});
    }
    for (const auto& event : events) {
        const auto* wheel = std::get_if<platform::MouseWheel>(&event);
        if (wheel == nullptr || wheel->delta_y == 0.0F) {
            continue;
        }
        const float factor = std::pow(1.15F, wheel->delta_y);
        const auto pointer = input.mouse_position();
        m_camera.zoom_about(factor,
                            math::Vec2{pointer.x * display_scale, pointer.y * display_scale});
    }
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
    if (snapshot.layout != m_layout || m_quads.empty()) {
        return stats;
    }

    const auto values = band(snapshot, mode);
    const Palette& palette = palette_for(mode);
    cull(m_visible);

    m_batch.begin(pass, m_camera.view_projection());
    m_batch.set_texture(m_white, m_sampler);
    const std::uint32_t per_chunk = m_layout.cells_per_chunk();
    for (const std::uint32_t chunk : m_visible) {
        const std::uint32_t first = m_layout.chunk_first_cell(chunk);
        for (std::uint32_t i = 0; i < per_chunk; ++i) {
            renderer::Quad& quad = m_scratch[i];
            quad.bounds = m_quads[first + i].bounds;
            quad.colour = palette[values[first + i]];
        }
        m_batch.add(std::span<const renderer::Quad>{m_scratch});
    }
    stats.batch = m_batch.end();
    stats.visible_chunks = static_cast<std::uint32_t>(m_visible.size());
    stats.visible_cells = stats.visible_chunks * per_chunk;
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
