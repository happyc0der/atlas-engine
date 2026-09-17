// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/virtual_path.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using atlas::ErrorCode;
using atlas::assets::VirtualPath;

TEST_CASE("a simple path parses unchanged", "[assets][path]") {
    const auto path = VirtualPath::parse("textures/grass.png");
    REQUIRE(path.has_value());
    CHECK(path->text() == "textures/grass.png");
    CHECK(path->filename() == "grass.png");
    CHECK(path->parent() == "textures");
    CHECK(path->extension() == "png");
}

TEST_CASE("repeated and trailing separators are collapsed", "[assets][path]") {
    // Two paths that mean the same thing must normalise to the same text, or they would hash
    // to different identifiers and the same file would be loaded twice.
    for (const auto* const text :
         {"textures//grass.png", "textures/grass.png/", "./textures/grass.png",
          "textures/./grass.png", "textures///grass.png//"}) {
        INFO("input " << text);
        const auto path = VirtualPath::parse(text);
        REQUIRE(path.has_value());
        CHECK(path->text() == "textures/grass.png");
    }
}

TEST_CASE("a path at the root has no parent", "[assets][path]") {
    const auto path = VirtualPath::parse("config.json");
    REQUIRE(path.has_value());
    CHECK(path->filename() == "config.json");
    CHECK(path->parent().empty());
}

TEST_CASE("upward traversal is refused", "[assets][path]") {
    // The whole reason this type exists. Resolving `..` correctly is possible; resolving it
    // subtly wrongly lets an asset pack read anything on the machine, so it is refused
    // outright and no legitimate asset path needs it.
    for (const auto* const text :
         {"../secret", "textures/../../secret", "a/b/../../../etc/passwd", "..", "textures/.."}) {
        INFO("input " << text);
        const auto path = VirtualPath::parse(text);
        REQUIRE_FALSE(path.has_value());
        CHECK(path.error().code() == ErrorCode::PermissionDenied);
    }
}

TEST_CASE("absolute paths are refused", "[assets][path]") {
    for (const auto* const text : {"/etc/passwd", "/", "//server/share"}) {
        INFO("input " << text);
        const auto path = VirtualPath::parse(text);
        REQUIRE_FALSE(path.has_value());
        CHECK(path.error().code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("a drive letter is refused", "[assets][path]") {
    // Absolute in a way that testing for a leading slash does not catch.
    const auto path = VirtualPath::parse("C:/Windows/System32");
    REQUIRE_FALSE(path.has_value());
    CHECK(path.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("backslashes are refused rather than translated", "[assets][path]") {
    // Translating them would make the same text mean different things on different
    // platforms, and therefore hash to different asset identifiers.
    const auto path = VirtualPath::parse("textures\\grass.png");
    REQUIRE_FALSE(path.has_value());
    CHECK(path.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a null byte is refused", "[assets][path]") {
    // It would truncate the path at the filesystem call, hiding everything after it from
    // every check above that call.
    const std::string text = std::string("textures/grass.png") + '\0' + "ignored";
    const auto path = VirtualPath::parse(std::string_view{text.data(), text.size()});
    REQUIRE_FALSE(path.has_value());
    CHECK(path.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("an empty or vacuous path is refused", "[assets][path]") {
    CHECK_FALSE(VirtualPath::parse("").has_value());
    CHECK_FALSE(VirtualPath::parse("/").has_value());
    CHECK_FALSE(VirtualPath::parse(".").has_value());
    CHECK_FALSE(VirtualPath::parse("./").has_value());
    CHECK_FALSE(VirtualPath::parse("///").has_value());
}

TEST_CASE("an absurdly long path is refused", "[assets][path]") {
    // It would reach a filesystem call. A path this long is either a mistake or an attack,
    // and neither deserves an allocation.
    const std::string long_path(VirtualPath::kMaxLength + 1, 'a');
    const auto path = VirtualPath::parse(long_path);
    REQUIRE_FALSE(path.has_value());
    CHECK(path.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("extensions are reported as written", "[assets][path]") {
    CHECK(VirtualPath::parse("a/b.PNG")->extension() == "PNG");
    CHECK(VirtualPath::parse("a/b.tar.gz")->extension() == "gz");
    CHECK(VirtualPath::parse("a/b")->extension().empty());

    // A leading dot is a hidden file, not an extension.
    CHECK(VirtualPath::parse("a/.gitignore")->extension().empty());
}

TEST_CASE("extension comparison ignores case", "[assets][path]") {
    // Which file extension a contributor's tools produce is not a decision anyone made, so
    // choosing an importer must not depend on it.
    const auto path = VirtualPath::parse("textures/Grass.PNG");
    REQUIRE(path.has_value());
    CHECK(path->has_extension("png"));
    CHECK(path->has_extension("PNG"));
    CHECK_FALSE(path->has_extension("jpg"));
    CHECK_FALSE(path->has_extension("pn"));
}

TEST_CASE("paths compare and order by their normalised text", "[assets][path]") {
    const auto first = VirtualPath::parse("a/b.png");
    const auto same = VirtualPath::parse("a//b.png");
    const auto other = VirtualPath::parse("a/c.png");

    REQUIRE(first.has_value());
    REQUIRE(same.has_value());
    REQUIRE(other.has_value());

    CHECK(*first == *same);
    CHECK_FALSE(*first == *other);
    CHECK(*first < *other);
}

TEST_CASE("a path exposes its normalised text", "[assets][path]") {
    const auto path = VirtualPath::parse("shaders/sprite.vert");
    REQUIRE(path.has_value());
    const std::string_view view = path->text();
    CHECK(view == "shaders/sprite.vert");
}
