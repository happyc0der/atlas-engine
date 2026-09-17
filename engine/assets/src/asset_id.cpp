// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/asset_id.hpp>
#include <atlas/assets/asset_state.hpp>

namespace atlas::assets {

std::string_view to_string(AssetType type) noexcept {
    switch (type) {
    case AssetType::Unknown: return "unknown";
    case AssetType::Texture: return "texture";
    case AssetType::Shader: return "shader";
    case AssetType::AudioClip: return "audio clip";
    }
    return "unrecognised";
}

std::string_view to_string(AssetState state) noexcept {
    switch (state) {
    case AssetState::Unloaded: return "unloaded";
    case AssetState::Queued: return "queued";
    case AssetState::Loading: return "loading";
    case AssetState::Decoded: return "decoded";
    case AssetState::Ready: return "ready";
    case AssetState::Failed: return "failed";
    }
    return "unrecognised";
}

std::string AssetId::to_string() const {
    return atlas::to_hex(m_value);
}

}  // namespace atlas::assets
