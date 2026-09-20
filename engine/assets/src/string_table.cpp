// SPDX-License-Identifier: GPL-3.0-or-later
//
// The string table reader.
//
// Its own translation unit, away from `importer.cpp`, which compiles the image decoder's
// single-header implementation: in M12 one unrelated function in there was enough to make the
// static analyser walk further into that third-party code and report a leak inside it.
//
// This is the JSON reader's third call site and its second inside this module. It changes no
// boundary — the library stays private to `scene` and `assets`, and `atlas::text` never links
// it, receiving an `ImportedStringTable` of plain strings instead.

#include "string_table.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atlas::assets::detail {
namespace {

/// Ordered, as the scene and clip readers use it: a table is written and reviewed by people,
/// and key order is part of a file being canonical.
using Json = nlohmann::ordered_json;

constexpr std::string_view kFormatMarker = "atlas-strings";
constexpr std::uint32_t kStringTableVersion = 1;
constexpr std::uint32_t kMinStringTableVersion = 1;

/// Reports a key listed twice, which the parsed document can no longer show.
///
/// **Checking after the parse does not work, and the first version of this file tried to.**
/// `nlohmann`'s object types collapse a repeated key while parsing — one entry survives and the
/// other is gone — so by the time there is a document to inspect, the evidence has been
/// discarded. A test asserting the refusal is what found that; the check had been written,
/// looked right, and could never fire.
///
/// So detection happens *during* the parse, through the callback `parse` accepts, which is
/// handed every key the text actually contained.
///
/// **Deliberately the callback and not the SAX interface**, which was the second thing tried.
/// `sax_parse` goes through the entry point that dispatches on `input_format_t`, and that
/// instantiates `binary_reader` — nlohmann's CBOR, MessagePack and BSON readers — which MSVC
/// reports unreachable code inside. `/external:W0` does not cover a template instantiated from
/// this translation unit, so the warning lands here and `/WX` makes it an error. Windows Debug
/// found that and no other lane did. The callback touches none of it, and it reads better: one
/// pass rather than two.
///
/// The rule is deliberately stronger than "no duplicate string key": **no object in the
/// document may list a key twice.** That is simpler to state, simpler to implement, and there
/// is no place in this format where a repeat means anything.
class DuplicateKeyFinder {
  public:
    [[nodiscard]] bool found() const noexcept { return !m_duplicate.empty(); }

    [[nodiscard]] const std::string& duplicate() const noexcept { return m_duplicate; }

    /// Every element the parser reads, in document order.
    ///
    /// Returns whether to keep it, which is always true: this observes and never edits. A
    /// duplicate is recorded rather than thrown, and the caller refuses the document after the
    /// parse returns.
    bool operator()(int /*depth*/, Json::parse_event_t event, Json& parsed) {
        switch (event) {
        case Json::parse_event_t::object_start: m_scopes.emplace_back(); break;
        case Json::parse_event_t::object_end:
            if (!m_scopes.empty()) {
                m_scopes.pop_back();
            }
            break;
        case Json::parse_event_t::key: {
            // `parsed` is the key itself at this event, and it is a JSON string.
            auto key = parsed.get<std::string>();
            if (!m_scopes.empty() && !m_scopes.back().insert(key).second && m_duplicate.empty()) {
                // The first one found is the one reported. Carrying on costs nothing and
                // avoids a second exit path through a parser that is about to finish anyway.
                m_duplicate = std::move(key);
            }
            break;
        }
        case Json::parse_event_t::array_start:
        case Json::parse_event_t::array_end:
        case Json::parse_event_t::value: break;
        }
        return true;
    }

  private:
    std::vector<std::unordered_set<std::string>> m_scopes;
    std::string m_duplicate;
};

}  // namespace

Result<ImportedStringTable> parse_string_table(std::span<const std::byte> bytes,
                                               std::string_view debug_name) {
    const auto refuse = [&debug_name](std::string message) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("'{}': {}", debug_name, message)));
    };

    const ImportLimits& limits = import_limits();

    // Before the parse, not after it. This is the whole of what a document parser can do about
    // an enormous input: once nlohmann has been handed the text it has already allocated.
    if (bytes.size() > limits.max_string_table_bytes) {
        return refuse(std::format("is {} bytes, past the limit of {}", bytes.size(),
                                  limits.max_string_table_bytes));
    }

    // A view over the same bytes, because the parser takes characters. `as_bytes` is the
    // permitted direction and there is no `as_chars`, so this is the one place the cast is
    // unavoidable; every byte of it has already been bounded above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

    // Non-throwing, because ADR-0005 forbids exceptions crossing a module boundary. Comments
    // are permitted, matching the scene and clip readers: a table is a file somebody edits by
    // hand, and a note beside an entry is exactly what they would write.
    //
    // The callback watches for a key listed twice as the text is read, because the document
    // this returns can no longer show one. See DuplicateKeyFinder.
    DuplicateKeyFinder finder;
    const Json root = Json::parse(text, std::ref(finder), false, true);
    if (root.is_discarded()) {
        return refuse("is not valid JSON");
    }
    if (!root.is_object()) {
        return refuse("is not a JSON object");
    }

    // After the parse rather than before it, because the callback runs during it. Checked here,
    // ahead of the format marker, so that a document which disagrees with itself is refused for
    // that reason rather than for whichever field the disagreement happened to hide.
    if (finder.found()) {
        return refuse(std::format("lists the key '{}' more than once", finder.duplicate()));
    }

    if (!root.contains("format") || !root["format"].is_string() ||
        root["format"].get<std::string>() != kFormatMarker) {
        return refuse(std::format(
            "is not a string table: the format marker is missing or is not '{}'", kFormatMarker));
    }
    if (!root.contains("version") || !root["version"].is_number_unsigned()) {
        return refuse("has no schema version");
    }
    const auto version = root["version"].get<std::uint64_t>();
    if (version > kStringTableVersion) {
        return refuse(
            std::format("is version {}, and this build understands up to {}. Use a newer build.",
                        version, kStringTableVersion));
    }
    if (version < kMinStringTableVersion) {
        return refuse(std::format("is version {}, and this build reads no older than {}", version,
                                  kMinStringTableVersion));
    }

    ImportedStringTable table;

    // Required, and required to be non-empty. A table that does not say what language it is in
    // is a table nobody can file, and defaulting it to English would make the one mistake that
    // matters invisible.
    if (!root.contains("locale") || !root["locale"].is_string()) {
        return refuse("does not say which locale it is for");
    }
    table.locale = root["locale"].get<std::string>();
    if (table.locale.empty()) {
        return refuse("has an empty locale");
    }

    if (!root.contains("strings") || !root["strings"].is_object()) {
        return refuse("has no 'strings' object");
    }
    const Json& strings = root["strings"];

    if (strings.size() > limits.max_strings) {
        return refuse(std::format("holds {} entries, past the limit of {}", strings.size(),
                                  limits.max_strings));
    }
    table.strings.reserve(strings.size());

    for (const auto& [key, value] : strings.items()) {
        if (key.empty()) {
            return refuse("has an entry with an empty key");
        }
        if (key.size() > limits.max_string_key_length) {
            return refuse(std::format("has a key of {} characters, past the limit of {}",
                                      key.size(), limits.max_string_key_length));
        }
        if (!value.is_string()) {
            return refuse(std::format("has a non-string value for key '{}'", key));
        }
        auto text_value = value.get<std::string>();
        if (text_value.size() > limits.max_string_value_length) {
            return refuse(std::format("has a value of {} characters for key '{}', past the limit "
                                      "of {}",
                                      text_value.size(), key, limits.max_string_value_length));
        }

        table.strings.emplace_back(key, std::move(text_value));
    }

    return table;
}

}  // namespace atlas::assets::detail
