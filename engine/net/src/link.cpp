// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/net/link.hpp>

namespace atlas::net {

LinkEnd Link::end(std::size_t peer) {
    ATLAS_ASSERT_MSG(peer < peer_count(), "no such peer");
    return {*this, peer};
}

Status LinkEnd::send_to(std::size_t peer, std::span<const std::byte> message) {
    return m_link->send(m_index, peer, message);
}

Status LinkEnd::broadcast(std::span<const std::byte> message) {
    // A loop over `send` rather than a method every backend implements. Broadcasting means the
    // same thing on every link there could be, so writing it once is the whole reason `Link`
    // has four calls instead of five.
    for (std::size_t peer = 0; peer < m_link->peer_count(); ++peer) {
        if (peer == m_index) {
            continue;
        }
        if (auto status = m_link->send(m_index, peer, message); !status) {
            return status;
        }
    }
    return {};
}

void LinkEnd::pump() {
    m_link->pump(m_index);
}

CommandInbox& LinkEnd::inbox(std::size_t peer) {
    ATLAS_ASSERT_MSG(peer < m_link->peer_count(), "no such peer");
    return m_link->inbox(m_index, peer);
}

std::size_t LinkEnd::peer_count() const noexcept {
    return m_link->peer_count();
}

}  // namespace atlas::net
