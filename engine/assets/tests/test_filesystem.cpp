// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <system_error>

using atlas::ErrorCode;
using atlas::assets::FileSystem;
using atlas::assets::VirtualPath;

namespace {

/// A temporary directory tree that removes itself.
class TempTree {
  public:
    TempTree() {
        std::error_code error;
        m_root = std::filesystem::temp_directory_path(error) /
                 std::format("atlas-assets-{}", reinterpret_cast<std::uintptr_t>(this));
        std::filesystem::create_directories(m_root, error);
    }

    ~TempTree() {
        std::error_code error;
        std::filesystem::remove_all(m_root, error);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    /// Create a file with contents, making parent directories as needed.
    void write(std::string_view relative, std::string_view contents) const {
        const auto path = m_root / relative;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream stream(path, std::ios::binary);
        stream << contents;
    }

    [[nodiscard]] std::filesystem::path sub(std::string_view name) const {
        const auto path = m_root / name;
        std::error_code error;
        std::filesystem::create_directories(path, error);
        return path;
    }

  private:
    std::filesystem::path m_root;
};

[[nodiscard]] VirtualPath path_of(std::string_view text) {
    auto parsed = VirtualPath::parse(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] std::string text_of(std::span<const std::byte> bytes) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes back to text.
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

TEST_CASE("nothing resolves before anything is mounted", "[assets][filesystem]") {
    const FileSystem filesystem;
    const auto resolved = filesystem.resolve(path_of("a.txt"));

    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().message().find("nothing is mounted") != std::string::npos);
}

TEST_CASE("a mounted file resolves and reads", "[assets][filesystem]") {
    const TempTree tree;
    tree.write("textures/grass.txt", "green");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());

    const auto path = path_of("textures/grass.txt");
    CHECK(filesystem.exists(path));

    const auto contents = filesystem.read(path);
    REQUIRE(contents.has_value());
    CHECK(text_of(*contents) == "green");
}

TEST_CASE("mounting something that is not a directory is refused", "[assets][filesystem]") {
    const TempTree tree;
    tree.write("a-file.txt", "x");

    FileSystem filesystem;
    const auto status = filesystem.mount("bad", tree.root() / "a-file.txt");

    // A configuration mistake worth reporting immediately rather than at the first missing
    // asset, when the cause is much further away.
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("mounting a directory that does not exist is refused", "[assets][filesystem]") {
    FileSystem filesystem;
    const auto status = filesystem.mount("missing", "/definitely/not/here/at/all");
    REQUIRE_FALSE(status.has_value());
}

TEST_CASE("two mounts cannot share a name", "[assets][filesystem]") {
    const TempTree tree;
    FileSystem filesystem;

    REQUIRE(filesystem.mount("base", tree.sub("one")).has_value());
    const auto second = filesystem.mount("base", tree.sub("two"));

    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code() == ErrorCode::AlreadyExists);
}

TEST_CASE("a higher-priority mount shadows a lower one", "[assets][filesystem]") {
    // How a modification directory overrides base content without either knowing about the
    // other.
    const TempTree tree;
    const auto base = tree.sub("base");
    const auto mods = tree.sub("mods");

    {
        std::ofstream{base / "config.txt"} << "from base";
        std::ofstream{mods / "config.txt"} << "from mods";
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", base, 0).has_value());
    REQUIRE(filesystem.mount("mods", mods, 100).has_value());

    const auto contents = filesystem.read(path_of("config.txt"));
    REQUIRE(contents.has_value());
    CHECK(text_of(*contents) == "from mods");
}

TEST_CASE("a lower-priority mount still supplies what the higher one lacks",
          "[assets][filesystem]") {
    const TempTree tree;
    const auto base = tree.sub("base");
    const auto mods = tree.sub("mods");

    {
        std::ofstream{base / "only-in-base.txt"} << "base";
        std::ofstream{mods / "only-in-mods.txt"} << "mods";
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", base, 0).has_value());
    REQUIRE(filesystem.mount("mods", mods, 100).has_value());

    CHECK(filesystem.exists(path_of("only-in-base.txt")));
    CHECK(filesystem.exists(path_of("only-in-mods.txt")));
}

TEST_CASE("unmounting removes a root", "[assets][filesystem]") {
    const TempTree tree;
    tree.write("a.txt", "x");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());
    REQUIRE(filesystem.exists(path_of("a.txt")));

    CHECK(filesystem.unmount("base"));
    CHECK_FALSE(filesystem.exists(path_of("a.txt")));
    CHECK_FALSE(filesystem.unmount("base"));
}

TEST_CASE("a missing file reports which roots were searched", "[assets][filesystem]") {
    const TempTree tree;
    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.sub("base")).has_value());
    REQUIRE(filesystem.mount("mods", tree.sub("mods")).has_value());

    const auto resolved = filesystem.resolve(path_of("nowhere.txt"));
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code() == ErrorCode::AssetNotFound);

    // Naming the roots turns "not found" into something a reader can act on.
    CHECK(resolved.error().message().find("base") != std::string::npos);
    CHECK(resolved.error().message().find("mods") != std::string::npos);
}

TEST_CASE("a symbolic link out of the mounted root is refused", "[assets][filesystem]") {
    // The check that matters is where a path ends up, not that it was built from the root.
    // A link is the standard way to make those two differ.
    const TempTree tree;
    const auto outside = tree.sub("outside");
    const auto inside = tree.sub("inside");

    {
        std::ofstream{outside / "secret.txt"} << "not yours";
    }

    std::error_code error;
    std::filesystem::create_symlink(outside / "secret.txt", inside / "link.txt", error);
    if (error) {
        SKIP("this filesystem does not support symbolic links");
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("inside", inside).has_value());

    const auto resolved = filesystem.resolve(path_of("link.txt"));
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code() == ErrorCode::AssetNotFound);
}

TEST_CASE("a link that stays inside the root is fine", "[assets][filesystem]") {
    // The rule is about leaving the root, not about links.
    const TempTree tree;
    const auto root = tree.sub("root");

    {
        std::ofstream{root / "real.txt"} << "fine";
    }

    std::error_code error;
    std::filesystem::create_symlink(root / "real.txt", root / "alias.txt", error);
    if (error) {
        SKIP("this filesystem does not support symbolic links");
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("root", root).has_value());

    const auto contents = filesystem.read(path_of("alias.txt"));
    REQUIRE(contents.has_value());
    CHECK(text_of(*contents) == "fine");
}

TEST_CASE("a sibling directory with a matching prefix is not inside the root",
          "[assets][filesystem]") {
    // '/data/assets-private' begins with '/data/assets' as text but is not inside it. A
    // string-prefix containment test would let it through.
    const TempTree tree;
    const auto assets = tree.sub("assets");
    (void)tree.sub("assets-private");

    {
        std::ofstream{tree.root() / "assets-private" / "secret.txt"} << "no";
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("assets", assets).has_value());
    CHECK_FALSE(filesystem.exists(path_of("secret.txt")));
}

TEST_CASE("listing is sorted and free of duplicates", "[assets][filesystem]") {
    // A filesystem's own order is arbitrary and may differ between runs, and the same path
    // can exist in two mounts. Anything hashing or serialising a listing would inherit both
    // problems. See docs/DETERMINISM.md.
    const TempTree tree;
    const auto base = tree.sub("base");
    const auto mods = tree.sub("mods");

    for (const auto* name : {"zebra.txt", "apple.txt", "mango.txt"}) {
        std::ofstream{base / name} << "x";
    }
    {
        std::ofstream{mods / "apple.txt"} << "shadowed";
    }

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", base, 0).has_value());
    REQUIRE(filesystem.mount("mods", mods, 100).has_value());

    const auto listing = filesystem.list();
    REQUIRE(listing.size() == 3);
    CHECK(listing[0].text() == "apple.txt");
    CHECK(listing[1].text() == "mango.txt");
    CHECK(listing[2].text() == "zebra.txt");
}

TEST_CASE("listing can be restricted to a prefix", "[assets][filesystem]") {
    const TempTree tree;
    tree.write("textures/a.png", "x");
    tree.write("textures/b.png", "x");
    tree.write("shaders/c.vert", "x");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());

    const auto listing = filesystem.list("textures");
    REQUIRE(listing.size() == 2);
    CHECK(listing[0].text() == "textures/a.png");
    CHECK(listing[1].text() == "textures/b.png");
}

TEST_CASE("a modification time is reported and moves when a file changes", "[assets][filesystem]") {
    const TempTree tree;
    tree.write("a.txt", "one");

    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());

    const auto path = path_of("a.txt");
    const auto first = filesystem.modified_at(path);
    REQUIRE(first.has_value());

    // Set it explicitly rather than sleeping: the value is what hot reload compares, and a
    // test that waits for a clock is slow and occasionally wrong.
    std::error_code error;
    std::filesystem::last_write_time(tree.root() / "a.txt", *first + std::chrono::seconds{10},
                                     error);
    REQUIRE_FALSE(error);

    const auto second = filesystem.modified_at(path);
    REQUIRE(second.has_value());
    // Extra parentheses on purpose: without them Catch2 decomposes the comparison and tries
    // to print a file time, whose representation here is a 128-bit integer with no stream
    // operator.
    CHECK((*second > *first));
}

TEST_CASE("the modification time of a missing file is an error", "[assets][filesystem]") {
    const TempTree tree;
    FileSystem filesystem;
    REQUIRE(filesystem.mount("base", tree.root()).has_value());

    CHECK_FALSE(filesystem.modified_at(path_of("nowhere.txt")).has_value());
}
