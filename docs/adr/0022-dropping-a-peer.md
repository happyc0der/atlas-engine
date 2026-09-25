<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0022 — A peer may be dropped, at a tick the relay decides

## Status

**Proposed**, 2026-09-25, written at M25's gate. Accepted when M25 lands it. Nothing in
`engine/net` changes before this record is read.

Amends [ADR-0017](0017-lockstep-transport.md) in two places and supersedes nothing. D5's rule
that a quiet peer ends the session **stays the default**; this record adds a second policy an
application may choose. D2's socket hub gains a relay, which it turns out to have needed all
along (see Context). [ADR-0014](0014-deterministic-lockstep.md)'s invariant is untouched: the
gate still reads no clock, and readiness still depends only on who has reported.

## Context

`docs/DEFERRED.md` recorded dropping a peer and playing on, with its trigger: *"a consumer for
whom one person's connection ending everyone's session is worse than the risk of getting that
agreement wrong."* **That trigger has not fired, and this record says so first.** No consumer in
this repository has a session worth saving from one person's network: the lab's runs are
demonstrations, and chess has two players, so a drop leaves one person playing nobody. The owner
chose on 2026-09-24 to finish the three items M17 left open, in order, and this is the third.
What is built here is built on that decision, against the lab, as M17 and M24 were.

### What the deferral said the hard part was

A drop is simulation-visible. The dropped peer's commands stop arriving, so every remaining peer
must stop expecting them **at the identical tick**, or one peer runs a tick with the dropped
peer's commands that another runs without, and they diverge. The deferral named the pieces of a
protocol for that: a declaration, an acknowledgement, and a rule for when the declaration is
itself lost.

### What the survey found, which changes the problem

**A socket session cannot have more than two peers today, although the lab accepts sixteen.**
The hub is a star: the listener holds a connection to every connector, and a connector holds a
connection to the listener and nobody else (`enet_hub.cpp`, where a connector sets `peers[0]` and
nothing more). `Session` broadcasts every turn to every peer, so a connector's send to any other
connector fails. Run on 2026-09-25, three processes of the lab with `--listen 0 --expect 3` and
two `--connect`:

```
peer 2: atlas_lab: announcing this peer: that peer has gone [Unavailable]
peer 0: atlas_lab: peer 2 disconnected [Unavailable]
peer 1: atlas_lab: peer 0 disconnected [Unavailable]
```

All three exit 1 before the first tick. Peer 1 also logs itself *"socket peer 1 of 2"*, because a
connector infers the session's size from its own index. `--expect` has accepted 2 to 16 since
M17, and no case has ever started three processes. The loopback, where every peer is one process
and the hub a full mesh, runs three peers correctly, which is how this went unseen.

**So a peer drop with sockets is only meaningful after a relay exists**, because with two peers
there is nobody left to agree with. And the relay turns out to answer the agreement question on
its own, which is the decision below.

## Decision

**D1. The listener relays.** A connector sends everything to the listener. The listener forwards
each message it receives from one connector to every other connector, on the same reliable,
ordered channel it uses for its own messages, wrapped in a transport header naming the origin.
A connector files a relayed message in the inbox of its origin, so the session above sees the
same thing it sees on the loopback: a message from peer *k* in peer *k*'s inbox. The listener's
index message gains the session's size, so a connector knows how many peers there are rather
than guessing from its own index.

The header is transport framing, like the index message already is, not a `net::Message`: the
session owns messages and the transport owns how they travel (ADR-0017 D6). Nothing about a turn
changes, and a turn from a connector reaches every other peer exactly once, in the order the
listener received it.

**D2. The listener decides a drop, and the relay's order is the agreement.** On the listener,
when the transport reports that connector *k* has gone — silent past the deadline, or
disconnected — the session:

1. reads everything *k* sent that is already in its inbox, which the transport has already
   forwarded;
2. stops accepting and forwarding anything from *k*;
3. takes **H**, the highest tick for which it holds *k*'s turn;
4. sends `Drop{source = k, last_tick = H}` to every remaining connector;
5. removes *k* from its own gate's expectation set.

A connector receiving `Drop{k, H}` checks that it holds *k*'s turns through H, then removes *k*
from its expectation set.

**This needs no acknowledgement, and the reason is the whole design.** Every turn *k* ever sent
to another connector went through the listener, in order, on one channel. So when a connector
reads the `Drop`, it has already read every turn of *k*'s that the listener forwarded, which is
every turn through H and none after. No remaining peer can hold a turn of *k*'s past H, because
nobody but the listener received one to forward. And no remaining peer can have run a tick past
H, because running tick H + 1 needed *k*'s turn for it. Every peer therefore runs ticks up to H
with *k*'s commands and every tick after H without them. They agree by construction, not by
negotiating.

The deferral's third piece, **a declaration that is lost, cannot happen separately**. The `Drop`
travels the same reliable ordered channel as everything else, so if it does not arrive, the
connection to the listener has failed. A connector that loses the listener ends the session, as
it does today.

**D3. The gate needs nothing new.** `TurnGate::expect_sources` already replaces the set,
**keeping the progress** of every source that stays and forgetting one that leaves (ADR-0014).
Removing *k* after its turns through H are marked leaves ticks through H exactly as ready as
before and makes later ticks independent of *k*. The gate still learns about peers only through
`expect_sources`, and still reads no clock: the one deadline stays in the transport, as ADR-0017
D5 put it.

**D4. Dropping is a policy the application chooses; ending stays the default.**
`SessionConfig` gains `on_peer_lost`, with `End` as the default and `Drop` as the alternative.
Under `End`, nothing changes from today, including for chess. The lab gains `--drop-lost-peers`.
A drop is reported to the application in `PollReport`, naming the source and H, because what a
drop *means* is the application's. A game might hand the dropped player's side to nobody, or end
the game; the engine has no game state to decide with.

**D5. The listener is the one peer that cannot be dropped.** If the listener goes, every
connector loses the relay, and the session ends exactly as it does today. This makes peer zero
special, which [ADR-0020](0020-session-finish.md) rejected for finishing, in a design where no
peer was special. **The relay already made it special**: in a star, every connector's only route
is the listener. This record names the consequence rather than hiding it.

**D6. A dropped peer is told, if it can be.** The listener sends *k* a `Bye{Dropped}` before
closing the connection, so a peer that was only slow ends with its own reason rather than a bare
disconnect, and exits non-zero. A peer that has gone hears nothing, and nothing depends on it
hearing.

**D7. What the session no longer waits for.** Hash checks from *k* after H are not expected, and
the finish condition (ADR-0020 D3) counts only the peers still in the session. A dropped peer
never rejoins. Joining a session under way is refused today, and a rejoin would need the world
sent over the wire as a save, which nothing in Atlas does.

**D8. Protocol version 3.** A new message, `Drop{source, last_tick}`, as type 7. A new goodbye
reason, `Dropped`. The relay header and the size in the index message are transport framing and
change together with the version. A peer on version 2 is refused at the handshake, as every
version change has been.

**D9. The loopback can be a star.** The in-memory hub gains a relaying mode that routes as the
socket hub now does, plus a fault that makes one peer go silent. The drop protocol can then be
tested under the loopback's existing latency, reordering and holds, in one process, under the
sanitizers, and at every tick. The default loopback stays a full mesh, so every existing test is
unchanged.

## Alternatives

**Cap sockets at two peers, and drop only in a pair.** The smallest honest fix to the survey's
finding: `--expect` refuses anything above two, and a drop means the listener plays on alone. It
needs no relay and no header. Rejected as the recommendation because a drop between two peers
agrees with nobody, so it would test none of the protocol the deferral worried about. **It is
the right fallback if the owner declines the relay**, and the three-peer failure must be fixed
one way or the other either way: a flag that accepts sixteen and works for two is a bug, not a
limit.

**A full mesh, every connector connected to every other.** It removes the listener as a single
point of failure and as a trusted relay. Rejected: it needs connectors to reach each other
directly, which is exactly the NAT traversal ADR-0017 D4 deferred. And it brings back the
agreement problem in full. A connector could receive *k*'s turn for H + 1 directly, run H + 1,
and then disagree with a peer that never received it. Agreeing then needs every peer to report
what it holds, and a rule for forwarding turns that only some peers received: the protocol the
deferral feared.

**Agreement by acknowledgement round.** The listener proposes H, every connector confirms, and
the drop applies on the last confirmation. Rejected because the relay makes the round
unnecessary, as D2 shows, and a round would add the lost-confirmation case back.

**A drop as a command in the simulation.** The listener submits a `peer_dropped` command stamped
for tick H + 1, so the drop is hashed and recorded. Rejected for the engine: it would make `net`
register a command type, which is game-shaped, and the gate change must happen outside the
simulation anyway. An application that wants the drop in its world can submit its own command
from the `PollReport` on every peer, the way every peer's mod produces the same commands itself.

**Resynchronising a dropped peer, or letting it rejoin.** Out of scope, as D7 says.

## Consequences

- `engine/net`: the socket hub relays, frames relayed messages with their origin, and tells a
  connector the session's size. The loopback gains a star mode and a silence fault. `Session`
  gains the drop decision on the listener, the `Drop` message on connectors, `on_peer_lost`, and
  a `PollReport` field. `protocol.hpp` gains the type, the reason and version 3.
- `net`'s module edges stay `core;simulation`, and no socket type reaches a header.
- **The listener becomes trusted.** It can forge any connector's messages, because it relays
  them. There was no authentication before either (ADR-0017 D4 deferred encryption), and in a
  pair the listener already delivered the only messages the connector read. With three peers it
  is a real widening, and it is recorded rather than implied.
- **Bandwidth at the listener grows** with the square of the session's size, because it forwards
  every connector's turns to every other. At the lab's sizes this is kilobytes a second. It is
  measured against a prediction written first, in `bench_net`.
- The lab's three-process case becomes possible, and becomes a test: three peers agree over
  sockets with nobody dropped, which **fails on `main` today**.
- The whole-program proof: three processes, one killed mid-run, the other two finish at the same
  tick with equal hashes and both name the drop and its tick. The anti-vacuity half runs the same
  kill without `--drop-lost-peers` and requires the session to end, as ADR-0017 D5 does today.
- `CLAUDE.md`'s lockstep rule, which says dropping a peer is deferred, is rewritten to name this
  decision. `DETERMINISM.md`'s paragraph saying a drop would break the design gains a dated note
  explaining why this one does not. `DEFERRED.md`'s entry is struck, and rejoining is opened with
  its trigger.
- **The risk is D2's argument**, and it rests on one property: nothing of *k*'s reaches another
  connector except through the listener, in order. Every mutation of that property gets a test
  and must fail it: forwarding after the decision, sending the `Drop` before the forwarded turns,
  declaring H − 1 or H + 1, and a connector accepting a `Drop` for turns it has not received.

## Rollback cost

Moderate. The relay replaces a latent failure with a working three-peer session, and removing it
would bring the failure back, which the alternative above would then have to fix another way.
The drop policy is additive and off by default. Removing it deletes a message type, a goodbye
reason and a branch in the session, and costs one protocol version. Nothing is serialised to a
file, so no save or replay format is spent.
