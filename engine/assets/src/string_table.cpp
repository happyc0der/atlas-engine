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

/// Finds a key listed twice, which the parsed document can no longer show.
///
/// **This exists because checking after the parse does not work, and the first version of this
/// file tried to.** `nlohmann`'s object types collapse a repeated key while parsing — one entry
/// survives and the other is gone — so by the time there is a document to inspect, the evidence
/// has been discarded. A test asserting the refusal is what found that; the check had been
/// written, looked right, and could never fire.
///
/// So the detection happens during the parse instead, through the SAX interface, which reports
/// every key the text actually contained. It is a separate pass over the same bytes: a table is
/// kilobytes, and keeping it separate leaves the real parser reading a document rather than
/// also policing one.
///
/// The rule is deliberately stronger than "no duplicate string key": **no object in the
/// document may list a key twice.** That is simpler to state, simpler to implement, and there
/// is no place in this format where a repeat means anything.
// The whole class is shaped by nlohmann's SAX interface rather than by this project's
// conventions: the member type names and the method names are what the library looks for, and
// the handlers cannot be static because it calls them on an instance. Renaming any of them
// stops the parse from compiling, so the checks are turned off here and nowhere else.
// NOLINTBEGIN(readability-identifier-naming,readability-convert-member-functions-to-static)
class DuplicateKeyFinder {
  public:
    using number_integer_t = Json::number_integer_t;
    using number_unsigned_t = Json::number_unsigned_t;
    using number_float_t = Json::number_float_t;
    using string_t = Json::string_t;
    using binary_t = Json::binary_t;

    [[nodiscard]] bool found() const noexcept { return !m_duplicate.empty(); }

    [[nodiscard]] const std::string& duplicate() const noexcept { return m_duplicate; }

    bool key(string_t& value) {
        if (!m_scopes.empty() && !m_scopes.back().insert(value).second) {
            m_duplicate = value;
            return false;  // Stop the parse: the document is already refused.
        }
        return true;
    }

    bool start_object(std::size_t /*elements*/) {
        m_scopes.emplace_back();
        return true;
    }

    bool end_object() {
        if (!m_scopes.empty()) {
            m_scopes.pop_back();
        }
        return true;
    }

    // Everything else is a value this pass does not care about. They exist because the SAX
    // interface requires them, and each simply says "carry on".
    bool null() { return true; }

    bool boolean(bool /*value*/) { return true; }

    bool number_integer(number_integer_t /*value*/) { return true; }

    bool number_unsigned(number_unsigned_t /*value*/) { return true; }

    bool number_float(number_float_t /*value*/, const string_t& /*raw*/) { return true; }

    bool string(string_t& /*value*/) { return true; }

    bool binary(binary_t& /*value*/) { return true; }

    bool start_array(std::size_t /*elements*/) { return true; }

    bool end_array() { return true; }

    /// Malformed JSON is not this pass's business: the real parser reports it, with its own
    /// message. Returning false here simply stops.
    bool parse_error(std::size_t /*position*/, const std::string& /*last_token*/,
                     const Json::exception& /*ex*/) {
        return false;
    }

  private:
    std::vector<std::unordered_set<std::string>> m_scopes;
    std::string m_duplicate;
};

// NOLINTEND(readability-identifier-naming,readability-convert-member-functions-to-static)

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

    // Before the document is built, because building it is what destroys the evidence. See
    // DuplicateKeyFinder.
    DuplicateKeyFinder finder;
    Json::sax_parse(text, &finder, nlohmann::detail::input_format_t::json, false, true);
    if (finder.found()) {
        return refuse(std::format("lists the key '{}' more than once", finder.duplicate()));
    }

    // Non-throwing, because ADR-0005 forbids exceptions crossing a module boundary. Comments
    // are permitted, matching the scene and clip readers: a table is a file somebody edits by
    // hand, and a note beside an entry is exactly what they would write.
    const Json root = Json::parse(text, nullptr, false, true);
    if (root.is_discarded()) {
        return refuse("is not valid JSON");
    }
    if (!root.is_object()) {
        return refuse("is not a JSON object");
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
