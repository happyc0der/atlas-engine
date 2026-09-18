// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/simulation/replay.hpp>
#include <atlas/simulation/save_stream.hpp>

#include <algorithm>
#include <format>

namespace atlas::sim {
namespace {

constexpr log::Category kSim{"sim"};

// The bounds on what a recording may hold are declared in replay.hpp now, so that the writer
// checks the same numbers the reader enforces. They used to live here, where only the reader
// could see them, and a recording past them was written successfully and never read again.

/// Smallest a command can encode to: tick, source, sequence, type, payload length.
constexpr std::size_t kCommandOverhead = 8 + 4 + 8 + 4 + 8;

/// Smallest a checkpoint can encode to: tick, hash, system count.
constexpr std::size_t kCheckpointOverhead = 8 + 8 + 8;

void write_command(SaveWriter& writer, const Command& command) {
    writer.write_u64(command.target);
    writer.write_u32(static_cast<std::uint32_t>(command.source));
    writer.write_u64(command.sequence);
    writer.write_u32(static_cast<std::uint32_t>(command.type));
    writer.write_u64(static_cast<std::uint64_t>(command.payload.size()));
    writer.write_bytes(command.payload);
}

[[nodiscard]] Result<Command> read_command(SaveReader& reader) {
    Command command;

    auto target = reader.read_u64();
    if (!target) {
        return std::unexpected(std::move(target).error());
    }
    command.target = *target;

    auto source = reader.read_u32();
    if (!source) {
        return std::unexpected(std::move(source).error());
    }
    command.source = SourceId{*source};

    auto sequence = reader.read_u64();
    if (!sequence) {
        return std::unexpected(std::move(sequence).error());
    }
    command.sequence = *sequence;

    auto type = reader.read_u32();
    if (!type) {
        return std::unexpected(std::move(type).error());
    }
    command.type = CommandType{*type};

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

}  // namespace

Status check_writable(const Replay& replay) {
    if (replay.commands.size() > kMaxReplayCommands) {
        return std::unexpected(
            Error(ErrorCode::Exhausted,
                  std::format("this recording holds {} commands and a reader accepts at most {}",
                              replay.commands.size(), kMaxReplayCommands)));
    }
    if (replay.checkpoints.size() > kMaxReplayCheckpoints) {
        return std::unexpected(Error(
            ErrorCode::Exhausted,
            std::format("this recording holds {} checkpoints and a reader accepts at most {}. "
                        "A larger checkpoint interval records fewer.",
                        replay.checkpoints.size(), kMaxReplayCheckpoints)));
    }
    for (const HashCheckpoint& checkpoint : replay.checkpoints) {
        if (checkpoint.system_hashes.size() > kMaxReplaySystemHashes) {
            return std::unexpected(
                Error(ErrorCode::Exhausted,
                      std::format("the checkpoint at tick {} holds {} system hashes and a reader "
                                  "accepts at most {}",
                                  checkpoint.tick, checkpoint.system_hashes.size(),
                                  kMaxReplaySystemHashes)));
        }
    }
    return {};
}

Result<std::vector<std::byte>> Replay::to_bytes() const {
    // Before a byte is written, not after. The reader's limits are the writer's limits, and a
    // recording that would fail to load is refused here where the caller can still do something
    // about it.
    if (auto status = check_writable(*this); !status) {
        return std::unexpected(std::move(status).error());
    }

    SaveWriter writer;

    writer.write_u64(kReplayMagic);
    writer.write_u32(format_version);
    writer.write_u32(hash_algorithm_version);
    writer.write_u64(seed);
    writer.write_u64(first_tick);
    writer.write_u64(tick_count);
    writer.write_u64(initial_state_hash);

    writer.write_u64(static_cast<std::uint64_t>(commands.size()));
    for (const Command& command : commands) {
        write_command(writer, command);
    }

    writer.write_u64(static_cast<std::uint64_t>(checkpoints.size()));
    for (const HashCheckpoint& checkpoint : checkpoints) {
        writer.write_u64(checkpoint.tick);
        writer.write_u64(checkpoint.state_hash);
        writer.write_u64(static_cast<std::uint64_t>(checkpoint.system_hashes.size()));
        for (const SystemHash& system : checkpoint.system_hashes) {
            writer.write_u32(static_cast<std::uint32_t>(system.system));
            writer.write_u64(system.hash);
        }
    }

    return writer.take();
}

Result<Replay> Replay::from_bytes(std::span<const std::byte> bytes) {
    SaveReader reader(bytes);

    auto magic = reader.read_u64();
    if (!magic) {
        return std::unexpected(std::move(magic).error().context("reading the replay magic"));
    }
    if (*magic != kReplayMagic) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  "this is not an Atlas replay: the leading magic value does not match"));
    }

    Replay replay;

    auto format = reader.read_u32();
    if (!format) {
        return std::unexpected(std::move(format).error());
    }
    replay.format_version = *format;
    // Newer and older are refused separately, as the save format already does. One equality
    // test covering both directions told somebody with an old recording the same thing it told
    // somebody with a new one, and those two people need to do different things.
    if (replay.format_version > kReplayFormatVersion) {
        return std::unexpected(Error(
            ErrorCode::VersionMismatch,
            std::format("this replay is version {}, and this build understands up to {}. Playing "
                        "it would silently ignore whatever the newer version added.",
                        replay.format_version, kReplayFormatVersion)));
    }
    if (replay.format_version < kReplayFormatVersion) {
        return std::unexpected(
            Error(ErrorCode::VersionMismatch,
                  std::format("this replay is version {}, and this build reads only version {}. No "
                              "migration exists yet.",
                              replay.format_version, kReplayFormatVersion)));
    }

    auto algorithm = reader.read_u32();
    if (!algorithm) {
        return std::unexpected(std::move(algorithm).error());
    }
    replay.hash_algorithm_version = *algorithm;
    if (replay.hash_algorithm_version != kHashAlgorithmVersion) {
        // The recorded hashes were produced by a different algorithm, so comparing them
        // would report a divergence that is only a change of hash function.
        return std::unexpected(
            Error(ErrorCode::VersionMismatch,
                  std::format("this replay records hashes from algorithm version {} and this build "
                              "uses version {}; its hashes cannot be compared",
                              replay.hash_algorithm_version, kHashAlgorithmVersion)));
    }

    for (auto* field :
         {&replay.seed, &replay.first_tick, &replay.tick_count, &replay.initial_state_hash}) {
        auto value = reader.read_u64();
        if (!value) {
            return std::unexpected(std::move(value).error().context("reading the replay header"));
        }
        *field = *value;
    }

    auto command_count = reader.read_count(kMaxReplayCommands, kCommandOverhead);
    if (!command_count) {
        return std::unexpected(std::move(command_count).error().context("the command count"));
    }
    replay.commands.reserve(*command_count);
    for (std::size_t i = 0; i < *command_count; ++i) {
        auto command = read_command(reader);
        if (!command) {
            return std::unexpected(
                std::move(command).error().context(std::format("reading command {}", i)));
        }
        replay.commands.push_back(std::move(*command));
    }

    auto checkpoint_count = reader.read_count(kMaxReplayCheckpoints, kCheckpointOverhead);
    if (!checkpoint_count) {
        return std::unexpected(std::move(checkpoint_count).error().context("the checkpoint count"));
    }
    replay.checkpoints.reserve(*checkpoint_count);
    for (std::size_t i = 0; i < *checkpoint_count; ++i) {
        HashCheckpoint checkpoint;

        auto tick = reader.read_u64();
        if (!tick) {
            return std::unexpected(std::move(tick).error());
        }
        checkpoint.tick = *tick;

        auto hash = reader.read_u64();
        if (!hash) {
            return std::unexpected(std::move(hash).error());
        }
        checkpoint.state_hash = *hash;

        auto system_count = reader.read_count(kMaxReplaySystemHashes, 12);
        if (!system_count) {
            return std::unexpected(std::move(system_count).error().context("a system hash count"));
        }
        checkpoint.system_hashes.reserve(*system_count);
        for (std::size_t j = 0; j < *system_count; ++j) {
            auto system = reader.read_u32();
            if (!system) {
                return std::unexpected(std::move(system).error());
            }
            auto system_hash = reader.read_u64();
            if (!system_hash) {
                return std::unexpected(std::move(system_hash).error());
            }
            checkpoint.system_hashes.push_back(
                SystemHash{.system = SystemId{*system}, .hash = *system_hash});
        }

        replay.checkpoints.push_back(std::move(checkpoint));
    }

    if (auto status = reader.expect_end(); !status) {
        return std::unexpected(std::move(status).error().context("finishing the replay"));
    }

    return replay;
}

ReplayRecorder::ReplayRecorder(std::uint64_t seed, Tick first_tick,
                               std::uint64_t initial_state_hash, std::uint64_t checkpoint_interval,
                               ReplayLimits limits)
    : m_interval(checkpoint_interval == 0 ? 1 : checkpoint_interval), m_limits(limits) {
    m_replay.seed = seed;
    m_replay.first_tick = first_tick;
    m_replay.initial_state_hash = initial_state_hash;
}

Status ReplayRecorder::record_commands(std::span<const Command> commands) {
    // Refused whole rather than truncated. Half a tick's commands would replay as a run nobody
    // performed, which is worse than a recording that stops and says where.
    if (m_replay.commands.size() + commands.size() > m_limits.max_commands) {
        return std::unexpected(Error(
            ErrorCode::Exhausted,
            std::format("this recording already holds {} commands and a reader accepts at most "
                        "{}; recording stopped rather than producing a file that cannot be read",
                        m_replay.commands.size(), m_limits.max_commands)));
    }
    m_replay.commands.insert(m_replay.commands.end(), commands.begin(), commands.end());
    return {};
}

Status ReplayRecorder::record_tick(const TickReport& report) {
    if (m_ticks_recorded % m_interval == 0 &&
        m_replay.checkpoints.size() >= m_limits.max_checkpoints) {
        return std::unexpected(
            Error(ErrorCode::Exhausted,
                  std::format("this recording already holds {} checkpoints and a reader accepts at "
                              "most {}; a larger checkpoint interval records fewer",
                              m_replay.checkpoints.size(), m_limits.max_checkpoints)));
    }

    ++m_replay.tick_count;
    ++m_ticks_recorded;

    if (m_ticks_recorded % m_interval == 0) {
        m_replay.checkpoints.push_back(HashCheckpoint{
            .tick = report.tick,
            .state_hash = report.state_hash,
            .system_hashes = report.system_hashes,
        });
    }
    return {};
}

Result<ReplayResult> play(const Replay& replay, World& world, Schedule& schedule,
                          CommandQueue& commands) {
    ATLAS_ZONE_NAMED("sim::play");

    if (!schedule.finalised()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "the schedule has not been finalised, so a playback would "
                                     "not be comparing the same thing"));
    }

    // Checked before anything runs. A playback from the wrong starting state would diverge at
    // the first checkpoint and look like a determinism failure, which it is not.
    if (const std::uint64_t actual = world.hash(); actual != replay.initial_state_hash) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("this replay starts from state {:#018x} and the world is in state "
                              "{:#018x}; a playback from the wrong state proves nothing",
                              replay.initial_state_hash, actual)));
    }

    commands.clear();
    for (const Command& command : replay.commands) {
        if (auto status = commands.submit_stamped(command); !status) {
            return std::unexpected(std::move(status).error().context(
                "queuing a recorded command; the build cannot replay what it cannot decode"));
        }
    }

    Kernel kernel(world, schedule, commands,
                  KernelConfig{.seed = replay.seed, .record_system_hashes = true});
    kernel.set_tick(replay.first_tick);

    // Indexed by tick, so a checkpoint is found without scanning. The recorder writes them in
    // order, and this does not assume it.
    std::size_t next_checkpoint = 0;

    ReplayResult result;

    for (std::uint64_t i = 0; i < replay.tick_count; ++i) {
        auto report = kernel.step();
        if (!report) {
            return std::unexpected(std::move(report).error());
        }
        ++result.ticks_run;

        while (next_checkpoint < replay.checkpoints.size() &&
               replay.checkpoints[next_checkpoint].tick < report->tick) {
            // A checkpoint for a tick already passed means the recording is out of order.
            ++next_checkpoint;
        }
        if (next_checkpoint >= replay.checkpoints.size() ||
            replay.checkpoints[next_checkpoint].tick != report->tick) {
            continue;
        }

        const HashCheckpoint& expected = replay.checkpoints[next_checkpoint];
        ++next_checkpoint;
        ++result.checkpoints_checked;

        if (expected.state_hash == report->state_hash) {
            continue;
        }

        // Diverged. Attribute it before returning, because the tick alone is not a starting
        // point for anybody.
        Divergence divergence;
        divergence.tick = report->tick;
        divergence.expected_hash = expected.state_hash;
        divergence.actual_hash = report->state_hash;

        for (const SystemHash& recorded : expected.system_hashes) {
            const auto at =
                std::ranges::find(report->system_hashes, recorded.system, &SystemHash::system);
            if (at == report->system_hashes.end() || at->hash != recorded.hash) {
                divergence.first_system = recorded.system;
                break;
            }
        }

        const System* system =
            divergence.first_system.has_value() ? schedule.find(*divergence.first_system) : nullptr;

        divergence.description = std::format(
            "the replay diverged at tick {}: expected state {:#018x}, got {:#018x}{}",
            divergence.tick, divergence.expected_hash, divergence.actual_hash,
            system != nullptr
                ? std::format("; the first system whose writes differ is '{}'", system->name)
                : "; no per-system hashes were recorded, so it cannot be "
                  "attributed to a system");

        ATLAS_LOG_ERROR(kSim, "{}", divergence.description);
        result.divergence = std::move(divergence);
        return result;
    }

    return result;
}

}  // namespace atlas::sim
