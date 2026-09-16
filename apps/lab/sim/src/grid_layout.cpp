// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/grid_layout.hpp>

#include <format>

namespace atlas::lab {

Result<GridLayout> GridLayout::create(std::uint32_t width, std::uint32_t height,
                                      std::uint32_t chunk_size) {
    if (width == 0 || height == 0 || chunk_size == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("a grid needs non-zero dimensions, got {}x{} in chunks of {}", width,
                              height, chunk_size)));
    }
    if (chunk_size > kMaxChunkSize) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("chunk size {} exceeds the limit of {}", chunk_size, kMaxChunkSize)));
    }
    if (width % chunk_size != 0 || height % chunk_size != 0) {
        return std::unexpected(Error(
            ErrorCode::InvalidArgument,
            std::format("a grid must be a whole number of chunks: {}x{} is not divisible by {}",
                        width, height, chunk_size)));
    }
    const std::uint64_t cells = static_cast<std::uint64_t>(width) * height;
    if (cells > kMaxCells) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, std::format("{}x{} is {} cells, over the limit of {}",
                                                          width, height, cells, kMaxCells)));
    }
    return GridLayout{width, height, chunk_size};
}

}  // namespace atlas::lab
