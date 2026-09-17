// SPDX-License-Identifier: GPL-3.0-or-later
// Cutting committed text into events, tested without a window system.
//
// This is where the behaviour that can be wrong lives. The translation that calls it is one
// line; what matters is that a cut never lands inside a character and that concatenating the
// pieces reproduces the input, because every consumer appends them in order.
#include "text_split.hpp"
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using atlas::platform::Event;
using atlas::platform::TextInput;
using atlas::platform::detail::append_text_input;
using atlas::platform::detail::is_utf8_continuation;
using atlas::platform::detail::utf8_prefix_length;

namespace {

/// The text of every TextInput in `events`, joined. What a consumer that appends would see.
[[nodiscard]] std::string joined(const std::vector<Event>& events) {
    std::string all;
    for (const auto& event : events) {
        const auto* text = std::get_if<TextInput>(&event);
        REQUIRE(text != nullptr);
        all += text->text();
    }
    return all;
}

/// No event may end part-way through a character, which is the whole point of the split.
void check_every_piece_ends_on_a_boundary(const std::vector<Event>& events) {
    for (const auto& event : events) {
        const auto* text = std::get_if<TextInput>(&event);
        REQUIRE(text != nullptr);
        REQUIRE(text->length > 0);
        // A piece that ended mid-character would leave the next piece starting on a
        // continuation byte, which is the observable symptom.
        CHECK_FALSE(is_utf8_continuation(text->text().front()));
    }
}

}  // namespace

TEST_CASE("empty text produces no event", "[platform][text]") {
    std::vector<Event> events;
    append_text_input(events, "");
    CHECK(events.empty());
}

TEST_CASE("short text is one event", "[platform][text]") {
    std::vector<Event> events;
    append_text_input(events, "hello");

    REQUIRE(events.size() == 1);
    const auto* text = std::get_if<TextInput>(&events.front());
    REQUIRE(text != nullptr);
    CHECK(text->text() == "hello");
    CHECK(text->length == 5);
    // NUL-terminated, so the C interface the overlay uses is safe.
    CHECK(std::string_view{text->c_str()} == "hello");
}

TEST_CASE("a multi-byte sequence survives intact", "[platform][text]") {
    // Latin with a diacritic, a currency sign, and three Japanese characters: two, three and
    // three bytes each, so a naive byte cut would corrupt all of them.
    const std::string_view input = "héllo €100 日本語";
    std::vector<Event> events;
    append_text_input(events, input);

    REQUIRE(events.size() == 1);
    CHECK(joined(events) == input);
}

TEST_CASE("text of exactly the capacity is one event", "[platform][text]") {
    const std::string input(TextInput::kCapacity, 'a');
    std::vector<Event> events;
    append_text_input(events, input);

    REQUIRE(events.size() == 1);
    CHECK(joined(events) == input);
}

TEST_CASE("one byte past the capacity is two events", "[platform][text]") {
    const std::string input(TextInput::kCapacity + 1, 'a');
    std::vector<Event> events;
    append_text_input(events, input);

    REQUIRE(events.size() == 2);
    CHECK(joined(events) == input);
}

TEST_CASE("a long commit splits on character boundaries and concatenates back",
          "[platform][text]") {
    // Thirty three-byte characters is ninety bytes, so this must split. Sixty-three is not a
    // multiple of three, so the cut falls inside a character unless the split backs off: this
    // is the case that fails if the boundary logic is wrong.
    std::string input;
    for (int i = 0; i < 30; ++i) {
        input += "日";
    }
    REQUIRE(input.size() == 90);

    std::vector<Event> events;
    append_text_input(events, input);

    REQUIRE(events.size() == 2);
    check_every_piece_ends_on_a_boundary(events);
    CHECK(joined(events) == input);

    // Sixty-three bytes holds twenty-one whole characters; the twenty-second would end at
    // sixty-six. So the first piece is sixty-three bytes exactly and nothing is lost.
    const auto* first = std::get_if<TextInput>(&events.front());
    REQUIRE(first != nullptr);
    CHECK(first->length == 63);
}

TEST_CASE("a cut that lands inside a character backs off to the previous boundary",
          "[platform][text]") {
    // Sixty-two single-byte characters then one three-byte character: the limit falls on the
    // second byte of the last character, so the prefix must stop at sixty-two.
    std::string input(62, 'a');
    input += "語";
    REQUIRE(input.size() == 65);

    std::vector<Event> events;
    append_text_input(events, input);

    REQUIRE(events.size() == 2);
    const auto* first = std::get_if<TextInput>(&events.front());
    REQUIRE(first != nullptr);
    CHECK(first->length == 62);
    CHECK(joined(events) == input);
    check_every_piece_ends_on_a_boundary(events);
}

TEST_CASE("the prefix helper answers the boundary cases directly", "[platform][text]") {
    CHECK(utf8_prefix_length("", 63) == 0);
    CHECK(utf8_prefix_length("abc", 63) == 3);
    CHECK(utf8_prefix_length("abc", 3) == 3);
    CHECK(utf8_prefix_length("abcd", 3) == 3);
    // Two three-byte characters: a limit of four must back off to three, not cut at four.
    CHECK(utf8_prefix_length("日本", 4) == 3);
    CHECK(utf8_prefix_length("日本", 6) == 6);
}

TEST_CASE("continuation bytes are recognised", "[platform][text]") {
    const std::string_view japanese = "日";
    REQUIRE(japanese.size() == 3);
    CHECK_FALSE(is_utf8_continuation(japanese[0]));
    CHECK(is_utf8_continuation(japanese[1]));
    CHECK(is_utf8_continuation(japanese[2]));
    CHECK_FALSE(is_utf8_continuation('a'));
}
