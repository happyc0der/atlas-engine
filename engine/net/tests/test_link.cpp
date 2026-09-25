// SPDX-License-Identifier: GPL-3.0-or-later
// The seam, proved against a backend that is not the loopback.
//
// **An interface with one implementation is a claim, not a seam.** Until M17 `LinkEnd` held a
// `LoopbackHub*` and every test drove it through that one hub, so nothing could tell whether
// the operations it exposes were general or merely what the in-memory link happened to offer.
// This file is the cheapest possible second implementation — it records what it was asked to do
// and delivers nothing — and its whole job is to fail if `LinkEnd` ever reaches past `Link` for
// something only the loopback has.
#include <atlas/core/assert.hpp>
#include <atlas/net/link.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using atlas::net::CommandInbox;
using atlas::net::Link;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// A link that carries nothing and remembers everything.
class RecordingLink final : public Link {
  public:
    explicit RecordingLink(std::size_t peers) : m_peers(peers) {
        m_inboxes.resize(peers * peers);
        for (auto& inbox : m_inboxes) {
            inbox = std::make_unique<CommandInbox>();
        }
    }

    [[nodiscard]] std::size_t peer_count() const noexcept override { return m_peers; }

    [[nodiscard]] atlas::net::Topology topology() const noexcept override {
        return atlas::net::Topology::Mesh;
    }

    [[nodiscard]] atlas::Status send(std::size_t from, std::size_t to,
                                     std::span<const std::byte> message) override {
        if (from >= m_peers || to >= m_peers || from == to) {
            return std::unexpected(
                atlas::Error(atlas::ErrorCode::OutOfRange, "no such peer, or sending to oneself"));
        }
        // A nominated destination that refuses, so `broadcast`'s error path can be reached at
        // all. Without it the only way to fail a send is an argument `broadcast` never
        // produces, which is how the first version of this file let a mutation through.
        if (refuse_to.has_value() && to == *refuse_to) {
            return std::unexpected(
                atlas::Error(atlas::ErrorCode::Unavailable, "refused on purpose"));
        }
        sends.push_back({from, to, message.size()});
        return {};
    }

    void pump(std::size_t peer) override { pumps.push_back(peer); }

    [[nodiscard]] CommandInbox& inbox(std::size_t peer, std::size_t from) override {
        return *m_inboxes[(peer * m_peers) + from];
    }

    struct Sent {
        std::size_t from = 0;
        std::size_t to = 0;
        std::size_t bytes = 0;
    };

    std::vector<Sent> sends;
    std::vector<std::size_t> pumps;
    std::optional<std::size_t> refuse_to;

  private:
    std::size_t m_peers;
    std::vector<std::unique_ptr<CommandInbox>> m_inboxes;
};

[[nodiscard]] std::vector<std::byte> bytes_of(std::size_t count) {
    return std::vector<std::byte>(count, std::byte{7});
}

}  // namespace

TEST_CASE("an end routes every call to its link, stamped with its own index", "[net][link]") {
    REQUIRE(kMainThreadMarked);

    RecordingLink link{3};
    auto end = link.end(1);

    CHECK(end.index() == 1);
    CHECK(end.peer_count() == 3);

    REQUIRE(end.send_to(2, bytes_of(16)).has_value());
    REQUIRE(link.sends.size() == 1);

    // The end supplies the sender. Nothing above `LinkEnd` gets to say who it is, which is the
    // same property `atlas_submit` has for a mod and for the same reason.
    CHECK(link.sends[0].from == 1);
    CHECK(link.sends[0].to == 2);
    CHECK(link.sends[0].bytes == 16);

    end.pump();
    REQUIRE(link.pumps.size() == 1);
    CHECK(link.pumps[0] == 1);
}

TEST_CASE("broadcast is a loop over send, and skips the sender", "[net][link]") {
    RecordingLink link{4};
    auto end = link.end(2);

    REQUIRE(end.broadcast(bytes_of(8)).has_value());

    // Three sends for four peers, and none of them to itself: a peer that needs its own turn
    // has it already. Written once on `LinkEnd` rather than once per backend, which is why
    // `Link` has four calls and not five.
    REQUIRE(link.sends.size() == 3);
    std::vector<std::size_t> destinations;
    for (const auto& sent : link.sends) {
        CHECK(sent.from == 2);
        destinations.push_back(sent.to);
    }
    CHECK(destinations == std::vector<std::size_t>{0, 1, 3});
}

TEST_CASE("send_to refuses an index that is not a peer, and refuses oneself", "[net][link]") {
    RecordingLink link{2};
    auto end = link.end(0);

    CHECK_FALSE(end.send_to(5, bytes_of(4)).has_value());
    CHECK_FALSE(end.send_to(0, bytes_of(4)).has_value());
    CHECK(link.sends.empty());
}

TEST_CASE("a broadcast stops at the first refusal and reports it", "[net][link]") {
    // **The link is made to refuse, rather than the arguments made invalid.** An earlier version
    // of this case drove `send_to` with a bad index and checked only that it failed, which says
    // nothing about `broadcast` -- a `broadcast` that discarded every status would have passed
    // it. A mutation that dropped the status check survived, which is what sent this back.
    RecordingLink link{4};
    link.refuse_to = 2;
    auto end = link.end(0);

    const auto status = end.broadcast(bytes_of(8));
    REQUIRE_FALSE(status.has_value());

    // Peer 1 was reached and peer 3 was not: it stops rather than carrying on and reporting the
    // last outcome. Under lockstep a partially delivered turn is worse than none, because the
    // peers that got it run a tick the others never will.
    REQUIRE(link.sends.size() == 1);
    CHECK(link.sends[0].to == 1);
}

TEST_CASE("an end reaches the mailbox belonging to the pair", "[net][link]") {
    RecordingLink link{3};
    auto first = link.end(0);
    auto second = link.end(1);

    // One inbox per (receiver, sender) pair, so a peer flooding one starves nobody else's. The
    // addresses differing is what says the indices are not being swapped somewhere.
    CHECK(&first.inbox(1) != &first.inbox(2));
    CHECK(&first.inbox(1) != &second.inbox(0));
    CHECK(&first.inbox(1) == &link.inbox(0, 1));
}

TEST_CASE("ends are handles, and copying one costs nothing it should not", "[net][link]") {
    RecordingLink link{2};
    auto original = link.end(0);
    auto copy = original;  // NOLINT(performance-unnecessary-copy-initialization)

    // `Session` takes a `LinkEnd` by value and always has. What must outlive it is the link
    // behind the handle, which is the sentence M17 corrected in session.hpp.
    CHECK(copy.index() == original.index());
    REQUIRE(copy.send_to(1, bytes_of(2)).has_value());
    REQUIRE(original.send_to(1, bytes_of(2)).has_value());
    CHECK(link.sends.size() == 2);
    CHECK(&copy.inbox(1) == &original.inbox(1));
}
