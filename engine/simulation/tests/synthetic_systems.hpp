// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Synthetic systems for testing the kernel.
///
/// They do arithmetic on meaningless integers. There is no domain here and there must not
/// be: the engine has no game in it and neither do its tests.

#include <atlas/simulation/kernel.hpp>

#include "synthetic_tables.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace atlas::sim::testing {

/// A kernel wired up over two synthetic tables.
struct Harness {
    World world;
    Schedule schedule;
    CommandQueue commands;
    TableId values = TableId::Invalid;
    TableId counter = TableId::Invalid;

    explicit Harness(std::size_t rows = 8) {
        auto table = std::make_unique<ValueTable>();
        table->resize(rows);
        for (std::size_t i = 0; i < rows; ++i) {
            table->value[i] = static_cast<std::int32_t>(i);
            table->owner_index[i] = static_cast<std::uint16_t>(i % 4);
        }
        values = world.add_table("values", std::move(table)).value();
        counter = world.add_table("counter", std::make_unique<CounterTable>()).value();
    }

    [[nodiscard]] ValueTable& value_table() {
        return *dynamic_cast<ValueTable*>(world.table(values));
    }

    [[nodiscard]] CounterTable& counter_table() {
        return *dynamic_cast<CounterTable*>(world.table(counter));
    }
};

/// Adds one to every value, through a scratch buffer it owns.
///
/// Shaped the way a real system is: compute reads the world and fills private storage,
/// commit writes that storage back. The scratch lives in a shared_ptr so the two closures
/// share it and the system can be added by value.
[[nodiscard]] inline SystemDesc increment_values(TableId values) {
    auto scratch = std::make_shared<std::vector<std::int32_t>>();

    SystemDesc desc;
    desc.name = "increment values";
    desc.reads = {};
    desc.writes = {values};
    desc.compute = [scratch, values](const ComputeContext& context) {
        const auto* table = dynamic_cast<const ValueTable*>(context.world.table(values));
        scratch->assign(table->value.begin(), table->value.end());
        for (std::int32_t& value : *scratch) {
            value += 1;
        }
    };
    desc.commit = [scratch, values](const CommitContext& context) {
        auto* table = dynamic_cast<ValueTable*>(context.world.table(values));
        table->value = *scratch;
    };
    return desc;
}

/// Sums the values into the counter, reading one table and writing another.
[[nodiscard]] inline SystemDesc sum_into_counter(TableId values, TableId counter) {
    auto scratch = std::make_shared<std::uint64_t>(0);

    SystemDesc desc;
    desc.name = "sum into counter";
    desc.reads = {values};
    desc.writes = {counter};
    desc.compute = [scratch, values](const ComputeContext& context) {
        const auto* table = dynamic_cast<const ValueTable*>(context.world.table(values));
        std::uint64_t total = 0;
        for (const std::int32_t value : table->value) {
            total += static_cast<std::uint64_t>(value);
        }
        *scratch = total;
    };
    desc.commit = [scratch, counter](const CommitContext& context) {
        auto* table = dynamic_cast<CounterTable*>(context.world.table(counter));
        table->count = *scratch;
    };
    return desc;
}

/// Adds a random amount to the counter, so a test can tell whether the stream is reproducible.
[[nodiscard]] inline SystemDesc random_into_counter(TableId counter,
                                                    std::string_view stream_name = "noise") {
    auto scratch = std::make_shared<std::uint64_t>(0);
    const auto stream = stream_id(stream_name);

    SystemDesc desc;
    desc.name = "random into counter";
    desc.writes = {counter};
    desc.compute = [scratch, stream](const ComputeContext& context) {
        auto rng = context.rng.stream(stream);
        *scratch = rng.next_below(1000);
    };
    desc.commit = [scratch, counter](const CommitContext& context) {
        auto* table = dynamic_cast<CounterTable*>(context.world.table(counter));
        table->count += *scratch;
    };
    return desc;
}

}  // namespace atlas::sim::testing
