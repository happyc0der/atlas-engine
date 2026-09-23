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
    handler.apply = [cell_bound, ids](sim::World& world, const sim::ApplyContext&,
                                      std::span<const std::byte> payload) -> Status {
        // Decoded again against the bound as it is now. The queue re-validated an instant ago
        // against the same bound, so on one thread this cannot fail; it is kept because the
        // alternative is indexing a table with a number nobody checked, and until M19 that
        // was exactly what the next line did.
        auto decoded = decode_set_color_index(payload, cell_bound->load());
        if (!decoded) {
            return std::unexpected(std::move(decoded).error());
        }
        // The bound is what the application says the grid is; the table is what the world
        // holds. They agree in every run the lab makes, and nothing enforced it: a bound that
        // lagged a load would have written past the end of the table. This is the decline
        // ADR-0019 exists for — a well-formed command the world refuses — and the first one
        // in the tree. Before M19 the handler returned silently and the kernel counted the
        // command as applied.
        auto& cells = cell_table(world, ids);
        if (decoded->cell >= cells.color_index.size()) {
            return std::unexpected(
                Error(ErrorCode::OutOfRange,
                      std::format("set_color_index names cell {} and the world holds {}",
                                  decoded->cell, cells.color_index.size())));
        }
        cells.color_index[decoded->cell] = decoded->color;
        return ok();
    };
    return commands.register_handler(kSetColorIndex, std::move(handler));
}

std::vector<std::vector<std::byte>>
synthetic_payloads(Tick target, std::uint32_t count, std::uint64_t seed, std::uint32_t cell_count) {
    std::vector<std::vector<std::byte>> payloads;
    if (cell_count == 0) {
        return payloads;
    }
    payloads.reserve(count);
    sim::RngStream stream(seed, sim::stream_id("synthetic_commands"), target);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto cell = static_cast<std::uint32_t>(stream.next_below(cell_count));
        const auto color = static_cast<std::uint8_t>(stream.next_below(kColorCount));
        const auto encoded = encode_set_color_index(cell, color);
        payloads.emplace_back(encoded.begin(), encoded.end());
    }
    return payloads;
}

Status submit_synthetic_commands(sim::CommandQueue& commands, Tick target, std::uint32_t count,
                                 std::uint64_t seed, std::uint32_t cell_count,
                                 sim::SourceId source) {
    for (const auto& payload : synthetic_payloads(target, count, seed, cell_count)) {
        if (auto status = commands.submit(target, source, kSetColorIndex, payload); !status) {
            return status;
        }
    }
    return ok();
}

}  // namespace atlas::lab
