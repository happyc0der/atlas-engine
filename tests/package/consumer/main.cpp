// SPDX-License-Identifier: GPL-3.0-or-later
//
// A program that is not Atlas, linked against an installed Atlas (ADR-0024 D10).
//
// Each check uses one thing a game would, through the installed headers and libraries only:
//   - the engine's golden scenario, run from this program's own tables and systems and compared
//     with the constants Atlas installs, so a package that compiled differently from the tree
//     that measured them fails here;
//   - a script runtime brought up and shut down;
//   - a message across a loopback link;
//   - the engine's string table, read from Atlas_DATA_DIR and resolving a key the overlay uses;
//   - the app kit.
// Prints one line per check and exits nonzero on the first that fails.

#include <atlas/app/run_bounds.hpp>
#include <atlas/assets/importer.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/build_info.hpp>
#include <atlas/core/hash.hpp>
#include <atlas/net/inbox.hpp>
#include <atlas/net/loopback.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/golden.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/table.hpp>
#include <atlas/text/catalog.hpp>
#include <atlas/tools/text_keys.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSVC_LANG)
// Printed rather than asserted: which value /std:c++latest reports moves between compiler
// releases. run.py compares the standard flag itself with the one Atlas was compiled with.
constexpr long kLanguage = _MSVC_LANG;
#else
constexpr long kLanguage = __cplusplus;
#endif

namespace {

using atlas::sim::CommitContext;
using atlas::sim::ComputeContext;
using atlas::sim::SystemDesc;
using atlas::sim::TableId;

// The golden scenario's two tables, written again here as a game would write its own. Every
// byte they hash must match the engine's test tables, names included, or the golden hashes move.
class ValueTable final : public atlas::sim::Table {
  public:
    std::vector<std::int32_t> value;
    std::vector<std::uint16_t> owner_index;

    [[nodiscard]] std::size_t row_count() const noexcept override { return value.size(); }

    void hash_into(atlas::Hasher& hasher) const override {
        hasher.add(static_cast<std::uint64_t>(value.size()));
        for (std::size_t i = 0; i < value.size(); ++i) {
            hasher.add(value[i]);
            hasher.add(owner_index[i]);
        }
    }

    void write_to(atlas::sim::SaveWriter& writer) const override {
        writer.write_u64(static_cast<std::uint64_t>(value.size()));
        for (std::size_t i = 0; i < value.size(); ++i) {
            writer.write_i32(value[i]);
            writer.write_u16(owner_index[i]);
        }
    }

    [[nodiscard]] atlas::Status read_from(atlas::sim::SaveReader& /*reader*/) override {
        return atlas::fail(atlas::ErrorCode::InvalidArgument, "the consumer does not load saves");
    }

    void clear() override {
        value.clear();
        owner_index.clear();
    }
};

class CounterTable final : public atlas::sim::Table {
  public:
    std::uint64_t count = 0;

    [[nodiscard]] std::size_t row_count() const noexcept override { return 1; }

    void hash_into(atlas::Hasher& hasher) const override { hasher.add(count); }

    void write_to(atlas::sim::SaveWriter& writer) const override { writer.write_u64(count); }

    [[nodiscard]] atlas::Status read_from(atlas::sim::SaveReader& /*reader*/) override {
        return atlas::fail(atlas::ErrorCode::InvalidArgument, "the consumer does not load saves");
    }

    void clear() override { count = 0; }
};

[[nodiscard]] SystemDesc increment_values(TableId values) {
    const auto scratch = std::make_shared<std::vector<std::int32_t>>();
    SystemDesc desc;
    desc.name = "increment values";
    desc.writes = {values};
    desc.compute = [scratch, values](const ComputeContext& context) {
        const auto* table = dynamic_cast<const ValueTable*>(context.world.table(values));
        scratch->assign(table->value.begin(), table->value.end());
        for (std::int32_t& value : *scratch) {
            value += 1;
        }
    };
    desc.commit = [scratch, values](const CommitContext& context) {
        dynamic_cast<ValueTable*>(context.world.table(values))->value = *scratch;
    };
    return desc;
}

[[nodiscard]] SystemDesc sum_into_counter(TableId values, TableId counter) {
    const auto scratch = std::make_shared<std::uint64_t>(0);
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
        dynamic_cast<CounterTable*>(context.world.table(counter))->count = *scratch;
    };
    return desc;
}

[[nodiscard]] SystemDesc random_into_counter(TableId counter) {
    const auto scratch = std::make_shared<std::uint64_t>(0);
    SystemDesc desc;
    desc.name = "random into counter";
    desc.writes = {counter};
    desc.compute = [scratch](const ComputeContext& context) {
        auto rng = context.rng.stream(atlas::sim::stream_id("noise"));
        *scratch = rng.next_below(1000);
    };
    desc.commit = [scratch, counter](const CommitContext& context) {
        dynamic_cast<CounterTable*>(context.world.table(counter))->count += *scratch;
    };
    return desc;
}

[[nodiscard]] bool check(bool passed, std::string_view what, const std::string& detail = {}) {
    std::printf("%s: %.*s%s%s\n", passed ? "ok" : "FAILED", static_cast<int>(what.size()),
                what.data(), detail.empty() ? "" : " — ", detail.c_str());
    return passed;
}

[[nodiscard]] bool golden_scenario() {
    atlas::sim::World world;
    atlas::sim::Schedule schedule;
    atlas::sim::CommandQueue commands;

    auto table = std::make_unique<ValueTable>();
    table->value.resize(atlas::sim::kGoldenRows);
    table->owner_index.resize(atlas::sim::kGoldenRows);
    for (std::size_t i = 0; i < atlas::sim::kGoldenRows; ++i) {
        table->value[i] = static_cast<std::int32_t>(i);
        table->owner_index[i] = static_cast<std::uint16_t>(i % 4);
    }
    const auto values = world.add_table("values", std::move(table));
    const auto counter = world.add_table("counter", std::make_unique<CounterTable>());
    if (!values || !counter) {
        return check(false, "golden scenario", "the tables were refused");
    }
    if (!schedule.add(increment_values(*values)) ||
        !schedule.add(sum_into_counter(*values, *counter)) ||
        !schedule.add(random_into_counter(*counter)) || !schedule.finalise(world)) {
        return check(false, "golden scenario", "the schedule was refused");
    }

    atlas::sim::Kernel kernel(world, schedule, commands,
                              atlas::sim::KernelConfig{.seed = atlas::sim::kGoldenSeed});
    const auto reports = kernel.run(atlas::sim::kGoldenTicks);
    if (!reports) {
        return check(false, "golden scenario", reports.error().message());
    }
    atlas::Hasher over_time;
    for (const auto& report : *reports) {
        over_time.add(report.state_hash);
    }

    std::uint64_t expected = atlas::sim::kGoldenFinalState;
#if defined(ATLAS_CONSUMER_WRONG_GOLDEN)
    expected ^= 1U;  // The mutation that proves this check can fail.
#endif
    char detail[96];
    std::snprintf(detail, sizeof detail, "final_state=%#018llx all_ticks=%#018llx",
                  static_cast<unsigned long long>(reports->back().state_hash),
                  static_cast<unsigned long long>(over_time.value()));
    return check(reports->back().state_hash == expected &&
                     over_time.value() == atlas::sim::kGoldenAllTicks,
                 "golden scenario", detail);
}

[[nodiscard]] bool script_runtime() {
    auto runtime = atlas::script::Runtime::create();
    return check(runtime.has_value(), "script runtime",
                 runtime ? std::string{} : runtime.error().message());
}

[[nodiscard]] bool loopback_link() {
    auto hub = atlas::net::LoopbackHub::create(atlas::net::LoopbackConfig{.peer_count = 2});
    if (!hub) {
        return check(false, "loopback link", hub.error().message());
    }
    const std::vector<std::byte> message{std::byte{0x41}, std::byte{0x74}, std::byte{0x6C}};
    if (auto sent = (*hub)->send(0, 1, message); !sent) {
        return check(false, "loopback link", sent.error().message());
    }
    (*hub)->pump(1);
    std::vector<std::vector<std::byte>> received;
    (*hub)->inbox(1, 0).drain(received);
    return check(received.size() == 1 && received.front() == message, "loopback link");
}

[[nodiscard]] bool string_table() {
    const std::string path = std::string{ATLAS_CONSUMER_DATA_DIR} + "/strings/en.json";
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> text{std::istreambuf_iterator<char>(file),
                                 std::istreambuf_iterator<char>()};
    if (!file.good() && !file.eof()) {
        return check(false, "string table", "cannot read " + path);
    }
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(text[i]);
    }
    auto table = atlas::assets::import_string_table(bytes, path);
    if (!table) {
        return check(false, "string table", table.error().message());
    }
    atlas::text::Catalog catalog;
    if (auto loaded = catalog.load(*table); !loaded) {
        return check(false, "string table", loaded.error().message());
    }
    // A missing key resolves to itself (ADR-0016), so resolving means coming back different.
    const std::string_view resolved = catalog.lookup(atlas::tools::keys::kUndo);
    return check(resolved != atlas::tools::keys::kUndo, "string table",
                 std::string{atlas::tools::keys::kUndo} + " -> " + std::string{resolved});
}

[[nodiscard]] bool app_kit() {
    const bool unbounded_refused = !atlas::app::validate_headless_bound(true, 0, 0);
    const bool bounded_accepted = atlas::app::validate_headless_bound(true, 0, 10).has_value();
    return check(unbounded_refused && bounded_accepted, "app kit");
}

}  // namespace

int main() {
    atlas::mark_main_thread();
    const std::string summary{atlas::build_info::summary()};
    std::printf("consumer of %s, language %ld\n", summary.c_str(), kLanguage);

    const bool passed =
        golden_scenario() && script_runtime() && loopback_link() && string_table() && app_kit();
    std::printf(passed ? "consumer: every check passed\n" : "consumer: a check failed\n");
    return passed ? 0 : 1;
}
