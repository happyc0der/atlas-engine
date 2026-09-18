// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/net/message.hpp>
#include <atlas/simulation/command_codec.hpp>
#include <atlas/simulation/save_stream.hpp>

#include <format>

namespace atlas::net {
namespace {

using sim::SaveReader;
using sim::SaveWriter;

/// Magic, protocol version, type. Sixteen bytes in front of every message.
constexpr std::size_t kHeaderBytes = 8 + 4 + 4;

/// Smallest a system hash encodes to, for bounding a count against the bytes that remain.
constexpr std::size_t kSystemHashBytes = 4 + 8;

[[nodiscard]] Error too_large(std::string_view what, std::size_t held, std::size_t limit) {
    return {ErrorCode::Exhausted,
            std::format("a message holding {} {} is past the limit of {}; it would be refused by "
                        "whoever received it",
                        held, what, limit)};
}

void write_header(SaveWriter& writer, MessageType type) {
    writer.write_u64(kNetMagic);
    writer.write_u32(kProtocolVersion);
    writer.write_u32(static_cast<std::uint32_t>(type));
}

/// The type a buffer claims, having checked it is one this build has.
[[nodiscard]] Result<MessageType> read_header(SaveReader& reader) {
    auto magic = reader.read_u64();
    if (!magic) {
        return std::unexpected(std::move(magic).error().context("reading a message magic"));
    }
    if (*magic != kNetMagic) {
        return std::unexpected(Error(ErrorCode::MalformedData,
                                     "this is not an Atlas protocol message: the leading magic "
                                     "value does not match"));
    }

    auto version = reader.read_u32();
    if (!version) {
        return std::unexpected(std::move(version).error().context("reading a protocol version"));
    }
    if (*version != kProtocolVersion) {
        return std::unexpected(
            Error(ErrorCode::VersionMismatch,
                  std::format("this peer speaks protocol version {} and this build speaks {}",
                              *version, kProtocolVersion)));
    }

    auto type = reader.read_u32();
    if (!type) {
        return std::unexpected(std::move(type).error().context("reading a message type"));
    }
    switch (static_cast<MessageType>(*type)) {
    case MessageType::Hello:
    case MessageType::Welcome:
    case MessageType::Turn:
    case MessageType::HashCheck:
    case MessageType::Bye: return static_cast<MessageType>(*type);
    }
    // Refused rather than skipped. A protocol that ignores what it does not understand cannot
    // be versioned later without silently changing meaning, and a peer able to make another
    // ignore a message is a peer able to make it ignore a turn.
    return std::unexpected(
        Error(ErrorCode::MalformedData,
              std::format("message type {} is not one this build has; a message that cannot be "
                          "understood is a protocol violation rather than something to skip",
                          *type)));
}

// ------------------------------------------------------------------------------- encoding

[[nodiscard]] Status write_body(SaveWriter& writer, const Hello& hello) {
    if (hello.build_id.size() > kMaxBuildIdBytes) {
        return std::unexpected(
            too_large("build identifier bytes", hello.build_id.size(), kMaxBuildIdBytes));
    }
    write_header(writer, MessageType::Hello);
    writer.write_u32(hello.hash_algorithm_version);
    writer.write_u32(hello.save_format_version);
    writer.write_u32(hello.replay_format_version);
    writer.write_string(hello.build_id);
    writer.write_u64(hello.golden_final_state);
    writer.write_u64(hello.golden_all_ticks);
    writer.write_u64(hello.seed);
    writer.write_u64(hello.start_tick);
    writer.write_u64(hello.initial_state_hash);
    writer.write_u32(hello.tick_rate);
    writer.write_u32(hello.proposed_delay);
    return {};
}

[[nodiscard]] Status write_body(SaveWriter& writer, const Welcome& welcome) {
    if (welcome.peers.size() > kMaxPeers) {
        return std::unexpected(too_large("peers", welcome.peers.size(), kMaxPeers));
    }
    // Checked when writing as well as when reading. This list becomes the gate's expectation
    // set, and a gate told to expect one peer twice never becomes ready — so an encoder that
    // could produce one would be building a session that cannot start.
    for (std::size_t i = 1; i < welcome.peers.size(); ++i) {
        if (!(welcome.peers[i - 1] < welcome.peers[i])) {
            return std::unexpected(Error(ErrorCode::InvalidArgument,
                                         "a welcome's peer list must be strictly increasing"));
        }
    }

    write_header(writer, MessageType::Welcome);
    writer.write_u32(static_cast<std::uint32_t>(welcome.assigned_source));
    writer.write_u64(static_cast<std::uint64_t>(welcome.peers.size()));
    for (const sim::SourceId peer : welcome.peers) {
        writer.write_u32(static_cast<std::uint32_t>(peer));
    }
    writer.write_u64(welcome.agreed_seed);
    writer.write_u64(welcome.start_tick);
    writer.write_u64(welcome.initial_state_hash);
    writer.write_u32(welcome.agreed_delay);
    return {};
}

[[nodiscard]] Status write_body(SaveWriter& writer, const Turn& turn) {
    if (turn.commands.size() > kMaxCommandsPerTurn) {
        return std::unexpected(too_large("commands", turn.commands.size(), kMaxCommandsPerTurn));
    }
    // The command codec's reader bounds a payload at the queue's own limit, so a payload past
    // it would encode and never decode. A command that came through the queue cannot be past
    // it; one assembled by hand can.
    for (const sim::Command& command : turn.commands) {
        if (command.payload.size() > sim::CommandQueue::kMaxPayload) {
            return std::unexpected(too_large("payload bytes in one command", command.payload.size(),
                                             sim::CommandQueue::kMaxPayload));
        }
    }

    write_header(writer, MessageType::Turn);
    writer.write_u64(turn.tick);
    writer.write_u32(static_cast<std::uint32_t>(turn.source));
    writer.write_u64(static_cast<std::uint64_t>(turn.commands.size()));
    for (const sim::Command& command : turn.commands) {
        sim::write_command(writer, command);
    }
    return {};
}

[[nodiscard]] Status write_body(SaveWriter& writer, const HashCheck& check) {
    if (check.system_hashes.size() > kMaxSystemHashesPerCheck) {
        return std::unexpected(
            too_large("system hashes", check.system_hashes.size(), kMaxSystemHashesPerCheck));
    }
    write_header(writer, MessageType::HashCheck);
    writer.write_u64(check.tick);
    writer.write_u32(static_cast<std::uint32_t>(check.source));
    writer.write_u64(check.state_hash);
    writer.write_u64(static_cast<std::uint64_t>(check.system_hashes.size()));
    for (const sim::SystemHash& hash : check.system_hashes) {
        writer.write_u32(static_cast<std::uint32_t>(hash.system));
        writer.write_u64(hash.hash);
    }
    return {};
}

[[nodiscard]] Status write_body(SaveWriter& writer, const Bye& bye) {
    if (bye.detail.size() > kMaxByeDetailBytes) {
        return std::unexpected(too_large("detail bytes", bye.detail.size(), kMaxByeDetailBytes));
    }
    write_header(writer, MessageType::Bye);
    writer.write_u32(static_cast<std::uint32_t>(bye.reason));
    writer.write_string(bye.detail);
    return {};
}

// ------------------------------------------------------------------------------- decoding

[[nodiscard]] Result<Message> read_hello(SaveReader& reader) {
    Hello hello;
    auto hash_version = reader.read_u32();
    if (!hash_version) {
        return std::unexpected(std::move(hash_version).error().context("a hash algorithm version"));
    }
    hello.hash_algorithm_version = *hash_version;

    auto save_version = reader.read_u32();
    if (!save_version) {
        return std::unexpected(std::move(save_version).error().context("a save format version"));
    }
    hello.save_format_version = *save_version;

    auto replay_version = reader.read_u32();
    if (!replay_version) {
        return std::unexpected(
            std::move(replay_version).error().context("a replay format version"));
    }
    hello.replay_format_version = *replay_version;

    auto build_id = reader.read_string(kMaxBuildIdBytes);
    if (!build_id) {
        return std::unexpected(std::move(build_id).error().context("a build identifier"));
    }
    hello.build_id = *std::move(build_id);

    for (std::uint64_t* field : {&hello.golden_final_state, &hello.golden_all_ticks, &hello.seed,
                                 &hello.start_tick, &hello.initial_state_hash}) {
        auto value = reader.read_u64();
        if (!value) {
            return std::unexpected(std::move(value).error().context("a handshake field"));
        }
        *field = *value;
    }

    auto tick_rate = reader.read_u32();
    if (!tick_rate) {
        return std::unexpected(std::move(tick_rate).error().context("a tick rate"));
    }
    hello.tick_rate = *tick_rate;

    auto delay = reader.read_u32();
    if (!delay) {
        return std::unexpected(std::move(delay).error().context("a proposed input delay"));
    }
    hello.proposed_delay = *delay;

    return Message{std::move(hello)};
}

[[nodiscard]] Result<Message> read_welcome(SaveReader& reader) {
    Welcome welcome;
    auto assigned = reader.read_u32();
    if (!assigned) {
        return std::unexpected(std::move(assigned).error().context("an assigned source"));
    }
    welcome.assigned_source = sim::SourceId{*assigned};

    auto count = reader.read_count(kMaxPeers, 4);
    if (!count) {
        return std::unexpected(std::move(count).error().context("a peer count"));
    }
    welcome.peers.reserve(*count);
    for (std::size_t i = 0; i < *count; ++i) {
        auto peer = reader.read_u32();
        if (!peer) {
            return std::unexpected(std::move(peer).error().context("a peer identifier"));
        }
        const sim::SourceId id{*peer};
        // Strictly increasing, which makes it both sorted and free of duplicates in one test.
        // A duplicate would make the gate expect one peer twice and never become ready.
        if (i > 0 && !(welcome.peers.back() < id)) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      "a welcome's peer list is not strictly increasing, so it either repeats a "
                      "peer or is unordered"));
        }
        welcome.peers.push_back(id);
    }

    for (std::uint64_t* field :
         {&welcome.agreed_seed, &welcome.start_tick, &welcome.initial_state_hash}) {
        auto value = reader.read_u64();
        if (!value) {
            return std::unexpected(std::move(value).error().context("a welcome field"));
        }
        *field = *value;
    }

    auto delay = reader.read_u32();
    if (!delay) {
        return std::unexpected(std::move(delay).error().context("an agreed input delay"));
    }
    welcome.agreed_delay = *delay;

    return Message{std::move(welcome)};
}

[[nodiscard]] Result<Message> read_turn(SaveReader& reader) {
    Turn turn;
    auto tick = reader.read_u64();
    if (!tick) {
        return std::unexpected(std::move(tick).error().context("a turn's tick"));
    }
    turn.tick = *tick;

    auto source = reader.read_u32();
    if (!source) {
        return std::unexpected(std::move(source).error().context("a turn's source"));
    }
    turn.source = sim::SourceId{*source};

    auto count = reader.read_count(kMaxCommandsPerTurn, sim::kCommandOverhead);
    if (!count) {
        return std::unexpected(std::move(count).error().context("a turn's command count"));
    }
    turn.commands.reserve(*count);
    for (std::size_t i = 0; i < *count; ++i) {
        auto command = sim::read_command(reader);
        if (!command) {
            return std::unexpected(std::move(command).error().context("a command in a turn"));
        }
        turn.commands.push_back(*std::move(command));
    }

    return Message{std::move(turn)};
}

[[nodiscard]] Result<Message> read_hash_check(SaveReader& reader) {
    HashCheck check;
    auto tick = reader.read_u64();
    if (!tick) {
        return std::unexpected(std::move(tick).error().context("a hash check's tick"));
    }
    check.tick = *tick;

    auto source = reader.read_u32();
    if (!source) {
        return std::unexpected(std::move(source).error().context("a hash check's source"));
    }
    check.source = sim::SourceId{*source};

    auto state_hash = reader.read_u64();
    if (!state_hash) {
        return std::unexpected(std::move(state_hash).error().context("a state hash"));
    }
    check.state_hash = *state_hash;

    auto count = reader.read_count(kMaxSystemHashesPerCheck, kSystemHashBytes);
    if (!count) {
        return std::unexpected(std::move(count).error().context("a system hash count"));
    }
    check.system_hashes.reserve(*count);
    for (std::size_t i = 0; i < *count; ++i) {
        auto system = reader.read_u32();
        if (!system) {
            return std::unexpected(std::move(system).error().context("a system identifier"));
        }
        auto hash = reader.read_u64();
        if (!hash) {
            return std::unexpected(std::move(hash).error().context("a system hash"));
        }
        check.system_hashes.push_back(sim::SystemHash{sim::SystemId{*system}, *hash});
    }

    return Message{std::move(check)};
}

[[nodiscard]] Result<Message> read_bye(SaveReader& reader) {
    Bye bye;
    auto reason = reader.read_u32();
    if (!reason) {
        return std::unexpected(std::move(reason).error().context("a farewell reason"));
    }
    switch (static_cast<ByeReason>(*reason)) {
    case ByeReason::Quit:
    case ByeReason::ProtocolError:
    case ByeReason::Diverged:
    case ByeReason::Overflow:
    case ByeReason::VersionMismatch: bye.reason = static_cast<ByeReason>(*reason); break;
    default:
        // A reason this build does not have, like a type it does not have: refused rather than
        // guessed at, because "the session ended for a reason I invented" helps nobody.
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("farewell reason {} is not one this build has", *reason)));
    }

    auto detail = reader.read_string(kMaxByeDetailBytes);
    if (!detail) {
        return std::unexpected(std::move(detail).error().context("a farewell detail"));
    }
    bye.detail = *std::move(detail);

    return Message{std::move(bye)};
}

}  // namespace

Result<MessageType> peek_type(std::span<const std::byte> bytes) {
    SaveReader reader(bytes);
    return read_header(reader);
}

Result<std::vector<std::byte>> encode(const Message& message) {
    SaveWriter writer(kHeaderBytes + 64);

    const auto status = std::visit(
        [&writer](const auto& body) -> Status { return write_body(writer, body); }, message);
    if (!status) {
        return std::unexpected(status.error());
    }

    // After the fact as well as before it. The per-field checks above catch a count that is too
    // large; this catches a message that is within every count and still enormous, which a turn
    // of four thousand maximum-sized payloads would be.
    if (writer.size() > kMaxMessageBytes) {
        return std::unexpected(too_large("bytes", writer.size(), kMaxMessageBytes));
    }
    return writer.take();
}

Result<Message> decode(std::span<const std::byte> bytes) {
    // Before the reader, because this is the only bound on a buffer that arrives from outside.
    // Every count inside is checked against the bytes that remain, but the buffer itself has
    // already been allocated by whoever handed it over.
    if (bytes.size() > kMaxMessageBytes) {
        return std::unexpected(Error(ErrorCode::MalformedData,
                                     std::format("a message of {} bytes is past the limit of {}",
                                                 bytes.size(), kMaxMessageBytes)));
    }

    SaveReader reader(bytes);
    auto type = read_header(reader);
    if (!type) {
        return std::unexpected(std::move(type).error());
    }

    Result<Message> message = std::unexpected(Error(ErrorCode::Internal, "unreachable"));
    switch (*type) {
    case MessageType::Hello: message = read_hello(reader); break;
    case MessageType::Welcome: message = read_welcome(reader); break;
    case MessageType::Turn: message = read_turn(reader); break;
    case MessageType::HashCheck: message = read_hash_check(reader); break;
    case MessageType::Bye: message = read_bye(reader); break;
    }
    if (!message) {
        return message;
    }

    // Trailing bytes are refused. A message with something after it is either two messages run
    // together or one this build read wrongly, and neither is safe to act on.
    if (auto status = reader.expect_end(); !status) {
        return std::unexpected(std::move(status).error().context("after a message body"));
    }
    return message;
}

}  // namespace atlas::net
