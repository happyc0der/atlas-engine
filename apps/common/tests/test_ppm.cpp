// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/ppm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using atlas::ErrorCode;
using atlas::app::write_ppm;
using atlas::rhi::Device;
using atlas::rhi::TextureFormat;

namespace {

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::filesystem::path scratch(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

TEST_CASE("a BGRA capture is written as RGB", "[app][ppm]") {
    // Two pixels: pure red, pure blue, stored BGRA as the swapchain usually is. If the
    // channel swap were wrong the two would come out swapped, and every screenshot ever
    // written would have been wrong the same way.
    Device::Capture capture;
    capture.extent = {2, 1};
    capture.format = TextureFormat::Bgra8Unorm;
    capture.pixels = {
        std::byte{0},   std::byte{0}, std::byte{255}, std::byte{255},  // B G R A = red
        std::byte{255}, std::byte{0}, std::byte{0},   std::byte{255},  // B G R A = blue
    };

    const auto path = scratch("atlas-ppm-bgra.ppm");
    REQUIRE(write_ppm(path, capture).has_value());

    const std::string bytes = read_all(path);
    const std::string header = "P6\n2 1\n255\n";
    REQUIRE(bytes.starts_with(header));
    const std::string body = bytes.substr(header.size());
    REQUIRE(body.size() == 6);
    CHECK(static_cast<unsigned char>(body[0]) == 255);  // red pixel: R
    CHECK(static_cast<unsigned char>(body[2]) == 0);    //            B
    CHECK(static_cast<unsigned char>(body[3]) == 0);    // blue pixel: R
    CHECK(static_cast<unsigned char>(body[5]) == 255);  //             B
    std::filesystem::remove(path);
}

TEST_CASE("an RGBA capture is written without reordering", "[app][ppm]") {
    Device::Capture capture;
    capture.extent = {1, 1};
    capture.format = TextureFormat::Rgba8Unorm;
    capture.pixels = {std::byte{10}, std::byte{20}, std::byte{30}, std::byte{255}};

    const auto path = scratch("atlas-ppm-rgba.ppm");
    REQUIRE(write_ppm(path, capture).has_value());
    const std::string body = read_all(path).substr(std::string("P6\n1 1\n255\n").size());
    REQUIRE(body.size() == 3);
    CHECK(static_cast<unsigned char>(body[0]) == 10);
    CHECK(static_cast<unsigned char>(body[1]) == 20);
    CHECK(static_cast<unsigned char>(body[2]) == 30);
    std::filesystem::remove(path);
}

TEST_CASE("an empty capture is refused", "[app][ppm]") {
    const Device::Capture empty;
    const auto status = write_ppm(scratch("atlas-ppm-empty.ppm"), empty);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::InvalidArgument);
}
