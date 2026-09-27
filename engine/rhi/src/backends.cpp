// SPDX-License-Identifier: GPL-3.0-or-later
#include "backends.hpp"

#include <SDL3/SDL_gpu.h>

namespace atlas::rhi::detail {

std::vector<ShaderFormat> requested_shader_formats() {
    return {kShippedShaderFormats.begin(), kShippedShaderFormats.end()};
}

std::vector<std::string> compiled_backends() {
    std::vector<std::string> names;
    const int count = SDL_GetNumGPUDrivers();
    for (int i = 0; i < count; ++i) {
        if (const char* name = SDL_GetGPUDriver(i); name != nullptr) {
            names.emplace_back(name);
        }
    }
    return names;
}

std::vector<ShaderFormat> backend_shader_formats(std::string_view backend) {
    // Which compiled representation each of SDL's backends consumes. Metal also takes a
    // precompiled library, and Direct3D 12 also takes DXBC; Atlas names neither.
    if (backend == "vulkan") {
        return {ShaderFormat::SpirV};
    }
    if (backend == "metal") {
        return {ShaderFormat::Msl};
    }
    if (backend == "direct3d12") {
        return {ShaderFormat::Dxil};
    }
    return {};
}

}  // namespace atlas::rhi::detail
