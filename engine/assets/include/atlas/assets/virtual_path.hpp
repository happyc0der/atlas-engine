// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Paths as the engine sees them.
///
/// Nothing above this layer names a directory on a real filesystem. An asset lives at a
/// virtual path such as `textures/grass.png`, and where that actually is depends on which
/// roots are mounted. That indirection is what lets a modification directory shadow the base
/// content, and what keeps the same content working from a directory during development and
/// from an archive later.
///
/// The normalisation rules are also a security boundary. Asset packs, modifications and save
/// files are untrusted input, and a path is the classic way to escape the directory it is
/// supposed to stay inside. See docs/PROJECT_CHARTER.md and the security section of the
/// specification.

#include <atlas/core/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace atlas::assets {

/// A normalised, engine-relative path.
///
/// Only constructible through `parse`, which enforces the rules, so a VirtualPath in hand is
/// already known to be safe to resolve against a mounted root.
class VirtualPath {
  public:
    /// Longest path Atlas will accept.
    ///
    /// Bounded because the value reaches a filesystem call and a wildly long path is either
    /// a mistake or an attack, neither of which deserves an allocation.
    static constexpr std::size_t kMaxLength = 1024;

    /// Normalise and validate.
    ///
    /// Accepts forward slashes, collapses repeated separators, and removes `.` segments.
    /// Rejects anything that could leave the mounted root: an absolute path, a `..` segment,
    /// a backslash, a drive letter, a null byte, or an empty result.
    ///
    /// `..` is rejected rather than resolved. Resolving it correctly is possible; getting
    /// it subtly wrong is a directory escape, and no legitimate asset path needs it.
    [[nodiscard]] static Result<VirtualPath> parse(std::string_view text);

    /// The normalised text, with forward slashes and no trailing separator.
    ///
    /// No implicit conversion to a string view: `text()` already converts to one, and an
    /// extra operator only adds a way for a path to become a string somewhere nobody meant
    /// it to.
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }

    /// Everything after the last dot of the final segment, without the dot, exactly as
    /// written. Empty when there is no dot, or when the only dot starts the name: a file
    /// called `.gitignore` has no extension, it has a name beginning with a dot.
    ///
    /// Not lowercased, because a view cannot be. Use `has_extension` to compare, which is
    /// what a caller choosing an importer actually wants.
    [[nodiscard]] std::string_view extension() const;

    /// Whether the extension matches, ignoring case.
    ///
    /// Case-insensitive because a file named `.PNG` is the same kind of file as one named
    /// `.png`, and which one a contributor produces depends on their tools rather than on
    /// any decision.
    [[nodiscard]] bool has_extension(std::string_view candidate) const;

    /// The final segment.
    [[nodiscard]] std::string_view filename() const;

    /// Everything before the final segment, or empty at the root.
    [[nodiscard]] std::string_view parent() const;

    [[nodiscard]] friend bool operator==(const VirtualPath& a, const VirtualPath& b) noexcept {
        return a.m_text == b.m_text;
    }

    [[nodiscard]] friend auto operator<=>(const VirtualPath& a, const VirtualPath& b) noexcept {
        return a.m_text <=> b.m_text;
    }

  private:
    explicit VirtualPath(std::string text) noexcept : m_text(std::move(text)) {}

    std::string m_text;
};

}  // namespace atlas::assets
