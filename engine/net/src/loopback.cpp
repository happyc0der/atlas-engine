// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/net/loopback.hpp>
#include <atlas/simulation/rng.hpp>

#include <algorithm>
#include <format>

namespace atlas::net {
namespace {

/// Where a corrupting bit flip lands.
///
/// Past the sixteen-byte header, so the message still claims to be an Atlas message of a known
/// type and the damage has to be found by decoding the body or by the hashes disagreeing. A
/// flip inside the magic would be refused before anything interesting happened, which would
/// make the fault a test of the magic check rather than of the session.
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

LinkEnd LoopbackHub::end(std::size_t peer) {
    ATLAS_ASSERT_MSG(peer < m_peers.size(), "no such peer");
    return {*this, peer};
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

    ++m_stats.sent;
    Pipe& p = pipe(from, to);

    Pending pending;
    pending.bytes.assign(message.begin(), message.end());
    // Counted in the receiver's polls, because that is the thing a stalled peer still does.
    pending.release_at_poll = m_peers[to].polls + m_config.latency_polls;
    pending.order = m_next_order++;

    if (p.armed.has_value()) {
        const LinkFault fault = *p.armed;
        p.armed.reset();
        switch (fault) {
        case LinkFault::Drop: ++m_stats.dropped; return {};
        case LinkFault::Corrupt:
            if (pending.bytes.size() > kCorruptionFloor) {
                pending.bytes[kCorruptionFloor] ^= std::byte{0x01};
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
    return {};
}

void LoopbackHub::pump(std::size_t peer) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT_MSG(peer < m_peers.size(), "no such peer");

    ++m_peers[peer].polls;
    const std::uint64_t now = m_peers[peer].polls;

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
            const std::size_t bytes = pending.bytes.size();
            if (m_peers[peer].inboxes[from]->push(std::move(pending.bytes)) ==
                CommandInbox::Push::Accepted) {
                ++m_stats.delivered;
            } else {
                ++m_stats.refused;
            }
            (void)bytes;
        }
    }
}

Status LinkEnd::send_to(std::size_t peer, std::span<const std::byte> message) {
    return m_hub->send(m_index, peer, message);
}

Status LinkEnd::broadcast(std::span<const std::byte> message) {
    for (std::size_t peer = 0; peer < m_hub->peer_count(); ++peer) {
        if (peer == m_index) {
            continue;
        }
        if (auto status = m_hub->send(m_index, peer, message); !status) {
            return status;
        }
    }
    return {};
}

void LinkEnd::pump() {
    m_hub->pump(m_index);
}

CommandInbox& LinkEnd::inbox(std::size_t peer) {
    ATLAS_ASSERT_MSG(peer < m_hub->peer_count(), "no such peer");
    return *m_hub->m_peers[m_index].inboxes[peer];
}

std::size_t LinkEnd::peer_count() const noexcept {
    return m_hub->peer_count();
}

std::uint64_t LinkEnd::polls() const noexcept {
    return m_hub->m_peers[m_index].polls;
}

}  // namespace atlas::net
