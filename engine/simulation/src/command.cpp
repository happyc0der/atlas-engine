// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/profile.hpp>
#include <atlas/simulation/command.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <utility>

namespace atlas::sim {
struct CommandQueue::Impl {
    // Ordered maps rather than hashed ones. Both are walked in key order when the queue is
    // saved, and the order of an unordered container is not a property anything may depend
    // on. See docs/DETERMINISM.md.
    std::map<CommandType, CommandHandler> handlers;
    std::map<SourceId, std::uint64_t> next_sequence;
};

CommandQueue::CommandQueue() : m_impl(std::make_unique<Impl>()) {}

CommandQueue::~CommandQueue() = default;
CommandQueue::CommandQueue(CommandQueue&& other) noexcept = default;
CommandQueue& CommandQueue::operator=(CommandQueue&& other) noexcept = default;

Status CommandQueue::register_handler(CommandType type, CommandHandler handler) {
    if (type == CommandType::Invalid) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "the invalid command type cannot have a handler"));
    }
    if (handler.apply == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, std::format("command type {} has no apply function",
                                                          static_cast<std::uint32_t>(type))));
    }
    if (handler.validate == nullptr) {
        // Required rather than optional. Without it an unvalidated payload would reach apply,
        // and apply is where it is too late to refuse.
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("command type {} has no validate function; a payload that reaches "
                              "apply can no longer be refused",
                              static_cast<std::uint32_t>(type))));
    }

    const auto [entry, inserted] = m_impl->handlers.emplace(type, std::move(handler));
    if (!inserted) {
        return std::unexpected(
            Error(ErrorCode::AlreadyExists, std::format("command type {} already has a handler",
                                                        static_cast<std::uint32_t>(type))));
    }
    return ok();
}

bool CommandQueue::has_handler(CommandType type) const noexcept {
    return m_impl->handlers.contains(type);
}

Status CommandQueue::submit(Tick target, SourceId source, CommandType type,
                            std::span<const std::byte> payload) {
    if (payload.size() > kMaxPayload) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange,
                  std::format("a command payload of {} bytes exceeds the {} byte limit",
                              payload.size(), kMaxPayload)));
    }
    if (m_pending.size() >= kMaxPending) {
        return std::unexpected(
            Error(ErrorCode::Exhausted,
                  std::format("the command queue holds {} commands, which is the limit; something "
                              "is producing them faster than the simulation consumes them",
                              kMaxPending)));
    }

    const auto handler = m_impl->handlers.find(type);
    if (handler == m_impl->handlers.end()) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("command type {} has no registered handler",
                                                   static_cast<std::uint32_t>(type))));
    }

    // Validated here, while the caller still has the context that produced it.
    if (auto status = handler->second.validate(payload); !status) {
        return std::unexpected(std::move(status).error().context(
            std::format("rejecting a command of type {}", static_cast<std::uint32_t>(type))));
    }

    std::uint64_t& sequence = m_impl->next_sequence[source];

    Command command;
    command.target = target;
    command.source = source;
    command.sequence = sequence++;
    command.type = type;
    command.payload.assign(payload.begin(), payload.end());

    m_pending.push_back(std::move(command));
    return ok();
}

Status CommandQueue::submit_stamped(Command command) {
    if (command.payload.size() > kMaxPayload) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange,
                  std::format("a command payload of {} bytes exceeds the {} byte limit",
                              command.payload.size(), kMaxPayload)));
    }
    if (m_pending.size() >= kMaxPending) {
        return std::unexpected(Error(ErrorCode::Exhausted, "the command queue is full"));
    }

    const auto handler = m_impl->handlers.find(command.type);
    if (handler == m_impl->handlers.end()) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("command type {} has no registered handler",
                                                   static_cast<std::uint32_t>(command.type))));
    }
    if (auto status = handler->second.validate(command.payload); !status) {
        return std::unexpected(std::move(status).error().context("rejecting a recorded command"));
    }

    // Keep the source's counter ahead of anything replayed into it, so a command submitted
    // afterwards cannot reuse a sequence number that already appears in the log.
    std::uint64_t& next = m_impl->next_sequence[command.source];
    next = std::max(next, command.sequence + 1);

    m_pending.push_back(std::move(command));
    return ok();
}

std::vector<Command> CommandQueue::drain(Tick tick) {
    ATLAS_ZONE_NAMED("CommandQueue::drain");

    std::vector<Command> taken;

    // Partitioned rather than filtered into a new vector, so the commands that stay keep
    // their storage and the ones that go move out of it.
    const auto due = std::ranges::stable_partition(
        m_pending, [tick](const Command& command) { return command.target > tick; });

    taken.reserve(static_cast<std::size_t>(due.size()));
    for (Command& command : due) {
        taken.push_back(std::move(command));
    }
    m_pending.erase(due.begin(), due.end());

    // The total order. By source first and sequence second, so it is the same on every
    // machine and independent of when anything arrived.
    //
    // The tie-break after that exists because `sort` is not stable and the two fields above are
    // only unique if every producer keeps its promise. `submit` assigns the sequence and cannot
    // repeat one; `submit_stamped` keeps whatever it is given, and until M15's opening sweep a
    // peer could label a command with another peer's source and collide with it. That hole is
    // closed where it was reachable — `net::Session` now refuses such a turn — but the order
    // itself should not depend on that check being the only one, because the failure is silent:
    // two peers would each sort the pair their own way and diverge with nothing logged.
    //
    // Stability is not the fix. `stable_sort` would preserve insertion order, and insertion
    // order is exactly what differs between peers when a link reorders messages, so it would
    // make the result depend on arrival — which is the one thing this order exists to avoid.
    // Comparing the content instead gives the same answer on every machine, and costs nothing
    // in the ordinary case because it is only reached when both keys are equal.
    std::ranges::sort(taken, [](const Command& a, const Command& b) {
        if (a.source != b.source) {
            return a.source < b.source;
        }
        if (a.sequence != b.sequence) {
            return a.sequence < b.sequence;
        }
        if (a.type != b.type) {
            return a.type < b.type;
        }
        return std::ranges::lexicographical_compare(a.payload, b.payload);
    });

    return taken;
}

Result<ApplyOutcome> CommandQueue::apply(World& world, const Command& command) const {
    const auto handler = m_impl->handlers.find(command.type);
    if (handler == m_impl->handlers.end()) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("command type {} has no registered handler",
                                                   static_cast<std::uint32_t>(command.type))));
    }

    // Validated again rather than trusted. A command may have been submitted before a load
    // replaced the state it referred to, and the cost of checking is a function call.
    if (auto status = handler->second.validate(command.payload); !status) {
        return std::unexpected(std::move(status).error().context(std::format(
            "a command of type {} from source {} no longer validates",
            static_cast<std::uint32_t>(command.type), static_cast<std::uint32_t>(command.source))));
    }

    const ApplyContext context{.source = command.source, .tick = command.target};
    return ApplyOutcome{.verdict = handler->second.apply(world, context, command.payload)};
}

std::uint64_t CommandQueue::next_sequence(SourceId source) const noexcept {
    const auto at = m_impl->next_sequence.find(source);
    return at == m_impl->next_sequence.end() ? 0 : at->second;
}

std::vector<SourceId> CommandQueue::sources() const {
    std::vector<SourceId> out;
    out.reserve(m_impl->next_sequence.size());
    // The map is ordered, so this comes out sorted without sorting.
    for (const auto& [source, sequence] : m_impl->next_sequence) {
        out.push_back(source);
    }
    return out;
}

void CommandQueue::set_next_sequence(SourceId source, std::uint64_t sequence) {
    m_impl->next_sequence[source] = sequence;
}

void CommandQueue::clear() {
    m_pending.clear();
    m_impl->next_sequence.clear();
}

}  // namespace atlas::sim
