// SPDX-License-Identifier: GPL-3.0-or-later
#include "lab_harness.hpp"
#include <catch2/catch_test_macros.hpp>

using atlas::ErrorCode;
using atlas::lab::decode_set_color_index;
using atlas::lab::encode_set_color_index;
using atlas::lab::kSetColorIndex;
using atlas::lab::testing::kSmall;
using atlas::lab::testing::LabHarness;

TEST_CASE("set_color_index round-trips through five bytes", "[lab][commands]") {
    const auto bytes = encode_set_color_index(0x01'02'03'04U, 5);
    const auto decoded = decode_set_color_index(bytes, 0x7FFF'FFFFU).value();
    CHECK(decoded.cell == 0x01'02'03'04U);
    CHECK(decoded.color == 5);
    CHECK(static_cast<std::uint8_t>(bytes[0]) == 0x04);  // little-endian, by construction
}

TEST_CASE("set_color_index is refused for a wrong length or an out-of-range field",
          "[lab][commands]") {
    const std::array<std::byte, 4> four{};
    CHECK(decode_set_color_index(four, 100).error().code() == ErrorCode::MalformedData);
    CHECK(decode_set_color_index(encode_set_color_index(100, 0), 100).error().code() ==
          ErrorCode::OutOfRange);
    CHECK(decode_set_color_index(encode_set_color_index(0, atlas::lab::kColorCount), 100)
              .error()
              .code() == ErrorCode::OutOfRange);
}

TEST_CASE("a submitted command reaches the cell on its tick and no other", "[lab][commands]") {
    LabHarness h(kSmall);
    auto kernel = h.kernel(3);
    auto& cells = atlas::lab::cell_table(h.lab.world, h.lab.ids);
    cells.color_index[10] = 0;
    const auto payload = encode_set_color_index(10, 7);
    REQUIRE(h.commands.submit(1, atlas::sim::SourceId::Local, kSetColorIndex, payload).has_value());

    REQUIRE(kernel.step().has_value());  // tick 0: not yet
    CHECK(cells.color_index[10] == 0);
    const auto report = kernel.step().value();  // tick 1
    CHECK(report.commands_applied == 1);
    CHECK(cells.color_index[10] == 7);
}

TEST_CASE("the queue refuses a command the validator rejects", "[lab][commands]") {
    LabHarness h(kSmall);
    const auto out_of_grid = encode_set_color_index(h.lab.layout.cell_count(), 0);
    CHECK_FALSE(
        h.commands.submit(0, atlas::sim::SourceId::Local, kSetColorIndex, out_of_grid).has_value());
}

TEST_CASE("the cell bound follows the grid rather than the registration", "[lab][commands]") {
    LabHarness h(kSmall);
    // Shrink the bound as a load onto a smaller grid would; the same payload is now refused.
    h.bound->store(4);
    const auto payload = encode_set_color_index(10, 1);
    CHECK_FALSE(
        h.commands.submit(0, atlas::sim::SourceId::Local, kSetColorIndex, payload).has_value());
    h.bound->store(h.lab.layout.cell_count());
    CHECK(h.commands.submit(0, atlas::sim::SourceId::Local, kSetColorIndex, payload).has_value());
}

TEST_CASE("synthetic commands are a function of seed and tick", "[lab][commands]") {
    LabHarness a(kSmall);
    LabHarness b(kSmall);
    REQUIRE(atlas::lab::submit_synthetic_commands(a.commands, 5, 3, 99, 256).has_value());
    REQUIRE(atlas::lab::submit_synthetic_commands(b.commands, 5, 3, 99, 256).has_value());
    const auto da = a.commands.drain(5);
    const auto db = b.commands.drain(5);
    REQUIRE(da.size() == 3);
    REQUIRE(db.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(da[i].payload == db[i].payload);
    }
    LabHarness c(kSmall);
    REQUIRE(atlas::lab::submit_synthetic_commands(c.commands, 5, 3, 100, 256).has_value());
    const auto dc = c.commands.drain(5);
    bool any_differs = false;
    for (std::size_t i = 0; i < 3; ++i) {
        any_differs = any_differs || dc[i].payload != da[i].payload;
    }
    CHECK(any_differs);
}
