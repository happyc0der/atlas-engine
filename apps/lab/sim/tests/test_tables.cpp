// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/generate.hpp>
#include <atlas/lab/tables.hpp>
#include <atlas/simulation/save_stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using atlas::ErrorCode;
using atlas::lab::AdjacencyTable;
using atlas::lab::GridTable;
using atlas::sim::SaveReader;
using atlas::sim::SaveWriter;

namespace {

void expect_refused(const atlas::Status& status, const char* fragment) {
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
    CHECK(status.error().to_string().contains(fragment));
}

}  // namespace

TEST_CASE("adjacency refuses each broken invariant with its own message", "[lab][tables]") {
    // A valid 3-cell path: 0-1-2.
    const std::vector<std::uint32_t> first{0, 1, 3, 4};
    const std::vector<std::uint32_t> neighbour{1, 0, 2, 1};
    REQUIRE(AdjacencyTable::check(first, neighbour).has_value());

    expect_refused(AdjacencyTable::check({}, neighbour), "no row offsets");
    expect_refused(AdjacencyTable::check(std::vector<std::uint32_t>{1, 1, 3, 4}, neighbour),
                   "not zero");
    expect_refused(AdjacencyTable::check(std::vector<std::uint32_t>{0, 3, 1, 4}, neighbour),
                   "decrease");
    expect_refused(AdjacencyTable::check(std::vector<std::uint32_t>{0, 1, 3, 5}, neighbour),
                   "last offset");
    expect_refused(AdjacencyTable::check(first, std::vector<std::uint32_t>{1, 0, 9, 1}),
                   "names cell 9");
}

TEST_CASE("a failed set leaves the adjacency table unchanged", "[lab][tables]") {
    AdjacencyTable table;
    REQUIRE(table.set({0, 1, 2}, {1, 0}).has_value());
    atlas::Hasher before;
    table.hash_into(before);

    REQUIRE_FALSE(table.set({0, 5}, {0}).has_value());
    atlas::Hasher after;
    table.hash_into(after);
    CHECK(before.value() == after.value());
    CHECK(table.row_count() == 2);
}

TEST_CASE("adjacency read from a save is validated before it is trusted", "[lab][tables]") {
    SaveWriter writer;
    writer.write_u64(3);  // first: 3 entries
    writer.write_u32(0);
    writer.write_u32(1);
    writer.write_u32(2);
    writer.write_u64(2);  // neighbour: 2 entries, the second out of range
    writer.write_u32(1);
    writer.write_u32(7);
    AdjacencyTable table;
    SaveReader reader(writer.bytes());
    expect_refused(table.read_from(reader), "names cell 7");
    CHECK(table.row_count() == 0);
}

TEST_CASE("an absurd adjacency count is refused before allocating", "[lab][tables]") {
    SaveWriter writer;
    writer.write_u64(std::uint64_t{1} << 40U);
    AdjacencyTable table;
    SaveReader reader(writer.bytes());
    CHECK_FALSE(table.read_from(reader).has_value());
}

TEST_CASE("the grid row refuses a shape that is not a layout", "[lab][tables]") {
    SaveWriter writer;
    writer.write_u32(10);
    writer.write_u32(8);
    writer.write_u32(4);
    writer.write_u64(1);
    GridTable table;
    SaveReader reader(writer.bytes());
    const auto status = table.read_from(reader);
    REQUIRE_FALSE(status.has_value());
    CHECK(table.width == 0);  // unchanged
}

TEST_CASE("validate_world catches a grid row that disagrees with its tables", "[lab][tables]") {
    auto lab = atlas::lab::generate({.width = 8, .height = 8, .chunk_size = 4, .seed = 1}).value();
    REQUIRE(atlas::lab::validate_world(lab.world, lab.ids).has_value());

    // The grid row claims a different, still valid, shape.
    atlas::lab::grid_table(lab.world, lab.ids).width = 16;
    const auto mismatch = atlas::lab::validate_world(lab.world, lab.ids);
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error().code() == ErrorCode::MalformedData);
    CHECK(mismatch.error().to_string().contains("cells"));
}

TEST_CASE("cells read from a save must respect the owner and colour ranges", "[lab][tables]") {
    SaveWriter writer;
    writer.write_u64(1);
    writer.write_u32(5);   // region
    writer.write_u16(99);  // owner: past kOwnerCount
    writer.write_u8(0);
    atlas::lab::CellTable table;
    SaveReader reader(writer.bytes());
    expect_refused(table.read_from(reader), "owner 99");
}
