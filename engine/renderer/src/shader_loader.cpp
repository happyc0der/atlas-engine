// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/renderer/shader_loader.hpp>

#include "shader_manifest.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace atlas::renderer {
namespace {

// maybe_unused because the only use in this file is a debug log, and debug logging compiles
// out in release builds.
[[maybe_unused]] constexpr log::Category kRenderer{"renderer"};

[[nodiscard]] Result<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(
            Error(ErrorCode::NotFound,
                  std::format("cannot read '{}': {}. Run tools/cook_shaders.py if the cooked "
                              "outputs are missing.",
                              path.string(), error.message())));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("cannot open '{}'", path.string())));
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): file reads are bytes.
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!stream) {
            return std::unexpected(Error(ErrorCode::MalformedData,
                                         std::format("short read from '{}'", path.string())));
        }
    }
    return bytes;
}

/// One compiled shader, chosen for what the device accepts.
[[nodiscard]] Result<rhi::ShaderHandle> load_one(rhi::Device& device,
                                                 const std::filesystem::path& directory,
                                                 const shaders::ShaderInfo& info,
                                                 rhi::ShaderStage stage) {
    // Metal Shading Language is shipped as source and compiled by the driver; SPIR-V is
    // consumed directly. DXIL is absent because no compiler for it runs on the development
    // machine; see ADR-0006.
    rhi::ShaderFormat format{};
    std::string_view file;
    std::string_view entry_point;

    if (device.supports_shader_format(rhi::ShaderFormat::Msl)) {
        format = rhi::ShaderFormat::Msl;
        file = info.msl_file;
        entry_point = info.msl_entry_point;
    } else if (device.supports_shader_format(rhi::ShaderFormat::SpirV)) {
        format = rhi::ShaderFormat::SpirV;
        file = info.spirv_file;
        entry_point = info.spirv_entry_point;
    } else {
        return std::unexpected(
            Error(ErrorCode::NotSupported,
                  std::format("the {} backend accepts neither SPIR-V nor Metal Shading Language, "
                              "and Atlas ships no other format",
                              device.backend_name())));
    }

    auto code = read_file(directory / std::string{file});
    if (!code) {
        return std::unexpected(std::move(code).error());
    }

    return device.create_shader({
        .stage = stage,
        .format = format,
        .code = *code,
        .entry_point = entry_point,
        // Straight from the generated manifest, which reflection produced from the compiled
        // shader. A hand-written count here would be one more thing able to go stale.
        .samplers = info.samplers,
        .storage_textures = info.storage_textures,
        .storage_buffers = info.storage_buffers,
        .uniform_buffers = info.uniform_buffers,
        .debug_name = info.name,
    });
}

/// The generated entry for a shader, by name and stage.
[[nodiscard]] const shaders::ShaderInfo* find(std::string_view name, std::string_view stage) {
    for (const auto* info : shaders::kAll) {
        if (info->name == name && info->stage == stage) {
            return info;
        }
    }
    return nullptr;
}

}  // namespace

Result<ShaderPair> load_shader_pair(rhi::Device& device, std::string_view directory,
                                    std::string_view name) {
    const auto* vertex_info = find(name, "vertex");
    const auto* fragment_info = find(name, "fragment");
    if (vertex_info == nullptr || fragment_info == nullptr) {
        return std::unexpected(Error(
            ErrorCode::NotFound, std::format("no cooked shader named '{}' with both stages; run "
                                             "tools/cook_shaders.py",
                                             name)));
    }

    const std::filesystem::path path{directory};

    auto vertex = load_one(device, path, *vertex_info, rhi::ShaderStage::Vertex);
    if (!vertex) {
        return std::unexpected(std::move(vertex).error());
    }

    auto fragment = load_one(device, path, *fragment_info, rhi::ShaderStage::Fragment);
    if (!fragment) {
        // Do not leak the one that did load.
        device.destroy_shader(*vertex);
        return std::unexpected(std::move(fragment).error());
    }

    ATLAS_LOG_DEBUG(kRenderer, "loaded shader '{}' from '{}'", name, path.string());
    return ShaderPair{.vertex = *vertex, .fragment = *fragment};
}

}  // namespace atlas::renderer
