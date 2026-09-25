// SPDX-License-Identifier: GPL-3.0-or-later
// The wire messages, built here and taken apart here.
//
// Two things are being checked. That the pair agree — which a round trip shows — and that the
// encoder never produces something the decoder refuses, which a round trip does not show and
// which is the failure the replay format actually had: ceilings where only the reader could see
// them, so a large enough recording was written successfully and never read again.
#include <atlas/net/inbox.hpp>
#include <atlas/net/message.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/rng.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

using atlas::net::Bye;
using atlas::net::ByeReason;
using atlas::net::decode;
using atlas::net::Drop;
using atlas::net::encode;
using atlas::net::Finish;
using atlas::net::HashCheck;
using atlas::net::Hello;
using atlas::net::kMaxBuildIdBytes;
using atlas::net::kMaxByeDetailBytes;
using atlas::net::kMaxCommandsPerTurn;
using atlas::net::kMaxPeers;
using atlas::net::kMaxSystemHashesPerCheck;
using atlas::net::Message;
using atlas::net::MessageType;
using atlas::net::peek_type;
using atlas::net::Turn;
using atlas::net::Welcome;
using atlas::sim::Command;
using atlas::sim::CommandType;
using atlas::sim::SourceId;
using atlas::sim::SystemHash;
using atlas::sim::SystemId;

namespace {

[[nodiscard]] Command command_with(std::size_t payload_bytes, std::uint64_t sequence) {
    Command command{
        .target = 40 + sequence,
        .source = SourceId{3},
        .sequence = sequence,
        .type = CommandType{77},
        .payload = {},
    };
    command.payload.resize(payload_bytes);
    for (std::size_t i = 0; i < payload_bytes; ++i) {
        command.payload[i] = static_cast<std::byte>((i + sequence) % 251);
    }
    return command;
}

/// One of every message, filled with values that are distinguishable if a field is swapped.
[[nodiscard]] std::vector<Message> every_message() {
    Hello hello{
        .hash_algorithm_version = 2,
        .save_format_version = 1,
        .replay_format_version = 1,
        .build_id = "atlas 0.0.1 (abc123) Debug",
        .golden_final_state = 0xAA82'430D'E232'1AFFULL,
        .golden_all_ticks = 0x2603'546C'5687'E95EULL,
        .seed = 0x0A71'A500ULL,
        .start_tick = 12,
        .initial_state_hash = 0xDEAD'BEEFULL,
        .tick_rate = 60,
        .proposed_delay = 2,
    };
    Welcome welcome{
        .assigned_source = SourceId{2},
        .peers = {SourceId{0}, SourceId{1}, SourceId{2}},
        .agreed_seed = 99,
        .start_tick = 12,
        .initial_state_hash = 0xDEAD'BEEFULL,
        .agreed_delay = 3,
    };
    Turn turn{
        .tick = 41,
        .source = SourceId{1},
        .commands = {command_with(0, 0), command_with(5, 1), command_with(64, 2)},
    };
    HashCheck check{
        .tick = 41,
        .source = SourceId{1},
        .state_hash = 0x1234'5678ULL,
        .system_hashes = {{SystemId{11}, 111}, {SystemId{22}, 222}},
    };
    Bye bye{.reason = ByeReason::Diverged, .detail = "the first system to differ is 'drift'"};
    // Past 2^32, so a writer or reader that narrowed the tick to 32 bits is caught.
    Finish finish{.last_tick = 0x1'0000'0029ULL};
    // Past 2^32 for the same reason: a count narrowed to 32 bits would be caught.
    Drop drop{.source = SourceId{2}, .turns = 0x1'0000'0007ULL};

    return {Message{hello}, Message{welcome}, Message{turn}, Message{check},
            Message{bye},   Message{finish},  Message{drop}};
}

}  // namespace

TEST_CASE("a message at the protocol's ceiling survives the whole path", "[net][message]") {
    // **This is the case M17 exists to make possible, and it could not pass before it.**
    // `kMaxMessageBytes` was eight mebibytes against a `CommandInbox` budget of one, so every
    // message between the two encoded successfully, sent successfully, and was refused at the
    // far end by an overflow that ends the session and never clears. Nothing covered it because
    // the in-memory link is never asked for messages that size.
    //
    // So the path here is deliberately the whole one -- encode, push into an inbox, drain,
    // decode -- rather than a round trip through the codec alone, which is what would have kept
    // passing while the defect was live.
    Turn turn{.tick = 7, .source = SourceId{2}, .commands = {}};

    // **Derived from the constants rather than written down**, so this case keeps testing the
    // bound if the bound moves. A fixed payload would stop being near the ceiling the moment
    // somebody changed either number, and would then pass while proving nothing.
    //
    // The command count is capped first, because the two ceilings constrain each other: a
    // message may be `kMaxMessageBytes` and a turn may be `kMaxCommandsPerTurn` commands, so
    // the payload that fills one without breaching the other is what is solved for here.
    constexpr std::size_t kFraming = 32;  // target, source, sequence, type, payload length
    constexpr std::size_t kSlack = 1024;  // the envelope, and room to stay under rather than on
    const std::size_t count = atlas::net::kMaxCommandsPerTurn;
    const std::size_t payload = ((atlas::net::kMaxMessageBytes - kSlack) / count) - kFraming;
    REQUIRE(payload > 0);
    REQUIRE(payload <= atlas::sim::CommandQueue::kMaxPayload);

    turn.commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        turn.commands.push_back(command_with(payload, static_cast<std::uint64_t>(i)));
    }

    const auto bytes = encode(Message{turn});
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() <= atlas::net::kMaxMessageBytes);
    CHECK(bytes->size() > atlas::net::kMaxMessageBytes / 2);

    atlas::net::CommandInbox inbox;
    REQUIRE(inbox.push(*bytes) == atlas::net::CommandInbox::Push::Accepted);
    CHECK_FALSE(inbox.overflowed());

    std::vector<std::vector<std::byte>> drained;
    inbox.drain(drained);
    REQUIRE(drained.size() == 1);

    const auto restored = decode(drained.front());
    REQUIRE(restored.has_value());
    const auto* received = std::get_if<Turn>(&*restored);
    REQUIRE(received != nullptr);
    CHECK(received->commands.size() == count);
    CHECK(received->commands.back().payload.size() == payload);
}

TEST_CASE("every message survives a round trip, field by field", "[net][message]") {
    // Field by field rather than by variant index. An earlier version of this case compared
    // only the index, which passes on a decoder that reads every field into the wrong member —
    // and a handshake whose seed and start tick are swapped refuses every peer for a reason
    // nobody could work out. The values above are chosen to be distinguishable if any two
    // same-typed fields trade places.
    SECTION("hello") {
        const auto original = std::get<Hello>(every_message()[0]);
        const auto bytes = encode(Message{original});
        REQUIRE(bytes.has_value());
        const auto restored = decode(*bytes);
        REQUIRE(restored.has_value());
        const auto* back = std::get_if<Hello>(&*restored);
        REQUIRE(back != nullptr);
        CHECK(back->hash_algorithm_version == original.hash_algorithm_version);
        CHECK(back->save_format_version == original.save_format_version);
        CHECK(back->replay_format_version == original.replay_format_version);
        CHECK(back->build_id == original.build_id);
        CHECK(back->golden_final_state == original.golden_final_state);
        CHECK(back->golden_all_ticks == original.golden_all_ticks);
        CHECK(back->seed == original.seed);
        CHECK(back->start_tick == original.start_tick);
        CHECK(back->initial_state_hash == original.initial_state_hash);
        CHECK(back->tick_rate == original.tick_rate);
        CHECK(back->proposed_delay == original.proposed_delay);
    }

    SECTION("welcome") {
        const auto original = std::get<Welcome>(every_message()[1]);
        const auto bytes = encode(Message{original});
        REQUIRE(bytes.has_value());
        const auto restored = decode(*bytes);
        REQUIRE(restored.has_value());
        const auto* back = std::get_if<Welcome>(&*restored);
        REQUIRE(back != nullptr);
        CHECK(back->assigned_source == original.assigned_source);
        CHECK(back->peers == original.peers);
        CHECK(back->agreed_seed == original.agreed_seed);
        CHECK(back->start_tick == original.start_tick);
        CHECK(back->initial_state_hash == original.initial_state_hash);
        CHECK(back->agreed_delay == original.agreed_delay);
    }

    SECTION("hash check") {
        const auto original = std::get<HashCheck>(every_message()[3]);
        const auto bytes = encode(Message{original});
        REQUIRE(bytes.has_value());
        const auto restored = decode(*bytes);
        REQUIRE(restored.has_value());
        const auto* back = std::get_if<HashCheck>(&*restored);
        REQUIRE(back != nullptr);
        CHECK(back->tick == original.tick);
        CHECK(back->source == original.source);
        CHECK(back->state_hash == original.state_hash);
        REQUIRE(back->system_hashes.size() == original.system_hashes.size());
        for (std::size_t i = 0; i < original.system_hashes.size(); ++i) {
            INFO("system hash " << i);
            CHECK(back->system_hashes[i].system == original.system_hashes[i].system);
            CHECK(back->system_hashes[i].hash == original.system_hashes[i].hash);
        }
    }

    SECTION("bye") {
        const auto original = std::get<Bye>(every_message()[4]);
        const auto bytes = encode(Message{original});
        REQUIRE(bytes.has_value());
        const auto restored = decode(*bytes);
        REQUIRE(restored.has_value());
        const auto* back = std::get_if<Bye>(&*restored);
        REQUIRE(back != nullptr);
        CHECK(back->reason == original.reason);
        CHECK(back->detail == original.detail);
    }

    SECTION("finish") {
        const auto original = std::get<Finish>(every_message()[5]);
        const auto bytes = encode(Message{original});
        REQUIRE(bytes.has_value());
        const auto restored = decode(*bytes);
        REQUIRE(restored.has_value());
        const auto* back = std::get_if<Finish>(&*restored);
        REQUIRE(back != nullptr);
        CHECK(back->last_tick == original.last_tick);
        CHECK(peek_type(*bytes).value() == atlas::net::MessageType::Finish);
    }

    SECTION("drop") {
        const auto original = std::get<Drop>(every_message()[6]);
        const auto bytes = encode(Message{original});
        REQUIRE(bytes.has_value());
        const auto restored = decode(*bytes);
        REQUIRE(restored.has_value());
        const auto* back = std::get_if<Drop>(&*restored);
        REQUIRE(back != nullptr);
        CHECK(back->source == original.source);
        CHECK(back->turns == original.turns);
        CHECK(peek_type(*bytes).value() == atlas::net::MessageType::Drop);
    }

    // The turn has its own case below, because it is the one whose contents are applied.
}

TEST_CASE("a turn carries its commands byte for byte", "[net][message]") {
    // The one message whose contents the simulation actually applies. A field swapped here is a
    // command applied to the wrong entity at the wrong tick, in the same order on every peer,
    // which is a divergence nobody would ever attribute to the network.
    const Turn turn{
        .tick = 41,
        .source = SourceId{1},
        .commands = {command_with(0, 0), command_with(5, 1), command_with(64, 2)},
    };
    const auto bytes = encode(Message{turn});
    REQUIRE(bytes.has_value());
    const auto restored = decode(*bytes);
    REQUIRE(restored.has_value());

    const auto* back = std::get_if<Turn>(&*restored);
    REQUIRE(back != nullptr);
    CHECK(back->tick == turn.tick);
    CHECK(back->source == turn.source);
    REQUIRE(back->commands.size() == turn.commands.size());
    for (std::size_t i = 0; i < turn.commands.size(); ++i) {
        INFO("command " << i);
        CHECK(back->commands[i].target == turn.commands[i].target);
        CHECK(back->commands[i].source == turn.commands[i].source);
        CHECK(back->commands[i].sequence == turn.commands[i].sequence);
        CHECK(back->commands[i].type == turn.commands[i].type);
        CHECK(back->commands[i].payload == turn.commands[i].payload);
    }
}

TEST_CASE("an empty turn is a message rather than nothing", "[net][message]") {
    // A peer with nothing to say still says it. Silence is what a dead peer produces, and a
    // gate with no timeout cannot tell the two apart.
    const Turn empty{.tick = 7, .source = SourceId{4}, .commands = {}};
    const auto bytes = encode(Message{empty});
    REQUIRE(bytes.has_value());
    const auto restored = decode(*bytes);
    REQUIRE(restored.has_value());
    const auto* back = std::get_if<Turn>(&*restored);
    REQUIRE(back != nullptr);
    CHECK(back->tick == 7);
    CHECK(back->commands.empty());
}

TEST_CASE("an encoder refuses exactly what its decoder would refuse", "[net][message]") {
    // The property, stated at each ceiling: at the limit it encodes and decodes, and one past
    // it the encoder refuses rather than producing something nothing can read.
    SECTION("peers") {
        Welcome welcome{.assigned_source = SourceId{0}};
        for (std::size_t i = 0; i < kMaxPeers; ++i) {
            welcome.peers.push_back(SourceId{static_cast<std::uint32_t>(i)});
        }
        const auto at_limit = encode(Message{welcome});
        REQUIRE(at_limit.has_value());
        CHECK(decode(*at_limit).has_value());

        welcome.peers.push_back(SourceId{static_cast<std::uint32_t>(kMaxPeers)});
        const auto past = encode(Message{welcome});
        REQUIRE_FALSE(past.has_value());
        CHECK(past.error().code() == atlas::ErrorCode::Exhausted);
    }

    SECTION("commands in a turn") {
        Turn turn{.tick = 1, .source = SourceId{0}};
        for (std::size_t i = 0; i < kMaxCommandsPerTurn; ++i) {
            turn.commands.push_back(command_with(1, i));
        }
        const auto at_limit = encode(Message{turn});
        REQUIRE(at_limit.has_value());
        CHECK(decode(*at_limit).has_value());

        turn.commands.push_back(command_with(1, kMaxCommandsPerTurn));
        CHECK_FALSE(encode(Message{turn}).has_value());
    }

    SECTION("system hashes in a check") {
        HashCheck check{.tick = 1, .source = SourceId{0}, .state_hash = 5};
        for (std::size_t i = 0; i < kMaxSystemHashesPerCheck; ++i) {
            check.system_hashes.push_back(SystemHash{SystemId{static_cast<std::uint32_t>(i)}, i});
        }
        const auto at_limit = encode(Message{check});
        REQUIRE(at_limit.has_value());
        CHECK(decode(*at_limit).has_value());

        check.system_hashes.push_back(SystemHash{SystemId{1}, 1});
        CHECK_FALSE(encode(Message{check}).has_value());
    }

    SECTION("a build identifier") {
        Hello hello;
        hello.build_id = std::string(kMaxBuildIdBytes, 'x');
        const auto at_limit = encode(Message{hello});
        REQUIRE(at_limit.has_value());
        CHECK(decode(*at_limit).has_value());

        hello.build_id.push_back('x');
        CHECK_FALSE(encode(Message{hello}).has_value());
    }

    SECTION("a farewell detail") {
        Bye bye{.reason = ByeReason::Quit, .detail = std::string(kMaxByeDetailBytes, 'y')};
        const auto at_limit = encode(Message{bye});
        REQUIRE(at_limit.has_value());
        CHECK(decode(*at_limit).has_value());

        bye.detail.push_back('y');
        CHECK_FALSE(encode(Message{bye}).has_value());
    }

    SECTION("a payload larger than the command queue accepts") {
        Turn turn{.tick = 1, .source = SourceId{0}};
        turn.commands.push_back(command_with(atlas::sim::CommandQueue::kMaxPayload + 1, 0));
        CHECK_FALSE(encode(Message{turn}).has_value());
    }
}

TEST_CASE("trailing bytes are refused for every message", "[net][message]") {
    // A message with something after it is either two run together or one this build read
    // wrongly, and neither is safe to act on.
    for (const Message& original : every_message()) {
        auto bytes = encode(original);
        REQUIRE(bytes.has_value());
        bytes->push_back(std::byte{0});
        const auto refused = decode(*bytes);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == atlas::ErrorCode::MalformedData);
    }
}

TEST_CASE("every message is refused at every truncation", "[net][message]") {
    // Cut at every length rather than at chosen points, and run under the address sanitizer,
    // where reading past a buffer is loud rather than lucky.
    for (const Message& original : every_message()) {
        const auto bytes = encode(original);
        REQUIRE(bytes.has_value());
        for (std::size_t length = 0; length < bytes->size(); ++length) {
            INFO("cut at " << length);
            CHECK_FALSE(decode(std::span<const std::byte>{*bytes}.subspan(0, length)).has_value());
        }
        CHECK(decode(*bytes).has_value());
    }
}

TEST_CASE("a message type this build does not have is a violation", "[net][message]") {
    // Refused, never skipped. A peer that can make another ignore a message can make it ignore
    // a turn, and a turn ignored is a tick that runs without somebody's commands.
    auto bytes = encode(Message{Bye{.reason = ByeReason::Quit, .detail = ""}});
    REQUIRE(bytes.has_value());
    // The type is the third field, after the eight-byte magic and the four-byte version.
    (*bytes)[12] = std::byte{0xEE};
    (*bytes)[13] = std::byte{0x00};
    const auto refused = decode(*bytes);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == atlas::ErrorCode::MalformedData);
    CHECK_FALSE(peek_type(*bytes).has_value());
}

TEST_CASE("a zero-filled buffer is not a message", "[net][message]") {
    // Which is why no type and no farewell reason is numbered zero.
    const std::vector<std::byte> zeros(64, std::byte{0});
    CHECK_FALSE(decode(zeros).has_value());
    CHECK_FALSE(peek_type(zeros).has_value());
}

TEST_CASE("a peer speaking another protocol version is told so", "[net][message]") {
    auto bytes = encode(Message{Hello{}});
    REQUIRE(bytes.has_value());
    (*bytes)[8] = std::byte{0x63};  // the version, immediately after the magic
    const auto refused = decode(*bytes);
    REQUIRE_FALSE(refused.has_value());
    // A distinct code, so a session can tell "we cannot talk" from "you sent rubbish" without
    // reading the message.
    CHECK(refused.error().code() == atlas::ErrorCode::VersionMismatch);
}

TEST_CASE("a peer list that is not strictly increasing is refused", "[net][message]") {
    // It becomes the gate's expectation set. A repeat makes the gate wait for one peer twice
    // and never become ready, so it is refused on both sides rather than sorted or deduplicated.
    Welcome repeated{.assigned_source = SourceId{0},
                     .peers = {SourceId{0}, SourceId{1}, SourceId{1}}};
    CHECK_FALSE(encode(Message{repeated}).has_value());

    Welcome unordered{.assigned_source = SourceId{0}, .peers = {SourceId{2}, SourceId{1}}};
    CHECK_FALSE(encode(Message{unordered}).has_value());

    // And a hand-built message carrying one is refused by the decoder too, because an encoder
    // that refuses is only half of the guarantee.
    const Welcome legal{.assigned_source = SourceId{0}, .peers = {SourceId{0}, SourceId{1}}};
    auto bytes = encode(Message{legal});
    REQUIRE(bytes.has_value());
    // The peer identifiers follow the header, the assigned source and the count: 16 + 4 + 8.
    (*bytes)[28] = std::byte{1};
    CHECK_FALSE(decode(*bytes).has_value());
}

TEST_CASE("a farewell reason this build does not have is refused", "[net][message]") {
    auto bytes = encode(Message{Bye{.reason = ByeReason::Quit, .detail = ""}});
    REQUIRE(bytes.has_value());
    (*bytes)[16] = std::byte{0x2A};  // the reason, immediately after the header
    CHECK_FALSE(decode(*bytes).has_value());
}

TEST_CASE("a drop naming no peer a session could have is refused", "[net][message]") {
    // A peer's index, never a mod's: mod identifiers are never sent on the wire, and an index
    // past the protocol's ceiling names nobody. The encoder writes whatever it is given, so the
    // reader is where this is enforced.
    for (const std::uint32_t source : {std::uint32_t{16}, std::uint32_t{0x8000'0000U}}) {
        auto bytes = encode(Message{Drop{.source = SourceId{source}, .turns = 1}});
        REQUIRE(bytes.has_value());
        CHECK_FALSE(decode(*bytes).has_value());
    }
    auto fine = encode(Message{Drop{.source = SourceId{15}, .turns = 1}});
    REQUIRE(fine.has_value());
    CHECK(decode(*fine).has_value());
}

TEST_CASE("a claimed count larger than the buffer is refused before it is reserved",
          "[net][message]") {
    // The cheapest attack on a binary format. read_count compares the claim against the bytes
    // that remain, so this fails on the claim rather than on an allocation that was never going
    // to succeed.
    const Turn turn{.tick = 1, .source = SourceId{0}, .commands = {command_with(4, 0)}};
    auto bytes = encode(Message{turn});
    REQUIRE(bytes.has_value());
    // The command count follows the header, the tick and the source: 16 + 8 + 4.
    (*bytes)[28] = std::byte{0xFF};
    (*bytes)[29] = std::byte{0xFF};
    const auto refused = decode(*bytes);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == atlas::ErrorCode::MalformedData);
}

TEST_CASE("the type can be read without decoding the rest", "[net][message]") {
    const auto bytes = encode(Message{Turn{.tick = 3, .source = SourceId{1}}});
    REQUIRE(bytes.has_value());
    const auto type = peek_type(*bytes);
    REQUIRE(type.has_value());
    CHECK(*type == MessageType::Turn);
}
