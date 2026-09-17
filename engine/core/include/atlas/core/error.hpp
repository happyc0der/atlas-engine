// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Error type used throughout Atlas. See docs/adr/0005-error-and-ownership-model.md.
///
/// An Error describes a *recoverable* failure: a missing file, a malformed asset, a device
/// that cannot be created. Violated programmer invariants are not errors; they are bugs,
/// and they use ATLAS_ASSERT from <atlas/core/assert.hpp>.

#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace atlas {

/// Error codes, blocked by module so that the domain can be derived from the code.
///
/// Block 0 is generic, 100 is platform, 200 is GPU, 300 is assets, 400 is serialization.
/// Codes are stable: they may be added, but an existing code never changes meaning.
enum class ErrorCode : std::uint32_t {
    Unknown = 0,
    InvalidArgument = 1,
    OutOfRange = 2,
    NotFound = 3,
    AlreadyExists = 4,
    NotSupported = 5,
    Unavailable = 6,
    Exhausted = 7,
    PermissionDenied = 8,
    Cancelled = 9,
    Internal = 10,
    /// A file could not be opened, read or written. Distinct from `NotFound`, which says the
    /// path is wrong; this says the path was right and the operation still failed.
    IoFailure = 11,

    PlatformInitFailed = 100,
    WindowCreationFailed = 101,
    DisplayUnavailable = 102,

    GpuUnavailable = 200,
    GpuDeviceCreationFailed = 201,
    ShaderCompilationFailed = 202,
    PipelineCreationFailed = 203,
    ResourceCreationFailed = 204,
    DeviceLost = 205,

    AssetNotFound = 300,
    AssetImportFailed = 301,
    AssetDecodeFailed = 302,

    SerializationFailed = 400,
    VersionMismatch = 401,
    IntegrityCheckFailed = 402,
    MalformedData = 403,

    /// The audio subsystem could not start at all.
    AudioInitFailed = 500,
    /// There is no output device, or the one there is refused to open.
    AudioDeviceUnavailable = 501,
    /// Audio data the engine cannot represent: too many channels, an impossible rate, a
    /// sample format with no conversion.
    AudioFormatUnsupported = 502,
};

/// Coarse domain of an error, derived from its code rather than stored separately.
enum class ErrorDomain : std::uint8_t { Generic, Platform, Gpu, Asset, Serialization, Audio };

/// The ladder is open-ended at the top, so **a new block must add its rung above the previous
/// one**. Before M12 the top rung was `>= 400`, which meant a 500 code reported itself as a
/// serialization error: numerically free, semantically wrong, and silent. Adding a block is
/// three edits — the codes, an enumerator here, a rung, and the two `to_string` cases.
[[nodiscard]] constexpr ErrorDomain error_domain(ErrorCode code) noexcept {
    const auto value = static_cast<std::uint32_t>(code);
    if (value >= 500) {
        return ErrorDomain::Audio;
    }
    if (value >= 400) {
        return ErrorDomain::Serialization;
    }
    if (value >= 300) {
        return ErrorDomain::Asset;
    }
    if (value >= 200) {
        return ErrorDomain::Gpu;
    }
    if (value >= 100) {
        return ErrorDomain::Platform;
    }
    return ErrorDomain::Generic;
}

/// Stable, human-readable name of a code. Intended for logs, not for parsing.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;
[[nodiscard]] std::string_view to_string(ErrorDomain domain) noexcept;

/// A recoverable failure, with enough context to act on.
///
/// Errors accumulate context as they propagate: a decode failure deep in an importer
/// reaches the caller describing what was being loaded, not merely that something failed.
/// Construction allocates, which is acceptable on a failure path and is the reason errors
/// must not be produced per element inside a hot loop.
class Error {
  public:
    Error(ErrorCode code, std::string message,
          std::source_location where = std::source_location::current())
        : m_code(code), m_message(std::move(message)), m_where(where) {}

    explicit Error(ErrorCode code, std::source_location where = std::source_location::current())
        : Error(code, std::string(atlas::to_string(code)), where) {}

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }

    [[nodiscard]] ErrorDomain domain() const noexcept { return error_domain(m_code); }

    [[nodiscard]] const std::string& message() const noexcept { return m_message; }

    [[nodiscard]] const std::source_location& where() const noexcept { return m_where; }

    /// Native error code from a third-party API, when one was involved. Zero means none.
    [[nodiscard]] std::int64_t native_code() const noexcept { return m_native_code; }

    Error& with_native_code(std::int64_t value) & noexcept {
        m_native_code = value;
        return *this;
    }

    Error&& with_native_code(std::int64_t value) && noexcept {
        m_native_code = value;
        return std::move(*this);
    }

    /// Prefix the message with what was being attempted.
    ///
    /// The result reads outermost-first: "loading terrain.png: decoding PNG: truncated".
    Error& context(std::string_view what) &;
    Error&& context(std::string_view what) &&;

    /// Message including the error code name. Excludes the source location, which belongs
    /// in the log record rather than in the message text.
    [[nodiscard]] std::string to_string() const;

  private:
    ErrorCode m_code;
    std::string m_message;
    std::source_location m_where;
    std::int64_t m_native_code = 0;
};

}  // namespace atlas

template <> struct std::formatter<atlas::Error> : std::formatter<std::string> {
    auto format(const atlas::Error& error, std::format_context& ctx) const {
        return std::formatter<std::string>::format(error.to_string(), ctx);
    }
};
