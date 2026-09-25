// SPDX-License-Identifier: GPL-3.0-or-later
//
// The protocol's constants have no behaviour of their own yet. This translation unit exists so
// the module has something to compile and link before its first real source arrives, which is
// what lets the module boundary, the diagram and the test target be proved green on their own
// rather than alongside the code they are meant to constrain.

#include <atlas/net/protocol.hpp>
#include <atlas/simulation/command.hpp>

namespace atlas::net {

static_assert(kProtocolVersion == 3, "bumping the protocol version is a decision, not an edit");

// A message must be able to hold a turn carrying the largest payload the command queue accepts,
// or the two limits disagree and a legal command becomes unsendable.
static_assert(kMaxMessageBytes > sim::CommandQueue::kMaxPayload,
              "one command must fit in one message");

std::string_view to_string(ByeReason reason) noexcept {
    switch (reason) {
    case ByeReason::Quit: return "quit";
    case ByeReason::ProtocolError: return "protocol error";
    case ByeReason::Diverged: return "diverged";
    case ByeReason::Overflow: return "overflow";
    case ByeReason::VersionMismatch: return "version mismatch";
    }
    // A value the decoder refuses, so this is only reachable through a hand-built enumerator.
    return "unknown";
}

}  // namespace atlas::net
