// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/save_stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using atlas::ErrorCode;
using atlas::sim::SaveReader;
using atlas::sim::SaveWriter;

TEST_CASE("every width round-trips", "[sim][save]") {
    SaveWriter writer;
    writer.write_u8(0xAB);
    writer.write_u16(0xABCD);
    writer.write_u32(0xABCD'EF01);
    writer.write_u64(0xABCD'EF01'2345'6789);
    writer.write_i8(-128);
    writer.write_i16(-32768);
    writer.write_i32(std::numeric_limits<std::int32_t>::min());
    writer.write_i64(std::numeric_limits<std::int64_t>::min());
    writer.write_bool(true);
    writer.write_bool(false);

    SaveReader reader(writer.bytes());
    CHECK(reader.read_u8().value() == 0xAB);
    CHECK(reader.read_u16().value() == 0xABCD);
    CHECK(reader.read_u32().value() == 0xABCD'EF01);
    CHECK(reader.read_u64().value() == 0xABCD'EF01'2345'6789);
    CHECK(reader.read_i8().value() == -128);
    CHECK(reader.read_i16().value() == -32768);
    CHECK(reader.read_i32().value() == std::numeric_limits<std::int32_t>::min());
    CHECK(reader.read_i64().value() == std::numeric_limits<std::int64_t>::min());
    CHECK(reader.read_bool().value() == true);
    CHECK(reader.read_bool().value() == false);
    CHECK(reader.expect_end().has_value());
}

TEST_CASE("integers are written little-endian regardless of the host", "[sim][save]") {
    // Pinned to exact bytes, not merely round-tripped. A round trip passes even if the
    // writer emits the host's order, which would produce a file that only reads back on
    // machines like the one that wrote it.
    SaveWriter writer;
    writer.write_u32(0x1122'3344);

    const auto bytes = writer.bytes();
    REQUIRE(bytes.size() == 4);
    CHECK(std::to_integer<int>(bytes[0]) == 0x44);
    CHECK(std::to_integer<int>(bytes[1]) == 0x33);
    CHECK(std::to_integer<int>(bytes[2]) == 0x22);
    CHECK(std::to_integer<int>(bytes[3]) == 0x11);
}

TEST_CASE("floating point survives exactly", "[sim][save]") {
    // Through the bit pattern, so a value is not rounded through a decimal form on the way.
    SaveWriter writer;
    writer.write_f32(0.1F);
    writer.write_f64(0.1);
    writer.write_f32(-0.0F);
    writer.write_f64(std::numeric_limits<double>::denorm_min());

    SaveReader reader(writer.bytes());
    CHECK(reader.read_f32().value() == 0.1F);
    CHECK(reader.read_f64().value() == 0.1);

    const float negative_zero = reader.read_f32().value();
    CHECK(negative_zero == 0.0F);
    CHECK(std::signbit(negative_zero));

    CHECK(reader.read_f64().value() == std::numeric_limits<double>::denorm_min());
}

TEST_CASE("strings round-trip, including empty and embedded nulls", "[sim][save]") {
    SaveWriter writer;
    writer.write_string("hello");
    writer.write_string("");
    writer.write_string(std::string_view{"a\0b", 3});

    SaveReader reader(writer.bytes());
    CHECK(reader.read_string().value() == "hello");
    CHECK(reader.read_string().value().empty());
    CHECK(reader.read_string().value() == std::string(std::string_view{"a\0b", 3}));
}

TEST_CASE("a truncated buffer is refused rather than read past", "[sim][save]") {
    SaveWriter writer;
    writer.write_u64(1);

    // Every prefix short of the whole must fail, not just the empty one.
    for (std::size_t length = 0; length < 8; ++length) {
        SaveReader reader(writer.bytes().subspan(0, length));
        const auto value = reader.read_u64();
        INFO("prefix of " << length << " byte(s)");
        REQUIRE_FALSE(value.has_value());
        CHECK(value.error().code() == ErrorCode::MalformedData);
    }
}

TEST_CASE("a byte that is neither true nor false is corruption", "[sim][save]") {
    const std::array<std::byte, 1> raw{std::byte{2}};
    SaveReader reader(raw);
    const auto value = reader.read_bool();
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("a string longer than allowed is refused", "[sim][save]") {
    SaveWriter writer;
    writer.write_string(std::string(500, 'x'));

    SaveReader reader(writer.bytes());
    const auto text = reader.read_string(100);
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("a string claiming more than the buffer holds is refused", "[sim][save]") {
    // The length says a thousand characters and three bytes follow. Trusting the length
    // would read a thousand bytes from wherever the buffer happens to sit.
    SaveWriter writer;
    writer.write_u32(1000);
    writer.write_bytes(std::array<std::byte, 3>{std::byte{1}, std::byte{2}, std::byte{3}});

    SaveReader reader(writer.bytes());
    const auto text = reader.read_string();
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("a count beyond its ceiling is refused", "[sim][save]") {
    SaveWriter writer;
    writer.write_u64(5000);

    SaveReader reader(writer.bytes());
    const auto count = reader.read_count(100, 0);
    REQUIRE_FALSE(count.has_value());
    CHECK(count.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("a count the file cannot fill is refused before allocating", "[sim][save]") {
    // The cheapest attack on a binary format: a small header claiming an enormous number of
    // rows. Checking the count against the bytes that remain costs nothing and removes it.
    SaveWriter writer;
    writer.write_u64(1'000'000);
    writer.write_u32(7);

    SaveReader reader(writer.bytes());
    const auto count = reader.read_count(10'000'000, 6);
    REQUIRE_FALSE(count.has_value());
    CHECK(count.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("a count that does not overflow size_t is still checked as 64 bits", "[sim][save]") {
    // Compared before narrowing. Narrowing first would wrap this into a small, plausible
    // count on a platform where size_t is 32 bits.
    SaveWriter writer;
    writer.write_u64(0x1'0000'0001ULL);

    SaveReader reader(writer.bytes());
    const auto count = reader.read_count(1000, 0);
    REQUIRE_FALSE(count.has_value());
    CHECK(count.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("a plausible count is accepted", "[sim][save]") {
    SaveWriter writer;
    writer.write_u64(2);
    writer.write_u64(0);
    writer.write_u64(0);

    SaveReader reader(writer.bytes());
    const auto count = reader.read_count(1000, 8);
    REQUIRE(count.has_value());
    CHECK(*count == 2);
}

TEST_CASE("leftover bytes are refused", "[sim][save]") {
    // A file with data the reader did not consume does not say what the reader thinks it
    // says. Accepting it would mean accepting a file only partly understood.
    SaveWriter writer;
    writer.write_u32(1);
    writer.write_u32(2);

    SaveReader reader(writer.bytes());
    REQUIRE(reader.read_u32().has_value());

    const auto status = reader.expect_end();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("the offset advances by exactly what was read", "[sim][save]") {
    SaveWriter writer;
    writer.write_u8(1);
    writer.write_u32(2);
    writer.write_u64(3);

    SaveReader reader(writer.bytes());
    CHECK(reader.offset() == 0);
    CHECK(reader.remaining() == 13);
    REQUIRE(reader.read_u8().has_value());
    CHECK(reader.offset() == 1);
    REQUIRE(reader.read_u32().has_value());
    CHECK(reader.offset() == 5);
    REQUIRE(reader.read_u64().has_value());
    CHECK(reader.offset() == 13);
    CHECK(reader.at_end());
}

TEST_CASE("a failed read does not advance the offset", "[sim][save]") {
    // Otherwise a caller that handled the error and carried on would resume mid-value.
    SaveWriter writer;
    writer.write_u16(1);

    SaveReader reader(writer.bytes());
    REQUIRE_FALSE(reader.read_u64().has_value());
    CHECK(reader.offset() == 0);
    CHECK(reader.read_u16().value() == 1);
}
