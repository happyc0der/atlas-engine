// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A tiny world for proving two kernels agree, built here rather than borrowed.
///
/// `net` cannot reach the simulation module's own test helpers, and should not: what this needs
/// is different in one way that matters. The command below has an effect that **depends on the
/// state it finds**, which is the only way to prove that two peers resolve a contested command
/// identically — and that the losing peer's refusal is itself part of the state, so a rejection
/// cannot pass for the command never having been sent.
///
/// Deliberately meaningless, like every other synthetic fixture here. Cells have owners and a
/// contested count; neither carries any domain meaning, and the engine has no game in it.

#include <atlas/simulation/command.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/rng.hpp>
#include <atlas/simulation/schedule.hpp>
#include <atlas/simulation/table.hpp>
#include <atlas/simulation/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace atlas::net::testing {

/// Who owns each cell, and how many claims each cell has refused.
class CellTable final : public sim::Table {
  public:
    static constexpr std::size_t kMaxRows = 1'000'000;

    std::vector<std::uint32_t> owner;
    /// **A refusal with a visible effect**, which is the point of this table. A rejection that
    /// was a silent no-op would be indistinguishable from the command never having been sent,
    /// and a test asserting "both peers refused identically" would pass on two peers that never
    /// received it.
    std::vector<std::uint32_t> contested;
    /// **Who** was refused most recently, not merely that somebody was.
    ///
    /// Without this the fixture saturates: once every cell is owned, every later claim is a
    /// refusal, and a refusal that recorded only a count would make the claimant's identity
    /// invisible. A corrupted claim would then be refused exactly as an intact one is, and a
    /// fault-injection test would pass while injecting nothing observable — which is what
    /// happened before this column existed.
    std::vector<std::uint32_t> last_refused;

    void resize(std::size_t rows) {
        owner.assign(rows, 0);
        contested.assign(rows, 0);
        last_refused.assign(rows, 0);
    }

    [[nodiscard]] std::size_t row_count() const noexcept override { return owner.size(); }

    void hash_into(Hasher& hasher) const override {
        hasher.add(static_cast<std::uint64_t>(owner.size()));
        for (std::size_t i = 0; i < owner.size(); ++i) {
            hasher.add(owner[i]);
            hasher.add(contested[i]);
            hasher.add(last_refused[i]);
        }
    }

    void write_to(sim::SaveWriter& writer) const override {
        writer.write_u64(static_cast<std::uint64_t>(owner.size()));
        for (std::size_t i = 0; i < owner.size(); ++i) {
            writer.write_u32(owner[i]);
            writer.write_u32(contested[i]);
            writer.write_u32(last_refused[i]);
        }
    }

    [[nodiscard]] Status read_from(sim::SaveReader& reader) override {
        auto rows = reader.read_count(kMaxRows, 12);
        if (!rows) {
            return std::unexpected(std::move(rows).error());
        }
        std::vector<std::uint32_t> loaded_owner;
        std::vector<std::uint32_t> loaded_contested;
        std::vector<std::uint32_t> loaded_refused;
        loaded_owner.reserve(*rows);
        loaded_contested.reserve(*rows);
        loaded_refused.reserve(*rows);
        for (std::size_t i = 0; i < *rows; ++i) {
            auto o = reader.read_u32();
            if (!o) {
                return std::unexpected(std::move(o).error());
            }
            auto c = reader.read_u32();
            if (!c) {
                return std::unexpected(std::move(c).error());
            }
            auto r = reader.read_u32();
            if (!r) {
                return std::unexpected(std::move(r).error());
            }
            loaded_owner.push_back(*o);
            loaded_contested.push_back(*c);
            loaded_refused.push_back(*r);
        }
        owner = std::move(loaded_owner);
        contested = std::move(loaded_contested);
        last_refused = std::move(loaded_refused);
        return ok();
    }

    void clear() override {
        owner.clear();
        contested.clear();
        last_refused.clear();
    }
};

/// A reduction over the cells, so that a system has something to write and a per-system hash
/// has something to attribute.
class TallyTable final : public sim::Table {
  public:
    std::uint64_t owned = 0;
    std::uint64_t refusals = 0;

    [[nodiscard]] std::size_t row_count() const noexcept override { return 1; }

    void hash_into(Hasher& hasher) const override {
        hasher.add(owned);
        hasher.add(refusals);
    }

    void write_to(sim::SaveWriter& writer) const override {
        writer.write_u64(owned);
        writer.write_u64(refusals);
    }

    [[nodiscard]] Status read_from(sim::SaveReader& reader) override {
        auto a = reader.read_u64();
        if (!a) {
            return std::unexpected(std::move(a).error());
        }
        auto b = reader.read_u64();
        if (!b) {
            return std::unexpected(std::move(b).error());
        }
        owned = *a;
        refusals = *b;
        return ok();
    }

    void clear() override {
        owned = 0;
        refusals = 0;
    }
};

inline constexpr std::size_t kCells = 32;
inline const sim::CommandType kClaimCell = sim::command_type("claim cell");

/// `cell` then `claimant`, four bytes each.
[[nodiscard]] inline std::vector<std::byte> claim_payload(std::uint32_t cell,
                                                          std::uint32_t claimant) {
    std::vector<std::byte> payload(8);
    for (std::size_t i = 0; i < 4; ++i) {
        payload[i] = static_cast<std::byte>((cell >> (i * 8)) & 0xFFU);
        payload[i + 4] = static_cast<std::byte>((claimant >> (i * 8)) & 0xFFU);
    }
    return payload;
}

/// Claim a cell, or be refused by whoever already owns it.
///
/// **The refusal is state-dependent and is itself recorded.** Two peers applying the same
/// commands in the same order — which is what `(source, sequence)` guarantees — must agree on
/// which claim won and which was refused, and both outcomes change the hash. That is the whole
/// of what this fixture exists to make checkable.
[[nodiscard]] inline sim::CommandHandler claim_handler(sim::TableId cells) {
    sim::CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> Status {
        if (payload.size() != 8) {
            return std::unexpected(
                Error(ErrorCode::MalformedData, "expected a cell and a claimant"));
        }
        return ok();
    };
    handler.apply = [cells](sim::World& world, std::span<const std::byte> payload) {
        auto* table = dynamic_cast<CellTable*>(world.table(cells));
        if (table == nullptr) {
            return;
        }
        std::uint32_t cell = 0;
        std::uint32_t claimant = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            cell |=
                static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(payload[i]) << (i * 8));
            claimant |= static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(payload[i + 4])
                                                   << (i * 8));
        }
        if (cell >= table->owner.size()) {
            return;
        }
        if (table->owner[cell] == 0) {
            table->owner[cell] = claimant;
        } else {
            ++table->contested[cell];
            table->last_refused[cell] = claimant;
        }
    };
    return handler;
}

/// Sums the cells into the tally, so a tick changes state even when no command arrives.
///
/// Shaped the way every system here is: compute reads the world and fills storage the system
/// owns, commit writes that storage back. The scratch lives in a shared pointer so the two
/// closures share it and the description can be passed by value.
[[nodiscard]] inline sim::SystemDesc tally_system(sim::TableId cells, sim::TableId tally) {
    auto scratch = std::make_shared<std::pair<std::uint64_t, std::uint64_t>>(0, 0);

    sim::SystemDesc desc;
    desc.name = "tally cells";
    desc.reads = {cells};
    desc.writes = {tally};
    desc.compute = [scratch, cells](const sim::ComputeContext& context) {
        const auto* table = dynamic_cast<const CellTable*>(context.world.table(cells));
        std::uint64_t owned = 0;
        std::uint64_t refusals = 0;
        if (table != nullptr) {
            for (std::size_t i = 0; i < table->owner.size(); ++i) {
                owned += table->owner[i];
                refusals += table->contested[i] + table->last_refused[i];
            }
        }
        *scratch = {owned, refusals};
    };
    desc.commit = [scratch, tally](const sim::CommitContext& context) {
        auto* table = dynamic_cast<TallyTable*>(context.world.table(tally));
        if (table != nullptr) {
            table->owned = scratch->first;
            table->refusals = scratch->second;
        }
    };
    return desc;
}

/// A second system, so a per-system hash has more than one candidate to attribute a difference
/// to. It draws from the tick's own random stream, which is a pure function of the seed and the
/// tick and therefore identical on every peer.
[[nodiscard]] inline sim::SystemDesc stir_system(sim::TableId tally) {
    auto scratch = std::make_shared<std::uint64_t>(0);

    sim::SystemDesc desc;
    desc.name = "stir the tally";
    desc.reads = {};
    desc.writes = {tally};
    desc.compute = [scratch](const sim::ComputeContext& context) {
        auto stream = context.rng.stream("stir");
        *scratch = stream.next_below(16);
    };
    desc.commit = [scratch, tally](const sim::CommitContext& context) {
        auto* table = dynamic_cast<TallyTable*>(context.world.table(tally));
        if (table != nullptr) {
            table->refusals += *scratch;
        }
    };
    return desc;
}

/// One peer's simulation: a world, a schedule, a queue and a kernel.
struct Peer {
    sim::World world;
    sim::Schedule schedule;
    sim::CommandQueue commands;
    sim::TableId cells = sim::TableId::Invalid;
    sim::TableId tally = sim::TableId::Invalid;

    Peer() {
        auto cell_table = std::make_unique<CellTable>();
        cell_table->resize(kCells);
        auto registered = world.add_table("cells", std::move(cell_table));
        REQUIRE(registered.has_value());
        cells = *registered;

        auto tally_registered = world.add_table("tally", std::make_unique<TallyTable>());
        REQUIRE(tally_registered.has_value());
        tally = *tally_registered;

        REQUIRE(commands.register_handler(kClaimCell, claim_handler(cells)).has_value());
        REQUIRE(schedule.add(tally_system(cells, tally)).has_value());
        REQUIRE(schedule.add(stir_system(tally)).has_value());
        REQUIRE(schedule.finalise(world).has_value());
    }
};

}  // namespace atlas::net::testing
