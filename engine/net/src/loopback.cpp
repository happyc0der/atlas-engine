// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/net/loopback.hpp>
#include <atlas/simulation/rng.hpp>

#include <algorithm>
#include <format>

namespace atlas::net {
namespace {

/// The shortest message worth corrupting: anything longer than the header has a body to damage.
///
/// A flip inside the sixteen-byte header would be refused as "not an Atlas message" before
/// anything interesting happened, which would test the magic check rather than the session.
constexpr std::size_t kCorruptionFloor = 16;

}  // namespace

LoopbackHub::LoopbackHub(const LoopbackConfig& config) : m_config(config) {
    m_peers.resize(config.peer_count);
    for (std::size_t peer = 0; peer < config.peer_count; ++peer) {
        m_peers[peer].inboxes.reserve(config.peer_count);
        for (std::size_t from = 0; from < config.peer_count; ++from) {
            m_peers[peer].inboxes.push_back(std::make_unique<CommandInbox>());
        }
    }
    m_pipes.resize(config.peer_count * config.peer_count);
    m_lost.assign(config.peer_count, false);
}

Result<std::unique_ptr<LoopbackHub>> LoopbackHub::create(const LoopbackConfig& config) {
    if (config.peer_count < 2) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "a link needs at least two ends; one peer is a solo run and "
                                     "needs no link at all"));
    }
    if (config.peer_count > kMaxPeers) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange,
                  std::format("{} peers is past the limit of {}", config.peer_count, kMaxPeers)));
    }
    return std::unique_ptr<LoopbackHub>(new LoopbackHub(config));
}

LoopbackHub::Pipe& LoopbackHub::pipe(std::size_t from, std::size_t to) {
    return m_pipes[(from * m_peers.size()) + to];
}

Status LoopbackHub::arm_fault(std::size_t from, std::size_t to, LinkFault fault) {
    if (from >= m_peers.size() || to >= m_peers.size()) {
        return std::unexpected(Error(ErrorCode::OutOfRange, "no such peer"));
    }
    pipe(from, to).armed = fault;
    return {};
}

void LoopbackHub::release_held() noexcept {
    for (Pipe& p : m_pipes) {
        for (Pending& pending : p.in_flight) {
            pending.held = false;
        }
    }
}

std::uint64_t LoopbackHub::polls(std::size_t peer) const noexcept {
    return peer < m_peers.size() ? m_peers[peer].polls : 0;
}

bool LoopbackHub::lost(std::size_t peer) const noexcept {
    return peer < m_lost.size() && m_lost[peer];
}

Status LoopbackHub::lose(std::size_t peer) {
    if (peer >= m_peers.size()) {
        return std::unexpected(Error(ErrorCode::OutOfRange, "no such peer"));
    }
    m_lost[peer] = true;
    for (std::size_t other = 0; other < m_peers.size(); ++other) {
        if (other != peer) {
            pipe(peer, other).in_flight.clear();
            pipe(other, peer).in_flight.clear();
        }
    }
    return {};
}

CommandInbox& LoopbackHub::inbox(std::size_t peer, std::size_t from) {
    return *m_peers[peer].inboxes[from];
}

std::size_t LoopbackHub::held_count() const noexcept {
    std::size_t held = 0;
    for (const Pipe& p : m_pipes) {
        held += static_cast<std::size_t>(std::ranges::count_if(
            p.in_flight, [](const Pending& pending) { return pending.held; }));
    }
    return held;
}

Status LoopbackHub::send(std::size_t from, std::size_t to, std::span<const std::byte> message) {
    if (from >= m_peers.size() || to >= m_peers.size()) {
        return std::unexpected(Error(ErrorCode::OutOfRange, "no such peer"));
    }
    if (from == to) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "a peer does not send to itself: it has its own turn "
                                     "already and needs no link to learn of it"));
    }

    if (m_config.topology == Topology::Star && from != 0 && to != 0) {
        // As on the socket hub: no connection between two peers other than zero, so a message
        // for one of them alone would have to be forwarded by a relay that does not keep it.
        return std::unexpected(Error(
            ErrorCode::InvalidArgument,
            std::format("on a star, peer {} reaches peer {} only by broadcast, through peer zero",
                        from, to)));
    }
    if (m_lost[to]) {
        return std::unexpected(Error(ErrorCode::Unavailable, "that peer has gone"));
    }
    if (m_lost[from]) {
        // A lost peer is a process that died: what it "sends" goes nowhere.
        return {};
    }
    enqueue(from, to, {message.begin(), message.end()}, from, false);
    return {};
}

Status LoopbackHub::broadcast(std::size_t from, std::span<const std::byte> message) {
    if (m_config.topology == Topology::Mesh) {
        return Link::broadcast(from, message);
    }
    if (from >= m_peers.size()) {
        return std::unexpected(Error(ErrorCode::OutOfRange, "no such peer"));
    }
    if (m_lost[from]) {
        return {};
    }
    if (from != 0) {
        // Once, to the relay, marked for everyone. Forwarded when peer zero receives it.
        enqueue(from, 0, {message.begin(), message.end()}, from, true);
        return {};
    }
    for (std::size_t to = 1; to < m_peers.size(); ++to) {
        if (m_lost[to]) {
            continue;
        }
        enqueue(0, to, {message.begin(), message.end()}, 0, false);
    }
    return {};
}

void LoopbackHub::enqueue(std::size_t from, std::size_t to, std::vector<std::byte> bytes,
                          std::size_t origin, bool everyone) {
    ++m_stats.sent;
    Pipe& p = pipe(from, to);

    Pending pending;
    pending.bytes = std::move(bytes);
    // Counted in the receiver's polls, because that is the thing a stalled peer still does.
    pending.release_at_poll = m_peers[to].polls + m_config.latency_polls;
    pending.order = m_next_order++;
    pending.origin = origin;
    pending.everyone = everyone;

    if (p.armed.has_value()) {
        const LinkFault fault = *p.armed;
        p.armed.reset();
        switch (fault) {
        case LinkFault::Drop: ++m_stats.dropped; return;
        case LinkFault::Corrupt:
            if (pending.bytes.size() > kCorruptionFloor) {
                // The **last** byte, which in a turn carrying commands is inside a payload.
                //
                // Corrupting near the front hits the tick instead, and a turn for a different
                // tick is a valid message that marks the wrong turn — so the session stalls
                // rather than noticing, which is a correct outcome but not the interesting one.
                // A damaged payload is applied by the sender and refused or applied differently
                // by the receiver, which is precisely the divergence the hash checks exist to
                // catch. The header stays intact either way, so the message still claims to be
                // what it is rather than being thrown out as not an Atlas message at all.
                pending.bytes.back() ^= std::byte{0x01};
                ++m_stats.corrupted;
            }
            break;
        case LinkFault::Hold:
            pending.held = true;
            ++m_stats.held;
            break;
        }
    }

    p.in_flight.push_back(std::move(pending));
}

void LoopbackHub::pump(std::size_t peer) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT_MSG(peer < m_peers.size(), "no such peer");

    ++m_peers[peer].polls;
    const std::uint64_t now = m_peers[peer].polls;
    if (m_lost[peer]) {
        return;
    }

    for (std::size_t from = 0; from < m_peers.size(); ++from) {
        if (from == peer) {
            continue;
        }
        Pipe& p = pipe(from, peer);

        // Everything due and not parked, taken out in one pass so that a permutation applies to
        // the whole of one poll's worth rather than to a sliding window of it.
        std::vector<Pending> releasable;
        for (auto it = p.in_flight.begin(); it != p.in_flight.end();) {
            if (!it->held && it->release_at_poll < now) {
                releasable.push_back(std::move(*it));
                it = p.in_flight.erase(it);
            } else {
                ++it;
            }
        }
        if (releasable.empty()) {
            continue;
        }

        if (m_config.reorder && releasable.size() > 1) {
            // Keyed on the receiver's poll index, so the permutation is a pure function of the
            // seed and the poll rather than of how much traffic anything else has carried.
            sim::RngStream stream(m_config.reorder_seed, sim::stream_id("loopback reorder"), now);
            for (std::size_t i = releasable.size(); i > 1; --i) {
                const auto j = static_cast<std::size_t>(stream.next_below(i));
                if (j != i - 1) {
                    std::swap(releasable[i - 1], releasable[j]);
                    ++m_stats.reordered;
                }
            }
        }

        for (Pending& pending : releasable) {
            // **Filed and forwarded in the same step**, as the socket hub's listener does
            // (ADR-0022 D1): nothing reaches another peer that peer zero has not filed itself.
            if (pending.everyone && peer == 0) {
                for (std::size_t to = 1; to < m_peers.size(); ++to) {
                    if (to != pending.origin && !m_lost[to]) {
                        enqueue(0, to, pending.bytes, pending.origin, false);
                    }
                }
            }
            if (m_peers[peer].inboxes[pending.origin]->push(std::move(pending.bytes)) ==
                CommandInbox::Push::Accepted) {
                ++m_stats.delivered;
            } else {
                ++m_stats.refused;
            }
        }
    }
}

}  // namespace atlas::net
