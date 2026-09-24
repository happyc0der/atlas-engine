// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A lockstep session: who is playing, whose turns have arrived, and when to stop.
///
/// This is the piece that turns a link and a gate into deterministic lockstep. It implements
/// `sim::CommandSource`, so from the tick's point of view it is the same kind of thing as a
/// replay or a mod: something outside that submits stamped commands and says when a turn is
/// done. What is particular to it is that it speaks for **every peer it is connected to**, not
/// for one.
///
/// **The handshake refuses rather than hopes.** Every peer announces what it is — protocol,
/// hash algorithm, save and replay formats, both golden hashes, seed, start tick, initial state
/// hash, tick rate — and a mismatch in any of them ends the session before a tick runs. Two
/// builds that produce the same golden hashes agree about the simulation whatever else differs
/// between them, which is the condition the charter puts on a session between different builds.
/// A differing build identifier is logged and not refused, so a debug peer and a release peer
/// whose goldens agree can still play.
///
/// **The agreed input delay is the largest anybody proposed**, not the host's. A peer given less
/// delay than it needs is late every tick, and late is fatal here.
///
/// **A late turn is a protocol violation, not a warning.** A command stamped for a tick that has
/// already run cannot be applied by anybody, so the session is no longer sound and it ends. That
/// is checked before the command reaches the queue, so the kernel never sees it.
///
/// **A divergence stops the session and is never recovered from**, the same treatment device
/// loss gets: detected, attributed to the first system whose writes differ, reported with an
/// actionable error. Atlas does not resync. The recordings on each side are the debugging
/// artefact.
///
/// Thread affinity: the main thread, with the kernel and the command queue.

#include <atlas/core/result.hpp>
#include <atlas/net/link.hpp>
#include <atlas/net/message.hpp>
#include <atlas/simulation/command_source.hpp>
#include <atlas/simulation/divergence.hpp>
#include <atlas/simulation/schedule.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace atlas::net {

struct SessionConfig {
    /// Ticks between stamping a command and running it. The session agrees on the largest
    /// proposal it sees.
    std::uint32_t input_delay = 2;

    /// How often to broadcast a hash for comparison. Every tick would be correct and wasteful;
    /// the hash is computed every tick anyway, so this costs a message rather than a hash.
    std::uint32_t hash_check_interval = 16;

    std::uint64_t seed = 0;
    Tick start_tick = 0;
    std::uint64_t initial_state_hash = 0;
    std::uint32_t tick_rate = 60;

    /// Logged on a mismatch rather than refused. See the file comment.
    std::string build_id;
};

/// What the session is doing.
enum class SessionState : std::uint8_t {
    /// Announcing and waiting to be announced to. No tick runs yet.
    Handshaking,
    /// Every peer is agreed and the gate is expecting them.
    Running,
    /// This peer has said its last tick and is waiting for its partners to say theirs, and for
    /// what they still owe it up to that tick (ADR-0020). Turns and hash checks up to the last
    /// tick are still sent and still compared.
    Finishing,
    /// The run is over and every peer agreed where. Not a failure: `poll` succeeds, reports
    /// `finished`, and delivers nothing. A disconnect after this carries no information.
    Finished,
    /// Over, for the recorded reason. Nothing further is sent or accepted.
    Ended,
};

/// Counters a run can print, and a test can assert were not zero.
struct SessionStats {
    std::uint64_t turns_sent = 0;
    std::uint64_t turns_received = 0;
    std::uint64_t commands_received = 0;
    std::uint64_t hash_checks_sent = 0;
    std::uint64_t hash_checks_agreed = 0;
};

/// One peer's view of a lockstep session.
class Session final : public sim::CommandSource {
  public:
    /// Announce this peer to every other and start the handshake.
    ///
    /// Never blocks. The session is `Handshaking` when this returns, and reaches `Running` in a
    /// later `poll` once every peer has been heard from.
    ///
    /// **The link end is taken by value; what must outlive the session is the link behind it.**
    /// This sentence used to read "the link end is borrowed and must outlive the session",
    /// which was true of the hub the end points at and false of the parameter it sits above.
    /// A `LinkEnd` is a handle — an index and a pointer to its backend — so copying one is
    /// cheap and keeping one is meaningless once the backend is gone. M17 corrected the wording
    /// before widening this interface, because it is the sentence a transport author reads.
    ///
    /// Handed out by pointer because a `CommandSource` is deliberately neither copyable nor
    /// movable: something holding a reference to one must be able to rely on it staying put.
    [[nodiscard]] static Result<std::unique_ptr<Session>> create(LinkEnd link,
                                                                 SessionConfig config);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() override = default;

    /// This peer's identifier, which is what its own commands must be stamped with.
    ///
    /// **Not `SourceId::Local` unless this is peer zero.** `Local` is a role, not "whoever is
    /// running this": a peer that stamped it would be signing another peer's name to its own
    /// commands, and the two streams would collide in the total order lockstep depends on.
    [[nodiscard]] sim::SourceId self() const noexcept { return m_self; }

    [[nodiscard]] sim::SourceId id() const noexcept override { return m_self; }

    /// Bring in everything that has arrived, and hand it to the queue and the gate.
    ///
    /// Marks the turns of the peers it is connected to, which is why it takes the gate rather
    /// than a handle bound to one identifier.
    ///
    /// Fails when the session has ended or must end: a protocol violation, a peer that cannot be
    /// played with, an inbox that overflowed, or a divergence. A failure is final — `running()`
    /// is false afterwards and nothing further will be accepted.
    [[nodiscard]] Result<sim::PollReport> poll(Tick now, sim::CommandQueue& queue,
                                               sim::TurnGate& turns) override;

    /// Announce this peer's commands for `tick` and mark its own turn.
    ///
    /// **Always called, even with nothing to send.** An empty turn is a message, because silence
    /// is also what a dead peer produces and a gate with no timeout cannot tell them apart.
    ///
    /// **And called for every tick, with no gaps, starting at the first one.** A turn for tick
    /// five says nothing about ticks zero to four: the gate advances a source's frontier only
    /// over consecutive completed turns, so a driver that skips any tick stalls the session at
    /// the first one it skipped. In particular a driver stamping commands for `now + delay` must
    /// still announce the first `delay` ticks, empty, before anyone can run them — nobody will
    /// ever have commands for those, and without the announcement nobody will ever run them
    /// either.
    ///
    /// The local turn is marked here rather than waiting for it to come back over the link: a
    /// peer does not need the network to know what it did.
    [[nodiscard]] Status send_turn(Tick tick, std::span<const sim::Command> commands,
                                   sim::TurnGate& turns);

    /// Announce what this peer made of a tick, for comparison.
    ///
    /// Sends only on the agreed interval; calling it every tick is correct and costs nothing on
    /// the ticks it skips.
    [[nodiscard]] Status send_hash_check(Tick tick, std::uint64_t state_hash,
                                         std::span<const sim::SystemHash> system_hashes);

    /// Say goodbye and stop. Idempotent.
    ///
    /// **Does nothing once the session is `Finished`.** A finished peer has said everything its
    /// partners need, and a goodbye after that would reach a partner still finishing as a peer
    /// that left — turning a completed run into a failed one on the other side.
    void quit();

    /// Declare that this peer will run no tick after `last_tick` (ADR-0020).
    ///
    /// Allowed only while `Running`, and only once this peer has announced its turns up to
    /// `last_tick`: a peer that finished before saying what it would do on its last tick would
    /// leave its partners unable to run it. Sends `Finish` and moves to `Finishing`.
    ///
    /// Fails, and ends the session as a protocol violation, when a partner has already said it
    /// finishes at a different tick — two peers that disagree about where the run ends have
    /// disagreed about what the run was.
    ///
    /// The session becomes `Finished` in a later `poll`, decided by this peer alone from what has
    /// arrived: every partner has finished at the same tick, this peer has run that tick — which
    /// the gate allows only once every partner's turns up to it have arrived — and the last
    /// hash check at or before it has been compared with every partner. No clock is involved: a
    /// partner that never finishes is a silent peer, and the transport's deadline ends the
    /// session exactly as it would have while running.
    [[nodiscard]] Status finish(Tick last_tick);

    /// True once the run is over and agreed. See `finish`.
    ///
    /// **Ask this before asking whether the link has ended.** A partner leaves only once it is
    /// finished itself, which needs this peer's finish first, so a link that ends while this
    /// peer is finishing ends after the partner sent everything it will send — and the poll
    /// that received that also decided this. Asked the other way round, a completed run reads
    /// as a lost peer.
    [[nodiscard]] bool finished() const noexcept { return m_state == SessionState::Finished; }

    /// The tick any peer, this one included, has said the run ends at. Empty until one has.
    ///
    /// Every declared finish agrees or the session has already ended, so there is one answer.
    /// **What an application compares against its own idea of the run.** The session cannot
    /// tell a partner finishing early from one finishing on time, because it does not know how
    /// long the run was meant to be; the application does. A driver that waits for its own
    /// bound instead may wait on a partner that has stopped announcing turns — both alive, both
    /// waiting, and nothing but a wall clock to end it.
    [[nodiscard]] std::optional<Tick> declared_finish() const noexcept;

    [[nodiscard]] SessionState state() const noexcept { return m_state; }

    [[nodiscard]] bool running() const noexcept { return m_state == SessionState::Running; }

    /// Set once, when the hashes stopped agreeing. Empty otherwise.
    [[nodiscard]] const std::optional<sim::Divergence>& divergence() const noexcept {
        return m_divergence;
    }

    /// Every participant, this peer included, in identifier order. Empty until `Running`.
    [[nodiscard]] std::span<const sim::SourceId> peers() const noexcept { return m_peers; }

    [[nodiscard]] std::uint32_t agreed_delay() const noexcept { return m_agreed_delay; }

    [[nodiscard]] const SessionStats& stats() const noexcept { return m_stats; }

    /// The schedule used to name a system in a divergence. Borrowed, optional, and only ever
    /// read for a name.
    void set_schedule(const sim::Schedule* schedule) noexcept { m_schedule = schedule; }

  private:
    Session(LinkEnd link, SessionConfig config);

    /// What this peer says about itself.
    [[nodiscard]] Hello own_hello() const;

    /// Refuse a peer whose build cannot be compared with this one.
    [[nodiscard]] Status check_compatible(std::size_t peer, const Hello& hello) const;

    [[nodiscard]] Status handle(std::size_t peer, const Message& message, Tick now,
                                sim::CommandQueue& queue, sim::TurnGate& turns,
                                sim::PollReport& report);

    /// Send a farewell to everyone and move to Ended. The send is best effort: a peer that
    /// flooded us may well not be listening, and the authoritative act is the local transition.
    void end(ByeReason reason, std::string detail);

    /// A received finish, checked against every other finish this peer knows of.
    [[nodiscard]] Status accept_finish(std::size_t peer, const Finish& finish, Tick now);

    /// Whether `Finishing` has become `Finished`. See `finish`.
    [[nodiscard]] bool finish_is_complete(Tick now) const noexcept;

    LinkEnd m_link;
    SessionConfig m_config;
    SessionState m_state = SessionState::Handshaking;
    sim::SourceId m_self = sim::SourceId::Local;
    std::uint32_t m_agreed_delay = 0;
    std::vector<sim::SourceId> m_peers;

    /// The host collects one of these per peer before it can agree a delay.
    std::vector<std::optional<Hello>> m_heard;

    /// The highest tick this peer has announced a turn for, so `finish` can refuse a last tick
    /// its partners have not been told about.
    std::optional<Tick> m_last_turn_sent;

    /// Where this peer finishes, once it has said so.
    std::optional<Tick> m_finish_tick;

    /// Where each peer said it finishes, by link index. This peer's own entry stays empty.
    std::vector<std::optional<Tick>> m_peer_finish;

    /// The highest hash check each peer has had compared and agreed, by link index. What the
    /// finished condition asks, so a peer cannot finish without having compared its last one.
    std::vector<std::optional<Tick>> m_compared_through;

    /// The last few hashes this peer computed, so a remote check has something to compare
    /// against. Checkpoints rather than ticks: every peer checks on the same interval from the
    /// same start tick, so a remote check always names a tick this peer *will* also check.
    ///
    /// **"Will", not "did", and M17 is the milestone that found the difference.** Peers check
    /// the same ticks but do not reach them at the same moment, so a check can arrive before
    /// this peer has computed the tick it names. Over the in-memory link that almost never
    /// happened — every peer is driven from one loop — and over a socket it happens constantly.
    struct Checkpoint {
        Tick tick = 0;
        std::uint64_t state_hash = 0;
        std::vector<sim::SystemHash> system_hashes;
    };

    std::deque<Checkpoint> m_checkpoints;

    /// Checks that arrived for ticks this peer has not reached, held until it does.
    ///
    /// Bounded, because the producer is a peer. Under lockstep one cannot get far ahead — it
    /// may only run a tick every participant has reported for — so anything beyond a couple of
    /// checks outstanding is a peer that is not playing the same game, and is refused as such.
    std::deque<HashCheck> m_pending_checks;

    [[nodiscard]] Status compare_or_hold(std::size_t peer, const HashCheck& check);
    [[nodiscard]] Status compare_check(std::size_t peer, const HashCheck& check,
                                       const Checkpoint& mine);

    std::optional<sim::Divergence> m_divergence;
    const sim::Schedule* m_schedule = nullptr;
    SessionStats m_stats;

    /// Reused across polls so an ordinary frame allocates nothing for them.
    std::vector<std::vector<std::byte>> m_incoming;
};

}  // namespace atlas::net
