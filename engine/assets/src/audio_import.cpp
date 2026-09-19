// SPDX-License-Identifier: GPL-3.0-or-later
//
// Choosing an audio decoder by what a file contains.
//
// Its own translation unit rather than a few lines in `importer.cpp`, because that file exists
// to compile the image decoder's single-header implementation exactly once, and everything
// sharing a unit with it shares its analysis. One unrelated function in there was enough to
// make the static analyser walk further into the third-party code and report a leak inside it.
//
// The separation is also the right shape for what comes next: the day a second format is
// supported, the choosing goes here and the decoding goes beside `wav.cpp`.

#include "animation_clip.hpp"
#include "string_table.hpp"
#include "wav.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace atlas::assets {

Result<ImportedAudio> import_audio(std::span<const std::byte> bytes, std::string_view debug_name) {
    // By the leading bytes, never by the extension. An extension is a claim made by whoever
    // named the file; this function's input is untrusted, and the magic bytes are the only
    // part of that claim the file has to honour to be decodable at all.
    if (detail::looks_like_wav(bytes)) {
        return detail::import_wav(bytes, debug_name);
    }

    // Named, not guessed. "Unrecognised format" sends whoever reads it looking at their
    // pipeline; the four bytes that were actually there usually tell them what happened
    // instead, most often that the file is an error page or an Ogg.
    std::string leading;
    for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 4); ++i) {
        const auto value = static_cast<unsigned char>(bytes[i]);
        leading += (value >= 0x20 && value < 0x7F) ? static_cast<char>(value) : '?';
    }
    return std::unexpected(
        Error(ErrorCode::AssetImportFailed,
              std::format("'{}' is not audio this engine decodes; it begins with '{}'. Only "
                          "RIFF/WAVE is supported.",
                          debug_name, leading)));
}

Result<ImportedAnimationClip> import_animation_clip(std::span<const std::byte> bytes,
                                                    std::string_view debug_name) {
    // One format, so there is nothing to choose between — but the document still names itself,
    // and the parser checks that marker before anything else. A file's extension is a claim
    // made by whoever named it; the marker is a claim the file has to honour to be read at all.
    return detail::parse_animation_clip(bytes, debug_name);
}

Result<ImportedStringTable> import_string_table(std::span<const std::byte> bytes,
                                                std::string_view debug_name) {
    // As above: one format, nothing to choose between, and the marker checked before anything
    // else rather than the extension trusted.
    return detail::parse_string_table(bytes, debug_name);
}

}  // namespace atlas::assets
