// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/virtual_path.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace atlas::assets {
namespace {

[[nodiscard]] bool is_separator(char character) noexcept {
    return character == '/';
}

}  // namespace

Result<VirtualPath> VirtualPath::parse(std::string_view text) {
    if (text.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "an asset path cannot be empty"));
    }
    if (text.size() > kMaxLength) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("asset path is {} characters, longer than the limit of {}",
                              text.size(), kMaxLength)));
    }

    // A null byte would truncate the path when it reached a filesystem call, so the part
    // after it would be invisible to every check above that call.
    if (text.contains('\0')) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "an asset path cannot contain a null byte"));
    }

    // Backslashes are rejected rather than translated. On one platform a backslash is a
    // separator and on another it is an ordinary character, so translating would make the
    // same path mean different things, and hashing it would give different identifiers.
    if (text.contains('\\')) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("asset path '{}' contains a backslash; use forward slashes", text)));
    }

    if (is_separator(text.front())) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("asset path '{}' is absolute; paths are relative to a mounted root",
                              text)));
    }

    // A Windows drive letter is absolute in a way a leading-slash test does not catch.
    if (text.size() >= 2 && text[1] == ':') {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("asset path '{}' names a drive; paths are relative to a mounted root",
                              text)));
    }

    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('/', start);
        const std::string_view segment = text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);

        if (segment == "..") {
            // Rejected, not resolved. Resolving correctly is possible; resolving subtly
            // wrongly is a directory escape, and no legitimate asset path needs it.
            return std::unexpected(
                Error(ErrorCode::PermissionDenied,
                      std::format("asset path '{}' contains '..', which could leave the mounted "
                                  "root. Asset paths may not traverse upwards.",
                                  text)));
        }

        // Empty segments come from repeated or trailing separators; "." means here. Both are
        // simply dropped, which is what normalising means.
        if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    if (segments.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("asset path '{}' normalises to nothing", text)));
    }

    std::string normalised;
    normalised.reserve(text.size());
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            normalised.push_back('/');
        }
        normalised.append(segments[i]);
    }

    return VirtualPath{std::move(normalised)};
}

std::string_view VirtualPath::filename() const {
    const std::size_t slash = m_text.find_last_of('/');
    return slash == std::string::npos ? std::string_view{m_text}
                                      : std::string_view{m_text}.substr(slash + 1);
}

std::string_view VirtualPath::parent() const {
    const std::size_t slash = m_text.find_last_of('/');
    return slash == std::string::npos ? std::string_view{}
                                      : std::string_view{m_text}.substr(0, slash);
}

std::string_view VirtualPath::extension() const {
    const std::string_view name = filename();
    const std::size_t dot = name.find_last_of('.');

    // A leading dot is a hidden file, not an extension: ".gitignore" has no extension.
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return name.substr(dot + 1);
}

bool VirtualPath::has_extension(std::string_view candidate) const {
    const std::string_view own = extension();
    if (own.size() != candidate.size()) {
        return false;
    }
    return std::ranges::equal(own, candidate, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

}  // namespace atlas::assets
