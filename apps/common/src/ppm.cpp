// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/ppm.hpp>

#include <format>
#include <fstream>
#include <string>

namespace atlas::app {

Status write_ppm(const std::filesystem::path& path, const rhi::Device::Capture& capture) {
    if (capture.pixels.empty() || capture.extent.width == 0 || capture.extent.height == 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "nothing was captured"));
    }

    const bool bgra = capture.format == rhi::TextureFormat::Bgra8Unorm ||
                      capture.format == rhi::TextureFormat::Bgra8UnormSrgb;

    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(Error(ErrorCode::PermissionDenied,
                                     std::format("cannot open '{}' for writing", path.string())));
    }

    stream << "P6\n" << capture.extent.width << ' ' << capture.extent.height << "\n255\n";

    const std::size_t pixel_count =
        static_cast<std::size_t>(capture.extent.width) * capture.extent.height;
    std::string rows;
    rows.resize(pixel_count * 3);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        const auto* pixel = &capture.pixels[i * 4];
        const auto r = static_cast<unsigned char>(pixel[bgra ? 2 : 0]);
        const auto g = static_cast<unsigned char>(pixel[1]);
        const auto b = static_cast<unsigned char>(pixel[bgra ? 0 : 2]);
        rows[(i * 3) + 0] = static_cast<char>(r);
        rows[(i * 3) + 1] = static_cast<char>(g);
        rows[(i * 3) + 2] = static_cast<char>(b);
    }

    stream.write(rows.data(), static_cast<std::streamsize>(rows.size()));
    if (!stream) {
        return std::unexpected(
            Error(ErrorCode::IoFailure, std::format("writing '{}' failed", path.string())));
    }
    return ok();
}

}  // namespace atlas::app
