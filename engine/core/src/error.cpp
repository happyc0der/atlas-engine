// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/error.hpp>

#include <format>
#include <string>
#include <string_view>

namespace atlas {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::NotSupported: return "NotSupported";
    case ErrorCode::Unavailable: return "Unavailable";
    case ErrorCode::Exhausted: return "Exhausted";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::IoFailure: return "IoFailure";

    case ErrorCode::PlatformInitFailed: return "PlatformInitFailed";
    case ErrorCode::WindowCreationFailed: return "WindowCreationFailed";
    case ErrorCode::DisplayUnavailable: return "DisplayUnavailable";

    case ErrorCode::GpuUnavailable: return "GpuUnavailable";
    case ErrorCode::GpuDeviceCreationFailed: return "GpuDeviceCreationFailed";
    case ErrorCode::ShaderCompilationFailed: return "ShaderCompilationFailed";
    case ErrorCode::PipelineCreationFailed: return "PipelineCreationFailed";
    case ErrorCode::ResourceCreationFailed: return "ResourceCreationFailed";
    case ErrorCode::DeviceLost: return "DeviceLost";

    case ErrorCode::AssetNotFound: return "AssetNotFound";
    case ErrorCode::AssetImportFailed: return "AssetImportFailed";
    case ErrorCode::AssetDecodeFailed: return "AssetDecodeFailed";

    case ErrorCode::SerializationFailed: return "SerializationFailed";
    case ErrorCode::VersionMismatch: return "VersionMismatch";
    case ErrorCode::IntegrityCheckFailed: return "IntegrityCheckFailed";
    case ErrorCode::MalformedData: return "MalformedData";
    }
    // No default case above, so that adding a code without a name is a compiler warning
    // (and therefore an error). This is reached only for a value cast from an integer.
    return "Unrecognised";
}

std::string_view to_string(ErrorDomain domain) noexcept {
    switch (domain) {
    case ErrorDomain::Generic: return "generic";
    case ErrorDomain::Platform: return "platform";
    case ErrorDomain::Gpu: return "gpu";
    case ErrorDomain::Asset: return "asset";
    case ErrorDomain::Serialization: return "serialization";
    }
    return "unrecognised";
}

Error& Error::context(std::string_view what) & {
    // Outermost context first, so the message reads as a narrowing path:
    //   "starting the renderer: creating the device: no supported backend"
    std::string combined;
    combined.reserve(what.size() + 2 + m_message.size());
    combined.append(what);
    combined.append(": ");
    combined.append(m_message);
    m_message = std::move(combined);
    return *this;
}

Error&& Error::context(std::string_view what) && {
    context(what);
    return std::move(*this);
}

std::string Error::to_string() const {
    if (m_native_code != 0) {
        return std::format("{} [{}] (native {})", m_message, atlas::to_string(m_code),
                           m_native_code);
    }
    return std::format("{} [{}]", m_message, atlas::to_string(m_code));
}

}  // namespace atlas
