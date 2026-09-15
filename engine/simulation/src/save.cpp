// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/simulation/save.hpp>

#include <algorithm>
#include <format>

namespace atlas::sim {
namespace {

constexpr log::Category kSim{"sim"};

/// Most tables a file may claim. A world with more than this is not a world.
constexpr std::size_t kMaxTables = 4096;

/// Most command sources a file may claim.
constexpr std::size_t kMaxSources = 65536;

/// Smallest a table's section can be: its identifier and its byte length.
constexpr std::size_t kTableSectionOverhead = 4 + 8;

[[nodiscard]] Result<SaveHeader> read_header_from(SaveReader& reader) {
    auto magic = reader.read_u64();
    if (!magic) {
        return std::unexpected(std::move(magic).error().context("reading the save magic"));
    }
    if (*magic != kSaveMagic) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  "this is not an Atlas save: the leading magic value does not match"));
    }

    SaveHeader header;

    auto format = reader.read_u32();
    if (!format) {
        return std::unexpected(std::move(format).error().context("reading the format version"));
    }
    header.format_version = *format;

    if (header.format_version > kSaveFormatVersion) {
        return std::unexpected(Error(
            ErrorCode::VersionMismatch,
            std::format("this save is version {}, and this build understands up to {}. Reading "
                        "it would silently drop whatever the newer version added.",
                        header.format_version, kSaveFormatVersion)));
    }
    if (header.format_version < kSaveFormatVersion) {
        // There is no version 1 to migrate from yet, so there is no migration code. Writing
        // it now would be writing untested code for an imagined problem; the version check
        // is what makes deferring it safe.
        return std::unexpected(
            Error(ErrorCode::VersionMismatch,
                  std::format("this save is version {}, and this build reads only version {}. No "
                              "migration exists yet.",
                              header.format_version, kSaveFormatVersion)));
    }

    auto algorithm = reader.read_u32();
    if (!algorithm) {
        return std::unexpected(std::move(algorithm).error().context("reading the hash version"));
    }
    header.hash_algorithm_version = *algorithm;

    auto tick = reader.read_u64();
    if (!tick) {
        return std::unexpected(std::move(tick).error().context("reading the tick"));
    }
    header.tick = *tick;

    auto seed = reader.read_u64();
    if (!seed) {
        return std::unexpected(std::move(seed).error().context("reading the seed"));
    }
    header.seed = *seed;

    auto hash = reader.read_u64();
    if (!hash) {
        return std::unexpected(std::move(hash).error().context("reading the state hash"));
    }
    header.state_hash = *hash;

    return header;
}

}  // namespace

Result<std::vector<std::byte>> save(const World& world, const Kernel& kernel,
                                    const CommandQueue& commands) {
    ATLAS_ZONE_NAMED("sim::save");

    SaveWriter writer;

    writer.write_u64(kSaveMagic);
    writer.write_u32(kSaveFormatVersion);
    writer.write_u32(kHashAlgorithmVersion);
    writer.write_u64(kernel.current_tick());
    writer.write_u64(kernel.seed());
    writer.write_u64(world.hash());

    // Tables in identifier order, which is the order the world keeps them in, so a save is
    // canonical without sorting anything here.
    const auto ids = world.ids();
    writer.write_u64(static_cast<std::uint64_t>(ids.size()));

    for (const TableId id : ids) {
        const Table* table = world.table(id);
        if (table == nullptr) {
            return std::unexpected(
                Error(ErrorCode::Internal, std::format("table {} is listed but missing",
                                                       static_cast<std::uint32_t>(id))));
        }

        writer.write_u32(static_cast<std::uint32_t>(id));

        // The table's bytes are written to a separate buffer so their length can precede
        // them. Without the length a reader cannot skip a table it does not recognise, and
        // cannot tell a table that read too little from one that read too much.
        SaveWriter table_writer;
        table->write_to(table_writer);

        writer.write_u64(static_cast<std::uint64_t>(table_writer.size()));
        writer.write_bytes(table_writer.bytes());
    }

    // Command sequence numbers, so a command submitted after a load cannot reuse a number
    // that already appeared before it.
    // In identifier order, which is what the queue hands back, so the bytes do not depend on
    // the order the sources happened to first issue a command.
    const std::vector<SourceId> sources = commands.sources();
    writer.write_u64(static_cast<std::uint64_t>(sources.size()));
    for (const SourceId source : sources) {
        writer.write_u32(static_cast<std::uint32_t>(source));
        writer.write_u64(commands.next_sequence(source));
    }

    return writer.take();
}

Result<SaveHeader> read_header(std::span<const std::byte> bytes) {
    SaveReader reader(bytes);
    return read_header_from(reader);
}

Status load(World& world, Kernel& kernel, CommandQueue& commands,
            std::span<const std::byte> bytes) {
    ATLAS_ZONE_NAMED("sim::load");

    SaveReader reader(bytes);

    auto header = read_header_from(reader);
    if (!header) {
        return std::unexpected(std::move(header).error());
    }

    if (header->hash_algorithm_version != kHashAlgorithmVersion) {
        return std::unexpected(
            Error(ErrorCode::VersionMismatch,
                  std::format("this save records hashes from algorithm version {}, and this build "
                              "uses version {}; the stored hash cannot be compared",
                              header->hash_algorithm_version, kHashAlgorithmVersion)));
    }

    auto table_count = reader.read_count(kMaxTables, kTableSectionOverhead);
    if (!table_count) {
        return std::unexpected(std::move(table_count).error().context("reading the table count"));
    }

    if (*table_count != world.table_count()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("this save holds {} table(s) and this build has {}; the save and the "
                              "build disagree about what the state is",
                              *table_count, world.table_count())));
    }

    // Every table's bytes are located and checked first, and nothing is written into the
    // world until all of them have been accepted. A half-loaded world is a state no author
    // reasoned about.
    struct Section {
        TableId id = TableId::Invalid;
        std::span<const std::byte> bytes;
    };

    std::vector<Section> sections;
    sections.reserve(*table_count);

    for (std::size_t i = 0; i < *table_count; ++i) {
        auto raw_id = reader.read_u32();
        if (!raw_id) {
            return std::unexpected(std::move(raw_id).error().context("reading a table identifier"));
        }
        const auto id = TableId{*raw_id};

        if (!world.contains(id)) {
            return std::unexpected(
                Error(ErrorCode::NotFound,
                      std::format("this save holds a table with identifier {} that this build does "
                                  "not have",
                                  *raw_id)));
        }
        if (std::ranges::any_of(sections, [id](const Section& s) { return s.id == id; })) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("table {} appears twice in this save", *raw_id)));
        }

        // Bounds-checked against what remains, so a length claiming more than the file holds
        // is refused before anything is read with it.
        auto length = reader.read_count(bytes.size(), 1);
        if (!length) {
            return std::unexpected(std::move(length).error().context("reading a table length"));
        }
        auto section = reader.read_bytes(*length);
        if (!section) {
            return std::unexpected(std::move(section).error().context("reading a table"));
        }

        sections.push_back(Section{.id = id, .bytes = *section});
    }

    auto source_count = reader.read_count(kMaxSources, 12);
    if (!source_count) {
        return std::unexpected(std::move(source_count).error().context("reading the source count"));
    }

    std::vector<std::pair<SourceId, std::uint64_t>> sequences;
    sequences.reserve(*source_count);
    for (std::size_t i = 0; i < *source_count; ++i) {
        auto raw_source = reader.read_u32();
        if (!raw_source) {
            return std::unexpected(std::move(raw_source).error().context("reading a source"));
        }
        auto next = reader.read_u64();
        if (!next) {
            return std::unexpected(std::move(next).error().context("reading a sequence number"));
        }
        sequences.emplace_back(SourceId{*raw_source}, *next);
    }

    if (auto status = reader.expect_end(); !status) {
        return std::unexpected(std::move(status).error().context("finishing the save"));
    }

    // Everything has been located and checked. Now it can be applied.
    //
    // A table's own read_from may still fail on its contents, which is why the tables are
    // cleared first: a failure part way through leaves every table empty rather than some
    // holding the file's rows and the rest holding the previous state, and an empty world is
    // a state a caller can recognise and recover from.
    world.clear();

    for (const Section& section : sections) {
        Table* table = world.table(section.id);
        SaveReader table_reader(section.bytes);

        if (auto status = table->read_from(table_reader); !status) {
            world.clear();
            return std::unexpected(std::move(status).error().context(
                std::format("reading table {}", static_cast<std::uint32_t>(section.id))));
        }
        if (auto status = table_reader.expect_end(); !status) {
            world.clear();
            return std::unexpected(std::move(status).error().context(
                std::format("table {} did not consume its whole section, so the file and this "
                            "build disagree about its layout",
                            static_cast<std::uint32_t>(section.id))));
        }
    }

    // The hash is checked after the state is in place, because it is a statement about the
    // state and not about the bytes. A mismatch means the file is internally inconsistent:
    // either it was corrupted, or a table read something different from what was written.
    if (const std::uint64_t actual = world.hash(); actual != header->state_hash) {
        world.clear();
        return std::unexpected(Error(
            ErrorCode::IntegrityCheckFailed,
            std::format("this save records state hash {:#018x} and the state it loaded hashes "
                        "to {:#018x}; the file is corrupt or was written by a build whose "
                        "tables differ",
                        header->state_hash, actual)));
    }

    kernel.set_tick(header->tick);
    kernel.set_seed(header->seed);

    commands.clear();
    for (const auto& [source, next] : sequences) {
        commands.set_next_sequence(source, next);
    }

    ATLAS_LOG_INFO(kSim, "loaded a save at tick {} with {} table(s), state hash {:#018x}",
                   header->tick, sections.size(), header->state_hash);
    return ok();
}

}  // namespace atlas::sim
