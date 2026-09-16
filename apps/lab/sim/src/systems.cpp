// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/systems.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace atlas::lab {

sim::SystemDesc step_region_value(const TableIds& ids) {
    const auto scratch = std::make_shared<std::vector<std::uint32_t>>();
    sim::SystemDesc desc;
    desc.name = "step region value";
    desc.writes = {ids.cells};
    desc.compute = [scratch, ids](const sim::ComputeContext& context) {
        const auto& cells = cell_table(context.world, ids);
        scratch->assign(cells.region_value.begin(), cells.region_value.end());
        for (std::uint32_t& value : *scratch) {
            // Wrapping unsigned arithmetic: defined, and the high bits feed back so the
            // sequence does not settle into a short cycle.
            value += 1U + (value >> 27U);
        }
    };
    desc.commit = [scratch, ids](const sim::CommitContext& context) {
        cell_table(context.world, ids).region_value = *scratch;
    };
    return desc;
}

sim::SystemDesc drift_owner_index(const TableIds& ids) {
    struct Drift {
        std::uint32_t cell;
        std::uint16_t owner;
    };

    const auto scratch = std::make_shared<std::vector<Drift>>();
    constexpr auto kStream = sim::stream_id("owner_drift");
    sim::SystemDesc desc;
    desc.name = "drift owner index";
    desc.reads = {ids.population};
    desc.writes = {ids.cells};
    desc.compute = [scratch, ids](const sim::ComputeContext& context) {
        const auto& cells = cell_table(context.world, ids);
        const auto& population = population_table(context.world, ids);
        const auto count = static_cast<std::uint32_t>(cells.row_count());
        scratch->clear();
        if (count == 0) {
            return;
        }
        auto rng = context.rng.stream(kStream);
        const std::uint32_t picks = std::max<std::uint32_t>(1, count / kDriftDivisor);
        for (std::uint32_t i = 0; i < picks; ++i) {
            const auto cell = static_cast<std::uint32_t>(rng.next_below(count));
            // The cross-table read: a populous cell drifts one step, a sparse one two.
            const std::uint16_t step =
                population.population_value[cell] > kPopulationCap / 2 ? 1 : 2;
            scratch->push_back(Drift{
                .cell = cell,
                .owner = static_cast<std::uint16_t>((cells.owner_index[cell] + step) % kOwnerCount),
            });
        }
    };
    desc.commit = [scratch, ids](const sim::CommitContext& context) {
        auto& cells = cell_table(context.world, ids);
        for (const Drift& drift : *scratch) {
            cells.owner_index[drift.cell] = drift.owner;
        }
    };
    return desc;
}

sim::SystemDesc accumulate_population(const TableIds& ids) {
    const auto scratch = std::make_shared<std::vector<std::uint32_t>>();
    sim::SystemDesc desc;
    desc.name = "accumulate population";
    desc.reads = {ids.cells, ids.adjacency};
    desc.writes = {ids.population};
    desc.compute = [scratch, ids](const sim::ComputeContext& context) {
        const auto& cells = cell_table(context.world, ids);
        const auto& adjacency = adjacency_table(context.world, ids);
        const auto& population = population_table(context.world, ids);
        const auto count = static_cast<std::uint32_t>(population.row_count());
        scratch->resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t gain = 0;
            for (const std::uint32_t n : adjacency.neighbours_of(i)) {
                gain += cells.color_index[n];
            }
            // Saturating at a documented cap, so the column has a bounded range the snapshot
            // can scale into a byte without knowing the tick count.
            (*scratch)[i] = std::min(kPopulationCap, population.population_value[i] + gain);
        }
    };
    desc.commit = [scratch, ids](const sim::CommitContext& context) {
        population_table(context.world, ids).population_value = *scratch;
    };
    return desc;
}

sim::SystemDesc shift_chunk_owner(const TableIds& ids) {
    struct Shift {
        std::uint32_t chunk = 0;
        std::uint16_t owner = 0;
        bool any = false;
    };

    const auto scratch = std::make_shared<Shift>();
    constexpr auto kStream = sim::stream_id("chunk_shift");
    sim::SystemDesc desc;
    desc.name = "shift chunk owner";
    desc.writes = {ids.chunks};
    desc.compute = [scratch, ids](const sim::ComputeContext& context) {
        const auto& chunks = chunk_table(context.world, ids);
        const auto count = static_cast<std::uint32_t>(chunks.row_count());
        scratch->any = count > 0;
        if (!scratch->any) {
            return;
        }
        auto rng = context.rng.stream(kStream);
        scratch->chunk = static_cast<std::uint32_t>(rng.next_below(count));
        scratch->owner = static_cast<std::uint16_t>(rng.next_below(kOwnerCount));
    };
    desc.commit = [scratch, ids](const sim::CommitContext& context) {
        if (scratch->any) {
            chunk_table(context.world, ids).owner_index[scratch->chunk] = scratch->owner;
        }
    };
    return desc;
}

Status add_lab_systems(sim::Schedule& schedule, const TableIds& ids) {
    if (auto s = schedule.add(step_region_value(ids)); !s) {
        return s;
    }
    if (auto s = schedule.add(drift_owner_index(ids)); !s) {
        return s;
    }
    if (auto s = schedule.add(accumulate_population(ids)); !s) {
        return s;
    }
    return schedule.add(shift_chunk_owner(ids));
}

}  // namespace atlas::lab
