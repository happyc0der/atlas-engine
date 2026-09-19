// SPDX-License-Identifier: GPL-3.0-or-later
// Positional substitution, with no catalog, no registry and no file.
//
// The pattern is untrusted input: it came out of a translation file that nobody in this
// repository wrote. So the interesting cases here are all the malformed ones, and the property
// every single one of them asserts is the same — **the call returns, and what it could not
// understand appears in the output unchanged**. There is no malformed input that fails, because
// there is no failure path to take.
#include <atlas/text/substitute.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>

using atlas::text::substitute;

namespace {

/// Spelled out at every call site below so a reader sees the arguments beside the pattern.
std::string sub(std::string_view pattern, std::span<const std::string_view> args = {}) {
    return substitute(pattern, args);
}

}  // namespace

TEST_CASE("a pattern with no placeholders is copied", "[text]") {
    CHECK(sub("").empty());
    CHECK(sub("Pause") == "Pause");

    const std::array<std::string_view, 1> args{"ignored"};
    CHECK(sub("Pause", args) == "Pause");
}

TEST_CASE("placeholders are replaced by position", "[text]") {
    const std::array<std::string_view, 2> args{"3", "7"};

    CHECK(sub("{0} of {1}", args) == "3 of 7");
    CHECK(sub("{0}", args) == "3");
    CHECK(sub("{1}", args) == "7");
    CHECK(sub("[{0}][{1}]", args) == "[3][7]");
}

TEST_CASE("indices may be reordered and repeated", "[text]") {
    const std::array<std::string_view, 2> args{"3", "7"};

    // The whole reason the indices are positional rather than sequential: a translator moves
    // the arguments around the sentence and the call site does not change.
    CHECK(sub("{1} of {0}", args) == "7 of 3");
    CHECK(sub("{0}{0}{0}", args) == "333");
}

TEST_CASE("an empty argument substitutes as nothing", "[text]") {
    const std::array<std::string_view, 1> args{""};
    CHECK(sub("a{0}b", args) == "ab");
}

TEST_CASE("an index with no argument is copied through", "[text]") {
    const std::array<std::string_view, 1> args{"only"};

    // Seeing `{3}` in the interface says which index was wrong, which an empty string would
    // not. This is the same treatment a malformed placeholder gets, because from a translator's
    // side they are the same kind of mistake.
    CHECK(sub("{3}", args) == "{3}");
    CHECK(sub("{0} {1}", args) == "only {1}");
    CHECK(sub("{0}", {}) == "{0}");
}

TEST_CASE("a malformed placeholder is copied through", "[text]") {
    CHECK(sub("{") == "{");
    CHECK(sub("{0") == "{0");
    CHECK(sub("{}") == "{}");
    CHECK(sub("{a}") == "{a}");
    CHECK(sub("{0a}") == "{0a}");
    CHECK(sub("{ 0}") == "{ 0}");
    CHECK(sub("{-1}") == "{-1}");
    CHECK(sub("}") == "}");
    CHECK(sub("}{") == "}{");
    CHECK(sub("{{}}") == "{{}}");
}

TEST_CASE("a nested brace resolves the inner placeholder", "[text]") {
    const std::array<std::string_view, 1> args{"x"};

    // `{` then `{0}` then `}`: the outer brace is not a placeholder because a `{` does not
    // follow it, so it is literal, and the inner one is. Stated as a test because it is the
    // one case where "no escape sequence" has a visible consequence.
    CHECK(sub("{{0}}", args) == "{x}");
}

TEST_CASE("a literal brace pair survives when nothing looks like an index", "[text]") {
    CHECK(sub("{{}}") == "{{}}");
    CHECK(sub("a {b} c") == "a {b} c");
}

TEST_CASE("an index past the digit cap is literal, not wrapped", "[text]") {
    const std::array<std::string_view, 2> args{"first", "second"};

    // **This is the case the digit cap exists for, and it is not the obvious one.** Overflow of
    // an unsigned type is defined to wrap, not undefined, so without the cap a long enough run
    // of digits does not fail — it quietly becomes a *small* index. 2^64 is
    // 18446744073709551616, so the number below is congruent to 1 and would select the second
    // argument: a pattern addressing a slot it does not name.
    //
    // A first attempt at this test used 99999999999999999999, which wraps to something far past
    // the end and is copied through for the ordinary out-of-range reason. It passed with the cap
    // removed, which is how the gap was found.
    CHECK(sub("{18446744073709551617}", args) == "{18446744073709551617}");

    // Ten digits is the first length past the cap.
    CHECK(sub("{9999999999}", args) == "{9999999999}");

    // Nine digits is inside the cap, parses honestly, and is simply out of range.
    CHECK(sub("{999999999}", args) == "{999999999}");
}

TEST_CASE("a leading zero is still an index", "[text]") {
    const std::array<std::string_view, 2> args{"a", "b"};
    CHECK(sub("{00}", args) == "a");
    CHECK(sub("{01}", args) == "b");
}

TEST_CASE("an argument is never rescanned", "[text]") {
    // The difference between a substituter and a macro expander, and the case that matters if a
    // translated string ever reaches an argument slot: an argument containing what looks like a
    // placeholder must appear literally rather than expand against the argument list.
    const std::array<std::string_view, 2> args{"{1}", "second"};
    CHECK(sub("{0}", args) == "{1}");
    CHECK(sub("{0} and {1}", args) == "{1} and second");

    // Including the case where expanding again would not terminate.
    const std::array<std::string_view, 1> loop{"{0}"};
    CHECK(sub("{0}", loop) == "{0}");
}

TEST_CASE("substitution is a single pass, so output length is bounded", "[text]") {
    const std::array<std::string_view, 1> args{"0123456789"};
    const std::string out = sub("{0}{0}{0}", args);
    CHECK(out.size() == 30);
}
