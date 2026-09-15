// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/importer.hpp>
#include <atlas/core/log.hpp>

#include <cstring>
#include <format>
#include <limits>

// stb_image is a single-header decoder. The implementation is compiled exactly once, here.
//
// Its failure mode is a null return plus a reason, which suits the error model; what it does
// not do is bound its own allocations, so the dimensions are checked against the limits
// before anything is decoded rather than afterwards.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO  // Atlas reads through the virtual filesystem, never by path.
#define STBI_NO_HDR    // Not needed, and its code paths are extra attack surface.
#define STBI_NO_LINEAR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include <stb_image.h>

namespace atlas::assets {
namespace {

constexpr log::Category kAssets{"assets"};

const ImportLimits kLimits{};

}  // namespace

const ImportLimits& import_limits() noexcept {
    return kLimits;
}

Result<ImportedTexture> import_texture(std::span<const std::byte> bytes,
                                       std::string_view debug_name) {
    if (bytes.empty()) {
        return std::unexpected(
            Error(ErrorCode::AssetDecodeFailed,
                  std::format("'{}' is empty, so there is nothing to decode", debug_name)));
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        // The decoder takes an int length. A file this large is not a texture.
        return std::unexpected(
            Error(ErrorCode::AssetDecodeFailed,
                  std::format("'{}' is {} bytes, larger than the decoder accepts", debug_name,
                              bytes.size())));
    }

    // Read the header first, so that a file claiming enormous dimensions is refused before
    // anything is allocated rather than after.
    int width = 0;
    int height = 0;
    int channels = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the decoder wants bytes.
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int length = static_cast<int>(bytes.size());

    if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0) {
        const char* reason = stbi_failure_reason();
        return std::unexpected(
            Error(ErrorCode::AssetDecodeFailed,
                  std::format("'{}' is not an image this build can read: {}", debug_name,
                              reason != nullptr ? reason : "unrecognised format")));
    }

    if (width <= 0 || height <= 0) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("'{}' claims a size of {}x{}", debug_name, width, height)));
    }

    const auto unsigned_width = static_cast<std::uint32_t>(width);
    const auto unsigned_height = static_cast<std::uint32_t>(height);
    const auto& limits = import_limits();

    if (unsigned_width > limits.max_texture_dimension ||
        unsigned_height > limits.max_texture_dimension) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("'{}' is {}x{}, beyond the limit of {} on either axis", debug_name,
                              unsigned_width, unsigned_height, limits.max_texture_dimension)));
    }

    const std::uint64_t decoded_bytes =
        static_cast<std::uint64_t>(unsigned_width) * unsigned_height * 4;
    if (decoded_bytes > limits.max_texture_bytes) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("'{}' would decode to {} bytes, beyond the limit of {}", debug_name,
                              decoded_bytes, limits.max_texture_bytes)));
    }

    // Four channels always. Every format the engine draws with is four-channel, and
    // converting here is cheaper than branching at every use.
    int actual_channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(data, length, &width, &height, &actual_channels, 4);
    if (decoded == nullptr) {
        const char* reason = stbi_failure_reason();
        return std::unexpected(Error(ErrorCode::AssetDecodeFailed,
                                     std::format("decoding '{}' failed: {}", debug_name,
                                                 reason != nullptr ? reason : "no reason given")));
    }

    ImportedTexture texture;
    texture.width = unsigned_width;
    texture.height = unsigned_height;
    texture.pixels.resize(static_cast<std::size_t>(decoded_bytes));
    std::memcpy(texture.pixels.data(), decoded, texture.pixels.size());

    stbi_image_free(decoded);

    ATLAS_LOG_DEBUG(kAssets, "decoded '{}': {}x{}, {} source channels", debug_name, unsigned_width,
                    unsigned_height, actual_channels);
    return texture;
}

}  // namespace atlas::assets
