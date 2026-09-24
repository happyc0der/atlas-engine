// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/net/session.hpp>
#include <atlas/simulation/golden.hpp>
#include <atlas/simulation/replay.hpp>
#include <atlas/simulation/save.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace atlas::net {
namespace {

constexpr log::Category kNet{"net"};

/// Most messages taken from one peer in one poll.
///
/// Back-pressure rather than an error: reaching it means this peer has more to say and will be
/// heard next poll. What it buys is that one peer with a backlog cannot starve another in the
/// same poll, which matters because a turn nobody reads is a tick nobody runs.
constexpr std::size_t kMaxMessagesPerPeerPerPoll = 64;

/// The last few checkpoints kept for comparison. At the default interval this is a thousand
/// ticks of tolerance, far more than any input delay.
constexpr std::size_t kCheckpointsKept = 64;

/// Remote checks held for ticks this peer has not reached yet.
///
/// Small on purpose. Under lockstep a peer may only run a tick every participant has reported
/// for, so it can lead by at most the input delay — a couple of checks at any sane interval.
/// A peer with more outstanding than this is not merely ahead, it is running a different
/// session, and saying so is better than growing a queue on its behalf.
constexpr std::size_t kMaxPendingChecks = 8;

}  // namespace

Session::Session(LinkEnd link, SessionConfig config)
    : m_link(link), m_config(std::move(config)), m_agreed_delay(m_config.input_delay) {
    m_heard.resize(m_link.peer_count());
}

Hello Session::own_hello() const {
    return Hello{
        .hash_algorithm_version = kHashAlgorithmVersion,
        .save_format_version = sim::kSaveFormatVersion,
        .replay_format_version = sim::kReplayFormatVersion,
        .build_id = m_config.build_id,
        .golden_final_state = sim::kGoldenFinalState,
        .golden_all_ticks = sim::kGoldenAllTicks,
        .seed = m_config.seed,
        .start_tick = m_config.start_tick,
        .initial_state_hash = m_config.initial_state_hash,
        .tick_rate = m_config.tick_rate,
        .proposed_delay = m_config.input_delay,
    };
}

Result<std::unique_ptr<Session>> Session::create(LinkEnd link, SessionConfig config) {
    if (config.hash_check_interval == 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "a hash check interval of zero would never compare anything, "
                                     "which is a session with no divergence detection at all"));
    }
    if (config.input_delay == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  "an input delay of zero stamps a command for the tick that is already "
                  "running, so it would be late on arrival at every other peer"));
    }

    std::unique_ptr<Session> session(new Session(link, std::move(config)));

    // Everyone announces, so that every peer can refuse a build it cannot be compared with
    // rather than only one of them being able to.
    const auto announcement = encode(Message{session->own_hello()});
    if (!announcement) {
        return std::unexpected(announcement.error());
    }
    if (auto status = session->m_link.broadcast(*announcement); !status) {
        return std::unexpected(std::move(status).error().context("announcing this peer"));
    }
    return session;
}

Status Session::check_compatible(std::size_t peer, const Hello& hello) const {
    const Hello mine = own_hello();

    const auto refuse = [&](std::string_view what, auto theirs, auto ours) {
        return std::unexpected(
            Error(ErrorCode::VersionMismatch,
                  std::format("peer {} cannot be played with: its {} is {} and this build's is {}",
                              peer, what, theirs, ours)));
    };

    if (hello.hash_algorithm_version != mine.hash_algorithm_version) {
        return refuse("hash algorithm version", hello.hash_algorithm_version,
                      mine.hash_algorithm_version);
    }
    if (hello.save_format_version != mine.save_format_version) {
        return refuse("save format version", hello.save_format_version, mine.save_format_version);
    }
    if (hello.replay_format_version != mine.replay_format_version) {
        return refuse("replay format version", hello.replay_format_version,
                      mine.replay_format_version);
    }

    // The probe. Two builds producing the same golden hashes agree about the simulation whatever
    // else differs between them, and the charter permits a cross-build session only on that
    // condition. It is a probe rather than a proof, and the ADR says so.
    if (hello.golden_final_state != mine.golden_final_state ||
        hello.golden_all_ticks != mine.golden_all_ticks) {
        return std::unexpected(Error(
            ErrorCode::VersionMismatch,
            std::format("peer {} does not agree about the fixed scenario: it reports {:#018x} and "
                        "{:#018x} where this build reports {:#018x} and {:#018x}. Two builds may "
                        "only play together when these match.",
                        peer, hello.golden_final_state, hello.golden_all_ticks,
                        mine.golden_final_state, mine.golden_all_ticks)));
    }

    // A peer starting from a different world is not a divergence to detect later; it is a
    // session that was never going to be fair. Refused up front, as a replay from the wrong
    // starting state already is.
    if (hello.seed != mine.seed) {
        return refuse("seed", hello.seed, mine.seed);
    }
    if (hello.start_tick != mine.start_tick) {
        return refuse("start tick", hello.start_tick, mine.start_tick);
    }
    if (hello.initial_state_hash != mine.initial_state_hash) {
        return refuse("initial state hash", hello.initial_state_hash, mine.initial_state_hash);
    }
    if (hello.tick_rate != mine.tick_rate) {
        return refuse("tick rate", hello.tick_rate, mine.tick_rate);
    }

    // Logged, never refused. The goldens are the probe; refusing on a build identifier would
    // forbid a debug peer playing a release peer even when they agree about the simulation,
    // which is a thing worth being able to test.
    if (hello.build_id != mine.build_id) {
        ATLAS_LOG_INFO(kNet,
                       "peer {} is a different build ('{}' against '{}'); its golden hashes "
                       "agree, so the session continues",
                       peer, hello.build_id, mine.build_id);
    }
    return {};
}

void Session::end(ByeReason reason, std::string detail) {
    if (m_state == SessionState::Ended) {
        return;
    }
    m_state = SessionState::Ended;

    // Best effort. A peer that flooded us may not be listening, and the act that matters is the
    // local transition rather than the farewell: nothing here waits for it or depends on it.
    if (const auto farewell = encode(Message{Bye{.reason = reason, .detail = std::move(detail)}})) {
        // The result is deliberately discarded. A peer that flooded us or sent rubbish may well
        // not be listening, and there is nothing useful to do about a farewell that does not
        // arrive; the act that matters is the local transition above, which has already
        // happened.
        const Status sent = m_link.broadcast(*farewell);
        (void)sent;
    }
}

void Session::quit() {
    end(ByeReason::Quit, "this peer left");
}

Status Session::send_turn(Tick tick, std::span<const sim::Command> commands, sim::TurnGate& turns) {
    if (m_state != SessionState::Running) {
        return std::unexpected(Error(ErrorCode::Unavailable, "the session is not running"));
    }

    Turn turn{.tick = tick, .source = m_self, .commands = {commands.begin(), commands.end()}};
    const auto bytes = encode(Message{std::move(turn)});
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (auto status = m_link.broadcast(*bytes); !status) {
        return status;
    }
    ++m_stats.turns_sent;

    // Marked here rather than waiting for it to come back over the link: a peer does not need
    // the network to learn what it did. At the same point in the frame as a remote turn, so the
    // two paths cannot drift in when they open the gate.
    return turns.mark_complete(m_self, tick);
}

/// Compare a remote check against this peer's own, or hold it until there is one.
///
/// **The two failure cases are not the same failure, and treating them alike is what M17
/// found.** A check for a tick older than anything held is a peer on a different schedule, or a
/// ring too small for the lag — either way there is nothing to compare against and never will
/// be. A check for a tick this peer has *not reached yet* is ordinary: peers check the same
/// ticks and arrive at them at different moments, which the in-memory link hid because every
/// peer there is driven from one loop.
Status Session::compare_or_hold(std::size_t peer, const HashCheck& check) {
    // First, and before anything about timing: is this a tick anybody here checks at all? Every
    // peer checks on the same interval from the same start tick, so one naming a tick off that
    // interval is on a different schedule and no amount of waiting will produce a counterpart.
    // Refusing it is what stops the divergence detector being quietly vacuous, and it is a
    // separate question from whether this peer has got there yet.
    if (check.tick % m_config.hash_check_interval != 0) {
        return std::unexpected(Error(
            ErrorCode::InvalidArgument,
            std::format("peer {} sent a hash check for tick {}, which this peer did not check "
                        "because it is not on the check interval of {}; the two are not "
                        "comparing the same ticks",
                        peer, check.tick, m_config.hash_check_interval)));
    }

    const auto mine = std::ranges::find(m_checkpoints, check.tick, &Checkpoint::tick);
    if (mine != m_checkpoints.end()) {
        return compare_check(peer, check, *mine);
    }

    // On the interval but not reached yet: hold it. Not "ignore it" — ignoring is the thing
    // that would make the detector vacuous, and holding answers it a few ticks later instead.
    const bool ahead = m_checkpoints.empty() || check.tick > m_checkpoints.back().tick;
    if (ahead) {
        if (m_pending_checks.size() >= kMaxPendingChecks) {
            return std::unexpected(Error(
                ErrorCode::Exhausted,
                std::format("peer {} has {} hash checks outstanding for ticks this peer has not "
                            "reached; under lockstep it cannot legitimately be that far ahead",
                            peer, m_pending_checks.size())));
        }
        m_pending_checks.push_back(check);
        return {};
    }

    // On the interval, and older than anything still held. Either this peer lagged far enough
    // that the ring retired the counterpart, or the two are genuinely out of step. Both are
    // worth stopping for, and neither is fixable by waiting.
    return std::unexpected(
        Error(ErrorCode::InvalidArgument,
              std::format("peer {} sent a hash check for tick {}, which this peer has already "
                          "passed and no longer holds; the two are not comparing the same ticks",
                          peer, check.tick)));
}

/// The comparison itself, once both halves exist.
Status Session::compare_check(std::size_t peer, const HashCheck& check, const Checkpoint& mine) {
    if (mine.state_hash == check.state_hash) {
        ++m_stats.hash_checks_agreed;
        return {};
    }

    static const sim::Schedule kNoSchedule;
    m_divergence = sim::attribute_divergence(check.tick, mine.state_hash, check.state_hash,
                                             mine.system_hashes, check.system_hashes,
                                             m_schedule != nullptr ? *m_schedule : kNoSchedule);
    // Named with the peer as well as the system: in a three-peer session "system X differs"
    // without "between me and peer 2" is not a starting point for anybody.
    m_divergence->description =
        std::format("{} (between source {} and peer {})", m_divergence->description,
                    static_cast<std::uint32_t>(m_self), peer);
    return std::unexpected(Error(ErrorCode::IntegrityCheckFailed, m_divergence->description));
}

Status Session::send_hash_check(Tick tick, std::uint64_t state_hash,
                                std::span<const sim::SystemHash> system_hashes) {
    if (m_state != SessionState::Running) {
        return std::unexpected(Error(ErrorCode::Unavailable, "the session is not running"));
    }
    if (tick % m_config.hash_check_interval != 0) {
        return {};
    }

    m_checkpoints.push_back(Checkpoint{
        .tick = tick,
        .state_hash = state_hash,
        .system_hashes = {system_hashes.begin(), system_hashes.end()},
    });
    while (m_checkpoints.size() > kCheckpointsKept) {
        m_checkpoints.pop_front();
    }

    // Anything that arrived early for this tick can now be answered. Done here rather than in
    // `poll`, because this is the moment the missing half appears and nowhere else.
    for (auto pending = m_pending_checks.begin(); pending != m_pending_checks.end();) {
        if (pending->tick != tick) {
            ++pending;
            continue;
        }
        const auto held = *pending;
        pending = m_pending_checks.erase(pending);
        if (auto status =
                compare_check(static_cast<std::size_t>(held.source), held, m_checkpoints.back());
            !status) {
            return status;
        }
    }

    HashCheck check{
        .tick = tick,
        .source = m_self,
        .state_hash = state_hash,
        .system_hashes = {system_hashes.begin(), system_hashes.end()},
    };
    const auto bytes = encode(Message{std::move(check)});
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (auto status = m_link.broadcast(*bytes); !status) {
        return status;
    }
    ++m_stats.hash_checks_sent;
    return {};
}

Status Session::handle(std::size_t peer, const Message& message, Tick now, sim::CommandQueue& queue,
                       sim::TurnGate& turns, sim::PollReport& report) {
    const auto peer_source = sim::SourceId{static_cast<std::uint32_t>(peer)};

    if (const auto* hello = std::get_if<Hello>(&message)) {
        if (auto status = check_compatible(peer, *hello); !status) {
            return status;
        }
        m_heard[peer] = *hello;

        // Every peer heard from, this one included: agree a delay and start. The delay is the
        // largest anybody proposed, because a peer given less than it needs is late every tick.
        m_heard[m_link.index()] = own_hello();
        if (std::ranges::all_of(m_heard, [](const auto& h) { return h.has_value(); })) {
            m_agreed_delay = 0;
            for (const auto& heard : m_heard) {
                // Every entry is present: the loop above only runs when all of them are.
                if (heard.has_value()) {
                    m_agreed_delay = std::max(m_agreed_delay, heard->proposed_delay);
                }
            }
            m_peers.clear();
            m_peers.reserve(m_link.peer_count());
            for (std::size_t i = 0; i < m_link.peer_count(); ++i) {
                m_peers.push_back(sim::SourceId{static_cast<std::uint32_t>(i)});
            }
            m_self = sim::SourceId{static_cast<std::uint32_t>(m_link.index())};
            if (auto status = turns.expect_sources(m_peers); !status) {
                return status;
            }
            m_state = SessionState::Running;
            ATLAS_LOG_INFO(kNet, "session running: {} peers, this one is source {}, input delay {}",
                           m_peers.size(), static_cast<std::uint32_t>(m_self), m_agreed_delay);
        }
        return {};
    }

    if (const auto* turn = std::get_if<Turn>(&message)) {
        // A peer may speak only for itself. Taken from the link it arrived on rather than from
        // the message, so a peer cannot submit a turn on another's behalf by writing its name.
        if (turn->source != peer_source) {
            return std::unexpected(
                Error(ErrorCode::InvalidArgument,
                      std::format("peer {} sent a turn claiming to be source {}", peer,
                                  static_cast<std::uint32_t>(turn->source))));
        }
        // Late is a protocol violation rather than a warning. A command stamped for a tick that
        // has already run cannot be applied by anybody, so the session is no longer sound —
        // checked here, before the queue sees it, so the kernel never counts it late.
        if (turn->tick < now) {
            return std::unexpected(Error(
                ErrorCode::InvalidArgument,
                std::format("peer {} sent a turn for tick {} while tick {} is already running; "
                            "under lockstep that command can no longer be applied anywhere",
                            peer, turn->tick, now)));
        }

        // The same rule, one level down. The check above proves the *envelope* belongs to the
        // peer that sent it; each command inside carries its own `source`, and until M15's
        // opening sweep nothing looked at it. A peer could therefore label a command as coming
        // from another peer, and because `submit_stamped` keeps the recorded sequence number,
        // two commands could end up sharing one `(source, sequence)` key — at which point the
        // total order `drain` relies on is not total and two peers can order them differently.
        // That is a silent divergence reachable from the wire, so it is a protocol violation
        // like the envelope, not a counted refusal.
        //
        // Checked before anything is submitted rather than inside the loop below. A turn is one
        // message and one event: submitting half of it and then refusing the rest would leave
        // the queue holding commands from a turn this session has decided is invalid.
        for (const sim::Command& command : turn->commands) {
            if (command.source != peer_source) {
                return std::unexpected(Error(
                    ErrorCode::InvalidArgument,
                    std::format("peer {} sent a turn for tick {} containing a command labelled "
                                "source {}; a peer may speak only for itself",
                                peer, turn->tick, static_cast<std::uint32_t>(command.source))));
            }
        }

        for (const sim::Command& command : turn->commands) {
            if (const auto status = queue.submit_stamped(command); !status) {
                // One peer's bad command does not stop the others being read. Counted, because
                // a rising number means something upstream is producing rubbish.
                ++report.commands_refused;
                continue;
            }
            ++report.commands_submitted;
            report.highest_target = std::max(report.highest_target, command.target);
        }
        ++m_stats.turns_received;
        m_stats.commands_received += turn->commands.size();

        if (auto status = turns.mark_complete(peer_source, turn->tick); !status) {
            return status;
        }
        ++report.turns_marked;
        return {};
    }

    if (const auto* check = std::get_if<HashCheck>(&message)) {
        if (check->source != peer_source) {
            return std::unexpected(Error(
                ErrorCode::InvalidArgument,
                std::format("peer {} sent a hash check claiming to be another source", peer)));
        }
        return compare_or_hold(peer, *check);
    }

    if (const auto* bye = std::get_if<Bye>(&message)) {
        return std::unexpected(
            Error(ErrorCode::Unavailable,
                  std::format("peer {} left: {} ({})", peer, to_string(bye->reason), bye->detail)));
    }

    if (std::holds_alternative<Finish>(message)) {
        // Named rather than falling through to the welcome refusal below, which would report a
        // finish as a message it is not. M21 slice 2 gives it its meaning (ADR-0020).
        return std::unexpected(Error(
            ErrorCode::NotSupported,
            std::format("peer {} sent a finish, which this build does not act on yet", peer)));
    }

    // A welcome is not part of this handshake: every peer announces and every peer agrees, so
    // there is nobody to be welcomed by. The message exists in the protocol because a session
    // with a host assigning identities is the shape a transport will want, and adding it later
    // would be a protocol version change.
    return std::unexpected(Error(ErrorCode::InvalidArgument,
                                 std::format("peer {} sent a welcome, which this handshake does "
                                             "not use",
                                             peer)));
}

Result<sim::PollReport> Session::poll(Tick now, sim::CommandQueue& queue, sim::TurnGate& turns) {
    sim::PollReport report;
    if (m_state == SessionState::Ended) {
        report.closed = true;
        return std::unexpected(Error(ErrorCode::Unavailable, "the session has ended"));
    }

    // Deliver whatever the link owes this peer. Done every poll whether or not a tick ran,
    // which is what makes a stalled peer able to receive the turn it is waiting for.
    m_link.pump();

    for (std::size_t peer = 0; peer < m_link.peer_count(); ++peer) {
        if (peer == m_link.index()) {
            continue;
        }
        CommandInbox& inbox = m_link.inbox(peer);
        if (inbox.overflowed()) {
            end(ByeReason::Overflow,
                std::format("peer {} sent faster than it could be read", peer));
            report.closed = true;
            return std::unexpected(
                Error(ErrorCode::Exhausted,
                      std::format("peer {} overran its inbox; a lockstep stream with a hole in it "
                                  "cannot be resumed",
                                  peer)));
        }

        inbox.drain(m_incoming);
        std::size_t handled = 0;
        for (const auto& bytes : m_incoming) {
            if (handled >= kMaxMessagesPerPeerPerPoll) {
                // Back-pressure, not an error: the rest is read next poll. What this buys is
                // that one peer with a backlog cannot starve another in the same poll.
                break;
            }
            ++handled;

            auto message = decode(bytes);
            if (!message) {
                end(ByeReason::ProtocolError, message.error().message());
                report.closed = true;
                return std::unexpected(std::move(message).error().context(
                    std::format("decoding a message from peer {}", peer)));
            }
            if (auto status = handle(peer, *message, now, queue, turns, report); !status) {
                const bool diverged = m_divergence.has_value();
                end(diverged ? ByeReason::Diverged : ByeReason::ProtocolError,
                    status.error().message());
                report.closed = true;
                return std::unexpected(std::move(status).error());
            }
        }

        // Anything past the per-poll cap is put back, in order, so back-pressure delays a
        // message rather than losing it.
        for (std::size_t i = handled; i < m_incoming.size(); ++i) {
            (void)inbox.push(std::move(m_incoming[i]));
        }
    }

    return report;
}

}  // namespace atlas::net
