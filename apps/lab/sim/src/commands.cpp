// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/commands.hpp>
#include <atlas/simulation/rng.hpp>

#include <format>
#include <utility>

namespace atlas::lab {

std::array<std::byte, kSetColorIndexBytes> encode_set_color_index(std::uint32_t cell,
                                                                  std::uint8_t color) noexcept {
    return {
        std::byte{static_cast<std::uint8_t>(cell & 0xFFU)},
        std::byte{static_cast<std::uint8_t>((cell >> 8U) & 0xFFU)},
        std::byte{static_cast<std::uint8_t>((cell >> 16U) & 0xFFU)},
        std::byte{static_cast<std::uint8_t>((cell >> 24U) & 0xFFU)},
        std::byte{color},
    };
}

Result<SetColorIndex> decode_set_color_index(std::span<const std::byte> payload,
                                             std::uint32_t cell_count) {
    if (payload.size() != kSetColorIndexBytes) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("set_color_index needs {} bytes, got {}",
                                                        kSetColorIndexBytes, payload.size())));
    }
    SetColorIndex command;
    command.cell = static_cast<std::uint32_t>(payload[0]) |
                   (static_cast<std::uint32_t>(payload[1]) << 8U) |
                   (static_cast<std::uint32_t>(payload[2]) << 16U) |
                   (static_cast<std::uint32_t>(payload[3]) << 24U);
    command.color = static_cast<std::uint8_t>(payload[4]);
    if (command.cell >= cell_count) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange,
                  std::format("set_color_index names cell {} of {}", command.cell, cell_count)));
    }
    if (command.color >= kColorCount) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange, std::format("set_color_index asks for colour {} of {}",
                                                     command.color, kColorCount)));
    }
    return command;
}

Status register_lab_commands(sim::CommandQueue& commands, const TableIds& ids,
                             const std::shared_ptr<const CellBound>& cell_bound) {
    sim::CommandHandler handler;
    handler.validate = [cell_bound](std::span<const std::byte> payload) -> Status {
        auto decoded = decode_set_color_index(payload, cell_bound->load());
        if (!decoded) {
            return std::unexpected(std::move(decoded).error());
        }
        return ok();
    };
    handler.apply = [cell_bound, ids](sim::World& world, std::span<const std::byte> payload) {
        // Decoded again against the bound as it is now. A command that no longer fits, which
        // can only mean the grid changed between validation and this tick, is not applied at
        // all rather than applied to a cell that happens to exist.
        auto decoded = decode_set_color_index(payload, cell_bound->load());
        if (!decoded) {
            return;
        }
        cell_table(world, ids).color_index[decoded->cell] = decoded->color;
    };
    return commands.register_handler(kSetColorIndex, std::move(handler));
}

Status submit_synthetic_commands(sim::CommandQueue& commands, Tick target, std::uint32_t count,
                                 std::uint64_t seed, std::uint32_t cell_count) {
    if (cell_count == 0) {
        return ok();
    }
    sim::RngStream stream(seed, sim::stream_id("synthetic_commands"), target);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto cell = static_cast<std::uint32_t>(stream.next_below(cell_count));
        const auto color = static_cast<std::uint8_t>(stream.next_below(kColorCount));
        const auto payload = encode_set_color_index(cell, color);
        if (auto status = commands.submit(target, sim::SourceId::Local, kSetColorIndex, payload);
            !status) {
            return status;
        }
    }
    return ok();
}

}  // namespace atlas::lab
