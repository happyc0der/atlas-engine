<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0014 — Deterministic lockstep over the command queue

## Status

**Accepted**, 2026-09-18, implemented in M14. Authorised by
[ADR-0010](0010-charter-amendment.md), which fixed networking's meaning as "deterministic
lockstep over the command queue and nothing else". Inherits
[ADR-0009](0009-scripting-decision.md)'s decision 2 — the command-queue boundary — unchanged,
and narrows it in one respect recorded below. The numeric policy this rests on is
[ADR-0008](0008-numeric-and-save-policy.md).

**No transport is chosen here and none is added.** Nothing reaches `vcpkg.json`. What this
record decides is the shape a transport would later plug into, and the criteria for choosing
one are written down rather than resolved.

**2026-09-20:** [ADR-0017](0017-lockstep-transport.md) is written and proposes superseding
**decision 9 only** — it chooses ENet, and a dependency does now reach `vcpkg.json`, so the two
sentences above stop describing the tree when M17 lands. They are left standing rather than
edited, because what this record decided without a transport is the part worth being able to
read later. **Decisions 1 to 8 stand unchanged**, and decision 3 — readiness depends on who has
reported and never on elapsed time — is the invariant 0017 is answerable to: the deadline for a
silent peer lives in the transport, and the gate still reads no clock. **It was accepted on
2026-09-22 when M17 closed, and is in force.**

0017 also answers, rather than supersedes, the question this record left open at decision 3:
what to do about a peer that has stopped reporting. The answer is that it ends the session,
because dropping it and continuing is simulation-visible and would need every remaining peer to
agree the drop tick or diverge.

## Context

The engine was not built for networking and is unusually ready for it, because determinism
demanded the same things lockstep does.

The command queue's own header already names a network peer as one of its four callers. Its
ordering rule is documented as *"ordered by source and sequence, never by arrival — a total
order that two machines can agree on without agreeing on timing"*. `SourceId` is described as
*"an index, not an address: it is written to replays and compared across machines"*.
`submit_stamped` exists as the receive path and already advances a source's counter past what
it ingests. Saves persist per-source sequence counters. Floating point in authoritative state
is gated behind a cross-architecture golden-hash comparison, fused multiply-add is off
project-wide, and the golden hashes have been measured identical on arm64, x86_64 and MSVC
since M7.

**What is missing is one thing: nothing waits for anybody.** `Kernel::step()` runs whenever its
schedule is finalised. There is no concept anywhere in the engine of a tick that is not yet
allowed to happen.

## Decision

**1. Peer-to-peer lockstep with an input-delay window.** Each peer stamps its own commands for
`T + delay`, sends one turn per tick to every other peer, and a tick executes only when every
expected source's turn has arrived. Every peer then applies the same commands in the same order
and reaches the same state **without exchanging any state at all**. The agreed delay is the
**maximum** of every peer's proposal, not the host's: a peer given less delay than it needs is
late every tick, and late is fatal here.

**2. `sim::TurnGate` holds the bookkeeping, and readiness depends only on which sources have
reported — never on time.** The gate reads no clock, holds no deadline, and has no timeout.
Every one of its answers is a pure function of the marks it has been given.

This is the load-bearing half of the record. The moment readiness could turn on elapsed time,
two machines running at different frame rates would run different ticks with different
commands, which is precisely the failure that stamping a command with its target tick was
introduced to prevent. **What to do about a peer that has stopped reporting is a transport
policy** — drop it from the expectation set, or stop the session — and that decision reaches
the simulation only as a call to `expect_sources`, never as a timer inside the gate.

An empty expectation set is always ready, so a solo run takes byte-for-byte the path it took
before M14. That is what keeps the golden hashes identical, and it is asserted rather than
assumed.

**3. The kernel consults the gate, and `step()` refuses a tick that is not ready.**
`KernelConfig` gains a borrowed `const TurnGate*`; a refused tick returns
`ErrorCode::Unavailable` naming the sources it is waiting on.

**This amends `kernel.hpp`'s documented contract**, which said: *"Fails only when the setup is
wrong, never because of what a system did."* A gate refusal is neither of those — it is a
runtime condition, and the sentence becomes false. It is amended here, deliberately, rather
than quietly outgrown.

The alternative was a gate the caller consults with the kernel untouched, which keeps the
contract intact at the price of a composition root that forgets the check running ahead of its
peers and diverging, with nothing able to tell it so. M11 shipped a gamepad that no composition
root ever enabled; the lesson taken from that is to make the omission loud.

**The check goes before the command drain**, and this is the most consequential sentence in the
implementation. `drain(tick)` *removes* what it returns. A tick refused after draining has
already taken its commands out of the queue and discarded them with the report; the retry would
run the same tick with fewer commands and produce a different hash from every peer — silent
corruption of the one property lockstep exists to provide. Placed first, "a refused tick changes
nothing" holds by construction rather than by cleanup.

**`Kernel::ready()` is the normal path and a refusal is the exception**, because `Error`
construction allocates and says so at its own definition. A stall lasting a thousand frames must
not allocate a thousand formatted strings inside the tick loop to report that nothing happened.

**4. `sim::CommandSource` lives in `simulation`, not in `net`.** A network peer, a replay, a
local input mapper and M15's sandboxed mod are the same thing to a tick: something outside it
that submits stamped commands and says when its turn is done. Putting the interface in a
networking module would make a mod depend on networking, or grow a second interface meaning the
same thing — and two interfaces that mean the same thing drift.

A source is handed a `SourceGate`, which binds the gate to one identifier, rather than the gate
itself. With a bare `TurnGate&` the rule "mark only your own turn" could only be a comment, and
in M15 the thing on the other side of that comment is untrusted.

**5. One `Turn{tick, source, commands}` message, never `Commands` plus `TurnComplete`.** Split
in two, the gate's correctness would depend on transport ordering: under reordering a completion
arriving before its commands opens the gate on an incomplete turn, the tick executes, the
commands arrive late, and the peers have diverged with nothing lost and no error raised. A
correct-looking run with different state is the worst failure available here. One message makes
"arrived" and "complete" the same event, which no reordering can separate. **An empty turn is
sent explicitly**, because silence is also what a dead peer produces.

**6. Link latency is measured in receiver polls, not in ticks.** If delivery were conditioned on
the receiver's tick advancing, then in exactly the situation the gate exists for — A waiting on
B — A's tick does not advance, so B's turn is never delivered, so A waits for ever. Tick-based
latency deadlocks precisely when the gate is doing its job. A poll is something a stalled peer
still does.

**7. A divergence is detected, attributed, and stops the session. Atlas does not resync.** This
mirrors device loss, which the charter already treats the same way: detected and reported with
an actionable error, never recovered from. The attribution names the first system whose writes
differ, because "something diverged at tick 4000" is not a starting point for anybody. The
recordings on each side are the debugging artefact.

**A late turn is a protocol violation, not a warning.** Under lockstep a command stamped for a
tick that has already run cannot be applied by anyone, so the session is no longer sound. The
turn's tick is checked before `submit_stamped`, and the session ends.

**8. Compatibility is probed with the golden hashes.** `Hello` carries the protocol version, the
hash-algorithm version, the save and replay format versions, a build identifier, **both golden
hash values**, the seed, the start tick and the initial state hash. Two builds that produce the
same golden hashes agree about the simulation whatever else differs between them, which is what
the charter permits a cross-build session on. A differing build identifier is logged and not
refused: refusing on it would forbid a debug peer playing a release peer even when their goldens
agree, which is a thing this project wants to be able to test.

**9. No transport, and the criteria for choosing one are recorded** rather than resolved: a
reliable ordered channel, because lockstep tolerates no loss and raw UDP would mean writing
reliability; Windows support in the pinned baseline; licence compatibility with GPL-3.0; whether
encryption and NAT traversal are wanted; and whether it may own a thread of its own.

## Alternatives

**Client-server state synchronisation.** The server simulates and broadcasts state. Rejected:
it needs per-tick state serialisation, and the world is megabytes while the inputs are a few
bytes — the wrong shape for a strategy game by three orders of magnitude. It also discards the
determinism the engine already has rather than using it.

**Rollback.** Predict, and re-simulate when a prediction was wrong. Rejected: it needs the same
per-tick state snapshots, and it buys sub-tick input latency, which a map-based strategy game
does not need. Recorded with its trigger, which is a consumer that needs input to feel immediate.

**Host relay.** Not an alternative — it is a topology *inside* lockstep, and it costs nothing
later because a turn is addressed by `SourceId` rather than by a socket.

**The gate consulted by the caller, kernel untouched.** See decision 3.

**`CommandSource` in `net`**, as the original brief had it. See decision 4.

**A timeout inside the gate.** Rejected by decision 2, and worth naming as rejected rather than
unconsidered: it is the obvious thing to reach for and it would make readiness depend on elapsed
time, which is the one property this whole record protects.

## Consequences

**Bandwidth is not the cost; the stall is.** A turn is a handful of commands — kilobytes a
second for eight peers. What lockstep costs is that a peer late by more than the delay freezes
everyone, and no amount of bandwidth fixes it. That is the trade, taken knowingly.

**`DETERMINISM.md`'s sentence about falling behind becomes half true and is split rather than
replaced.** *"Catch-up is clamped and surplus ticks are discarded, so falling behind changes
throughput, never results"* remains exactly true of every ungated run, which is every solo run
for ever. Under a gate a due tick is not discarded but postponed: it runs at its own number as
soon as every source has reported. Weakening a correct claim to make room for a new one would
not be an improvement.

**`SourceId::Local` becomes a role.** It means peer zero, not "whoever is running this". Two
call sites in the lab hard-coded it and were corrected in M14's opening sweep; a peer that
stamped `Local` would sign another peer's name to its own commands and the two streams would
collide in the total order lockstep depends on.

**`TickReport` had to learn the difference between a late command and an invalid one.** It
counted both as `commands_rejected`, and only a cumulative counter on the kernel separated them.
Under lockstep they are opposite: a late command is a protocol violation that ends the session,
an invalid one is something every peer rejects identically and must not.

**A gate that is never marked hangs, by design.** There is no timeout, so a peer that stops
reporting stalls the session until something outside changes the expectation set. `PollReport`
reports a closed source explicitly for that reason, and the lab's loopback harness bounds its
own wait so a test cannot hang continuous integration.

**M15 inherits the interface.** A sandboxed mod is a `CommandSource` with a bit-31 identifier,
local to each peer and never sent on the wire; the hash check catches a mod that decided
differently. That is a consequence of decision 4 and is why it was decided this way.

**What `SourceGate` does not close**: a `CommandSource` can still call `CommandQueue::submit`
with another source's identifier. That matters only when the source is untrusted, which is
M15's problem and M15's record; it is deferred with that trigger rather than left unsaid.

## Rollback cost

**Low in code, and the shape is what would survive.** The gate is one class that nothing is
obliged to use; `KernelConfig::gate` defaults to null and a null gate is the old path exactly,
which the golden tests assert. The `net` module is additive and depended on by one application.
Removing lockstep would mean deleting a module and one field.

What would not roll back is the amended sentence in the kernel's contract, and the fact that
`step()` can now fail for a runtime reason — every caller written after this point assumes it.
That is the real cost, and it is the reason this is a record rather than a commit message.
