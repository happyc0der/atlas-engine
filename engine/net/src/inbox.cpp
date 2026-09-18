// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/net/inbox.hpp>

namespace atlas::net {

CommandInbox::Push CommandInbox::push(std::vector<std::byte> message) {
    const std::scoped_lock lock{m_mutex};

    // Sticky, and checked first. An inbox that has already lost a message is not a complete
    // record of what its peer sent, and accepting more would produce a stream with a hole in it
    // that looks exactly like a stream without one.
    if (m_overflowed) {
        ++m_refused;
        return Push::Overflowed;
    }
    if (m_pending.size() >= kMaxMessages || m_queued_bytes + message.size() > kMaxBytes) {
        m_overflowed = true;
        ++m_refused;
        return Push::Overflowed;
    }

    m_queued_bytes += message.size();
    m_pending.push_back(std::move(message));
    ++m_accepted;
    return Push::Accepted;
}

void CommandInbox::drain(std::vector<std::vector<std::byte>>& out) {
    ATLAS_ASSERT_MAIN_THREAD();
    out.clear();
    {
        const std::scoped_lock lock{m_mutex};
        out.swap(m_pending);
        m_queued_bytes = 0;
    }
    // The overflow flag is deliberately not cleared here. Draining makes room; it does not make
    // the messages that were refused come back.
}

bool CommandInbox::overflowed() const noexcept {
    const std::scoped_lock lock{m_mutex};
    return m_overflowed;
}

std::size_t CommandInbox::depth() const noexcept {
    const std::scoped_lock lock{m_mutex};
    return m_pending.size();
}

std::size_t CommandInbox::queued_bytes() const noexcept {
    const std::scoped_lock lock{m_mutex};
    return m_queued_bytes;
}

std::uint64_t CommandInbox::accepted() const noexcept {
    const std::scoped_lock lock{m_mutex};
    return m_accepted;
}

std::uint64_t CommandInbox::refused() const noexcept {
    const std::scoped_lock lock{m_mutex};
    return m_refused;
}

}  // namespace atlas::net
