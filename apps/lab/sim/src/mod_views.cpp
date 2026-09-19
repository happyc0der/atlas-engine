// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/mod_views.hpp>

#include <bit>
#include <cstring>

namespace atlas::lab {
namespace {

void write_le(std::span<std::byte> out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::byte>(value & 0xFFU);
    out[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    out[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    out[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

}  // namespace

LayoutView layout_view(const GridLayout& layout) noexcept {
    LayoutView view;
    // Written a byte at a time rather than memcpy'd from a struct, so the bytes a guest sees are
    // the bytes named here whatever the host's endianness or padding happen to be.
    write_le(std::span(view.bytes).subspan(0, 4), layout.cell_count());
    write_le(std::span(view.bytes).subspan(4, 4), layout.width());
    write_le(std::span(view.bytes).subspan(8, 4), layout.height());
    return view;
}

std::span<const std::byte> color_view(const sim::World& world, const TableIds& ids) noexcept {
    const auto& cells = cell_table(world, ids);
    return std::as_bytes(std::span(cells.color_index));
}

}  // namespace atlas::lab
