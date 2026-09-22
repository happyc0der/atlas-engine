<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0017 — A transport for lockstep: ENet, polled, direct address only

## Status

**Proposed**, 2026-09-20. M17 builds it; this record becomes Accepted when it is in force, as
the legend in this index requires.

Answers the five criteria [ADR-0014](0014-deterministic-lockstep.md) decision 9 recorded rather
than resolved, and **supersedes that decision in part**: 0014 says *"No transport is chosen here
and none is added. Nothing reaches `vcpkg.json`."* One is chosen here and one is added. Every
other decision in 0014 is inherited unchanged, and the one this milestone most depends on —
readiness depends on who has reported and never on elapsed time — is strengthened rather than
weakened by what follows.

**This record numbers 0017 and not 0016.** The M17 plan was written on 2026-09-19 expecting
0016; M16's string-table record landed first and took it. Numbers follow landing order.

## Context

### What ADR-0014 asked, and what answers it now

Its decision 9, verbatim:

> **9. No transport, and the criteria for choosing one are recorded** rather than resolved: a
> reliable ordered channel, because lockstep tolerates no loss and raw UDP would mean writing
> reliability; Windows support in the pinned baseline; licence compatibility with GPL-3.0;
> whether encryption and NAT traversal are wanted; and whether it may own a thread of its own.

Two of the five were the owner's to answer and were answered on 2026-09-19: **no encryption and
no NAT traversal**, and **no thread of its own**. The remaining three are questions of evidence,
and the evidence turns out to settle them more sharply than the planning conversation expected —
two of the three candidates are eliminated by criteria this project wrote down before it knew
which candidates existed, which is the strongest kind of criterion to have.

### The consumer, stated plainly

`docs/DEFERRED.md` records the transport as *"Picked up when two machines need to play, which no
consumer needs today"*, and nothing about that has changed. M16 was the milestone that taught
what it costs to inherit a justification without checking it, so this record does not.

What differs here is that **the proof is the consumer**. ADR-0014 designed lockstep against an
in-memory link, and an in-memory link cannot show what a design does when a packet is late, a
process dies, or a connection is refused. Two processes agreeing hash for hash over a socket is
the only thing that can, and M14's own loopback was held to exactly that standard. That is a
weaker consumer than a game and a stronger one than nothing, and the difference is worth naming
rather than glossing.

## Decision

**1. ENet 1.3.18, from the stock port, linked privately to `net`.** Reliable-ordered delivery
over UDP, which is criterion 1 without writing reliability. No `supports` expression and no
entry in vcpkg's own `ci.baseline.txt`, which is criterion 2. Two host-only build tools and
**no runtime dependencies at all** — the transitive closure is empty.

**This is the only candidate that costs no module-graph edge.** ENet links as a `PRIVATE_DEPS`
on `net` exactly as SDL3 links to `audio`; `ATLAS_MODULE_DEPS_net` stays `core;simulation`, the
mermaid diagram is untouched, and no CLAUDE.md invariant moves. That is the clearest single
measure of what this choice buys, and it is why the Files list of M17's plan says which files
are deliberately *not* changed.

**2. The licence is MIT, and this record holds the evidence rather than the claim.** The port
manifest has **no `license` field**; `portfile.cmake` defers to the upstream `LICENSE`, and no
ENet tarball exists in this checkout's `downloads/`. So the assertion "ENet is MIT" was, until
this record, general knowledge and not something this repository could check.

Verified on 2026-09-20 by fetching the exact archive the port pins,
`https://github.com/lsalzman/enet/archive/v1.3.18.tar.gz`, and confirming its SHA512 **equals
the value in `external/vcpkg/ports/enet/portfile.cmake`** — so the bytes read are the bytes
vcpkg would build:

```
a0d2fa8c957704dd49e00a726284ac5ca034b50b00d2b20a94fa1bbfbb80841467834bfdc84aa0ed0d6aab894608fd6c86c3b94eee46343f0e6d9c22e391dbf9
```

`LICENSE` inside that archive is the standard MIT text, `Copyright (c) 2002-2024 Lee Salzman`,
SHA-256 `a29cd246638974da7c6edf423a0a69d4a6f7d759940b7cfa9a2bc3442ac3834c`. MIT into GPL-3.0 is
compatible one way, the same footing as SPIRV-Cross and WAMR. Criterion 3, met on evidence.

**A dependency whose licence this repository has not read does not enter a GPL-3.0 project.**
That is the rule this decision establishes, and the cost of following it was ten minutes.

**3. Polled, on the main thread, with no thread of its own.** Criterion 5, answered by the
owner. `pump()` drains ENet's event queue with a zero timeout during the poll that already
happens once a frame, and pushes whole packets into the same per-sender inboxes the loopback
fills — so everything downstream of the inbox is unchanged and untested-by-this-milestone in the
good sense.

The consequence worth recording is a document that stays true: `docs/ARCHITECTURE.md`'s *"**No
new thread.** The loopback link and every session run on the main thread, so the threading table
above is unchanged"* survives this milestone **unedited**, and the threading table gains no
`net` row. A transport that owned a thread would have added one, and with it a row of rules
about what may not allocate, log or assert on it.

**4. Direct address only.** Connect by host and port. Deferred, each with a trigger:
encryption (a session between people who do not trust the network between them); NAT traversal
(two peers who cannot reach each other directly, which is most of the internet and none of a
local network); a lobby or matchmaking (anything that needs to *find* a peer rather than be told
one); and a relay topology, which costs nothing later because a turn is addressed by `SourceId`
rather than by a socket.

**5. A peer that goes quiet ends the session.** This is the decision ADR-0014 deferred as *"a
transport policy"*, and the reason it is this one rather than the obvious alternative is worth
the space.

Dropping a peer and playing on sounds like what a session is for. But **a drop is
simulation-visible**: the dropped peer's commands stop arriving, so every remaining peer must
apply the drop at the *identical* tick or they compute different states and diverge. Agreeing on
that tick is a protocol of its own — a declaration, an acknowledgement, and a rule for what
happens when the declaration is itself lost. Getting it subtly wrong produces exactly the
failure lockstep exists to make impossible, and produces it rarely, on one person's bad network.

Ending the session needs no agreement, because every peer reaches the same conclusion from its
own timer independently, and the conclusion is the same one this project already reaches
everywhere else: a divergence stops the session, a late turn ends it, a lost device is never
recovered from. **A silent peer joins that list rather than opening a new category.**

The deadline lives in the transport. **The gate still reads no clock**, so ADR-0014's central
invariant is untouched — which is the test of whether this decision fits the design or merely
sits beside it.

Dropping-and-continuing is deferred with that reasoning in `docs/DEFERRED.md`. Trigger: a
consumer for whom one person's network ending everyone's session is worse than the risk of
getting the agreement wrong.

**6. `net::Link` is the seam, and `LinkEnd` keeps its shape.** `LinkEnd` stops holding a
`LoopbackHub*` and holds a `Link*`: an abstract backend with `send_to`, `broadcast`, `pump` and
`inbox`. `Session::create(LinkEnd, SessionConfig)` does not change.

A virtual `LinkEnd` would force `Session` to take a reference and change its signature; a
templated `Session` would have to stop being pimpl'd. This is the smallest change that makes the
seam real, and the measure of it is that **no existing test changes** — none of them names
`LinkEnd`, because they bind ends with `auto`.

**7. No framing, because ENet delivers packets whole.** `message.hpp` has said since M14 that
*"a length prefix that suits a stream socket is wrong for a datagram and vice versa. This module
owns the message, not the frame."* A datagram transport is what makes that sentence cost
nothing: there is no frame to write. This is the second reason to prefer ENet over a stream
socket, after the reliability one.

**8. The listener assigns indices, before the handshake.** The loopback hands out indices by
construction; over a socket somebody must decide, and both ends must agree before any `SourceId`
is stamped, because a `SourceId` reaches the replay and the hash. The listener waits for every
expected peer, assigns in connection order, and tells each one its index in the first message —
then ADR-0014's existing `Hello` exchange runs unchanged.

**9. The two bounds are one rule, and that landed before this record.** `kMaxMessageBytes` was
eight mebibytes against a `CommandInbox` budget of one, so a message between them was legal to
send and impossible to receive. M17's slice 0 brought the protocol bound down to the inbox's and
added a `static_assert` tying them. Recorded here because it is the wire contract changing, and
a wire contract changes in a record.

It also exposed a second inconsistency, fixed in prose rather than in numbers:
`kMaxCommandsPerTurn` times the command queue's payload ceiling permits a turn of roughly 256
MiB, so **the binding constraint on a turn is the message cap and not the command counter**.

**10. ENet's process-wide initialisation is owned by an RAII object, not hidden in the hub.**

This decision was missing when this record was first written on 2026-09-20 and is added here
rather than discovered in the implementation. `enet_initialize()` calls `WSAStartup` on Windows
and must be paired with `enet_deinitialize()`; it is process-wide state, and CLAUDE.md permits
exactly one such object — the log sink registry — with [ADR-0015](0015-sandboxed-mods.md)'s
`script::Runtime` already recorded as the second.

`net::EnetRuntime` is the third, and it takes the same shape for the same reasons: created once
by the composition root, at most one alive at a time, asserting rather than returning an error
on a second, because two of them is a programmer error and not a condition to recover from.

**The alternative — a reference-counted static inside `EnetHub` — is refused.** It would make
the initialisation invisible at every call site, which is precisely the "hidden global mutable
state" CLAUDE.md forbids, and it would tie the lifetime of Winsock to whichever hub happened to
be destroyed last. A socket library coming up and going down as a side effect of constructing a
session object is the kind of ordering nobody can debug when it goes wrong.

The cost is one more thing a composition root must remember, which is the same cost
`script::Runtime` and `platform::Platform` already impose, and is paid in the one place that
knows the whole program's lifetime.

## Alternatives

**SDL3_net.** Adds no package the project does not already have — its only dependency is
`sdl3`, pinned at 3.4.12 — and its headers are already covered by `check_module_deps.py`'s
`"SDL3_"` prefix. It looks like the cheapest option in the manifest and is the most expensive
where this project counts cost.

Rejected on two grounds, either of which alone is sufficient. **It is not a reliable ordered
channel**: the port describes itself as *"A simple, cross-platform wrapper over TCP/IP
sockets"*, so criterion 1 would be met only by choosing TCP and accepting head-of-line blocking,
which is precisely the pathology lockstep feels worst. And **it is the only candidate requiring
a `net → platform` edge**, which `cmake/ModuleGraph.cmake` forbids by name: *"It must never
depend on renderer, scene or platform: a peer exchanges commands and hashes, and nothing it does
is presentation."* That edge cascades into the module table, the mermaid diagram (a hard lint
failure, checked in three directions), `CLAUDE.md`'s SDL-types list, `CLAUDE.md`'s
"the platform brings SDL subsystems up" rule, and this record. The real price is not the edits:
it is that the one module in the graph which is pure state exchange would acquire the engine's
heaviest presentation dependency, in order to obtain a socket.

**GameNetworkingSockets.** Reliable-ordered, plus encryption and ICE NAT traversal, under
BSD-3-Clause. **Disqualified by criterion 2, which this project wrote down in ADR-0014 before
knowing this port existed**: its manifest carries `"supports": "!uwp & !(arm64 & windows)"`, and
Windows is a tier-one platform here. Independently it would take the manifest from seven
packages to eleven — openssl, protobuf, abseil and a host protoc build — for features decision 4
has decided not to want. Either reason would be enough; together they are not close.

**Raw UDP with reliability written here.** ADR-0014 named this and refused it in the same
breath: *"raw UDP would mean writing reliability"*. Sequence numbers, acknowledgements,
retransmission, congestion control and ordering are a library's worth of work with a long tail
of bugs that appear only under loss. ENet is that library, at 1.3.18, under MIT, with no
dependencies.

**TCP, written directly on the platform's sockets.** No dependency at all, and reliable-ordered
by definition. Rejected for head-of-line blocking — one lost segment stalls every later turn
already received, which under lockstep stalls every peer — and because it would mean framing,
socket code on three platforms, and the `net → platform` edge that sank SDL3_net.

## Consequences

**The manifest goes from seven packages to eight**, and `vcpkg.json` gains an `overrides` entry
because every other dependency here is version-pinned. The continuous-integration caches key on
`vcpkg.json`, so the first run after this lands rebuilds from source — cheaply, because ENet has
no dependencies and `restore-keys` brings the rest back.

**`net` stops being able to claim it owns no transport**, and its row in the ownership table
says so today: its "Never" column reads *"Owns a transport"*. That row is rewritten in the slice
that adds the dependency, not before.

**A socket test is the first test in this repository to open one.** By the owner's decision it
runs everywhere, as an ordinary integration case with a short deadline and
operating-system-assigned ports, so two runs cannot collide. A proof that does not run on the
platforms it claims to work on is not a proof.

**Two processes is a capability the test harness does not have.** Everything in
`tests/integration/` today is a single blocking `subprocess.run`; M16's use of a thread mutates
a file beside one running binary and is not a precedent. Process lifecycle, output pumping and
teardown-on-failure are written once, as a helper, and that is where this milestone's flakiness
risk lives — which is worth saying before it bites rather than after.

**What does not change is the interesting part.** No module-graph edge, no new thread, no
threading-table row, no framing, no test rewritten to accommodate the seam, and every golden
hash byte-identical. A transport that required any of those would have been the wrong one.

## Rollback cost

**Low in code, moderate in commitment.** The seam is the durable part and is worth keeping
whatever happens to ENet: `net::Link` with `LoopbackHub` behind it is a strictly better shape
than a raw `LoopbackHub*`, and it would survive swapping the transport underneath.

Removing ENet specifically means deleting one class, one `PRIVATE_DEPS` line, one manifest entry
and one override — genuinely small, because the decision to keep it behind `Link` is what makes
it small. **No ENet type appears in any header**, which is the same property that keeps
ADR-0015's fallback runtime a real option rather than a sentence.

What does not roll back is the wire contract. Once two builds have agreed a `SourceId` over a
socket, the index-assignment rule and the reconciled message bound are load-bearing for anything
that has to interoperate with them. Those are decisions 8 and 9, and they are the two here worth
re-reading before this is accepted.
