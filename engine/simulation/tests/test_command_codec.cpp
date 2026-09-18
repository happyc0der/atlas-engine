// SPDX-License-Identifier: GPL-3.0-or-later
// The command codec, and the bytes it produces.
//
// This file exists because the codec was moved in M14 rather than written. A round trip proves
// the pair agree with each other and would pass just as happily on a rewritten format that
// agreed with itself — and there are recordings on disk, and an integration check that unpacks
// the layout independently in Python. So the first case pins the bytes.
#include <atlas/simulation/command_codec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <vector>

using atlas::sim::Command;
using atlas::sim::CommandQueue;
using atlas::sim::CommandType;
using atlas::sim::kCommandOverhead;
using atlas::sim::read_command;
using atlas::sim::SaveReader;
using atlas::sim::SaveWriter;
using atlas::sim::SourceId;
using atlas::sim::write_command;

TEST_CASE("a command encodes to exactly these bytes", "[sim][codec]") {
    // Little-endian, in this order, with the payload length as a whole 64-bit field. Spelled out
    // rather than round-tripped because this is a file format: recordings exist that were
    // written before the codec moved, and `tests/integration/lab_checks.py` unpacks the same
    // layout with struct.unpack to tamper with one. A change here is a replay format version
    // change, and this case is what makes that obvious to whoever makes it.
    const Command command{
        .target = 0x0102'0304'0506'0708ULL,
        .source = SourceId{0x0A0B'0C0D},
        .sequence = 0x1112'1314'1516'1718ULL,
        .type = CommandType{0x2122'2324},
        .payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}},
    };

    SaveWriter writer;
    write_command(writer, command);

    const std::array<std::uint8_t, 35> expected{
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // target
        0x0D, 0x0C, 0x0B, 0x0A,                          // source
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,  // sequence
        0x24, 0x23, 0x22, 0x21,                          // type
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // payload length
        0xAA, 0xBB, 0xCC,                                // payload
    };

    const auto bytes = writer.bytes();
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes.size() == kCommandOverhead + command.payload.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("byte " << i);
        CHECK(static_cast<std::uint8_t>(bytes[i]) == expected[i]);
    }
}

TEST_CASE("a command survives a round trip at every payload size that matters", "[sim][codec]") {
    for (const std::size_t length :
         {std::size_t{0}, std::size_t{1}, std::size_t{255}, CommandQueue::kMaxPayload}) {
        INFO("payload of " << length << " bytes");
        Command original{
            .target = 41,
            .source = SourceId{7},
            .sequence = 9001,
            .type = CommandType{1234},
            .payload = {},
        };
        original.payload.resize(length);
        for (std::size_t i = 0; i < length; ++i) {
            original.payload[i] = static_cast<std::byte>(i % 251);
        }

        SaveWriter writer;
        write_command(writer, original);
        SaveReader reader(writer.bytes());
        const auto restored = read_command(reader);
        REQUIRE(restored.has_value());
        CHECK(reader.at_end());

        CHECK(restored->target == original.target);
        CHECK(restored->source == original.source);
        CHECK(restored->sequence == original.sequence);
        CHECK(restored->type == original.type);
        REQUIRE(restored->payload.size() == original.payload.size());
        CHECK(restored->payload == original.payload);
    }
}

TEST_CASE("a payload larger than the queue accepts is refused before it is reserved",
          "[sim][codec]") {
    // The cheapest attack on a binary format: claim an enormous count in a small buffer. The
    // length goes through read_count, so this fails on the claim rather than on the allocation.
    SaveWriter writer;
    writer.write_u64(1);                              // target
    writer.write_u32(0);                              // source
    writer.write_u64(0);                              // sequence
    writer.write_u32(1);                              // type
    writer.write_u64(CommandQueue::kMaxPayload + 1);  // payload length
    writer.write_u8(0);                               // one byte of the payload it claims

    SaveReader reader(writer.bytes());
    const auto refused = read_command(reader);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == atlas::ErrorCode::MalformedData);
    // The context names which field, because "malformed" without a field is not a starting
    // point for anybody holding a file that will not load.
    CHECK(refused.error().message().contains("payload length"));
}

TEST_CASE("a truncated command is refused at every length", "[sim][codec]") {
    // Cut at every length rather than at one chosen point: the interesting cuts are inside a
    // field, and picking them by hand means picking the ones already thought of.
    const Command command{
        .target = 5,
        .source = SourceId{2},
        .sequence = 3,
        .type = CommandType{9},
        .payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}},
    };
    SaveWriter writer;
    write_command(writer, command);
    const std::vector<std::byte> whole(writer.bytes().begin(), writer.bytes().end());

    for (std::size_t length = 0; length < whole.size(); ++length) {
        INFO("cut at " << length);
        SaveReader reader(std::span<const std::byte>{whole}.subspan(0, length));
        CHECK_FALSE(read_command(reader).has_value());
    }

    SaveReader complete(whole);
    CHECK(read_command(complete).has_value());
}
