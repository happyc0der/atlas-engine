// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moved verbatim out of replay.cpp's anonymous namespace in M14. Nothing about the bytes
// changed, which the byte-exact test beside it is there to prove: a recording written before
// this move reads identically after it.

#include <atlas/simulation/command_codec.hpp>

namespace atlas::sim {

void write_command(SaveWriter& writer, const Command& command) {
    writer.write_u64(command.target);
    writer.write_u32(static_cast<std::uint32_t>(command.source));
    writer.write_u64(command.sequence);
    writer.write_u32(static_cast<std::uint32_t>(command.type));
    writer.write_u64(static_cast<std::uint64_t>(command.payload.size()));
    writer.write_bytes(command.payload);
}

Result<Command> read_command(SaveReader& reader) {
    Command command;

    auto target = reader.read_u64();
    if (!target) {
        return std::unexpected(std::move(target).error().context("a command's target tick"));
    }
    command.target = *target;

    auto source = reader.read_u32();
    if (!source) {
        return std::unexpected(std::move(source).error().context("a command's source"));
    }
    command.source = SourceId{*source};

    auto sequence = reader.read_u64();
    if (!sequence) {
        return std::unexpected(std::move(sequence).error().context("a command's sequence number"));
    }
    command.sequence = *sequence;

    auto type = reader.read_u32();
    if (!type) {
        return std::unexpected(std::move(type).error().context("a command's type"));
    }
    command.type = CommandType{*type};

    // Bounded before anything is reserved. A buffer claiming a four-gigabyte payload is refused
    // here rather than after an allocation that was never going to succeed.
    auto length = reader.read_count(CommandQueue::kMaxPayload, 1);
    if (!length) {
        return std::unexpected(std::move(length).error().context("a command payload length"));
    }
    auto payload = reader.read_bytes(*length);
    if (!payload) {
        return std::unexpected(std::move(payload).error().context("a command payload"));
    }
    command.payload.assign(payload->begin(), payload->end());

    return command;
}

}  // namespace atlas::sim
