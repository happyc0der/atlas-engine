<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0019 — A command may be declined on world state

## Status

**Accepted**, 2026-09-23, implemented in M19.

Proposed when it was written the same day at M19's gate under [ADR-0018](0018-chess-probe.md)
D4, and the code followed the record. One decision was sharpened by implementing it and is
marked at its own heading: D7's account of the lab's hidden refusal was wrong about *which*
refusal it was, and the correction found a latent out-of-bounds write. Every other decision
survived unchanged, including the one most at risk — that every golden hash is byte-identical
across a change to the signature of every command handler in the tree.

Amends the tick contract in [ADR-0003](0003-simulation-render-separation.md) and the
`TickReport` split [ADR-0014](0014-deterministic-lockstep.md) made in its decision 10, by
adding a third fate for a command beside the two they name. Neither record is superseded:
everything they say about late and invalid commands stands.

## Context

A command that reaches the kernel has, today, two fates the kernel can name and one it cannot.

**Late**: stamped for a tick that has already run. Counted in `TickReport::commands_late`, a
protocol violation under lockstep (ADR-0014). **Invalid**: the payload no longer passes the
handler's `validate`, or the type has lost its handler. Counted in `commands_invalid`, logged,
skipped, identical on every peer. And then the third: **a well-formed command, delivered on
time, whose effect the world refuses.** The piece is not there. It is not your turn. The cell
is already owned. The slot is taken.

`CommandHandler::apply` is `std::function<void(World&, std::span<const std::byte>)>`
(`command.hpp:98-108`). It returns nothing, so the third fate has nowhere to go, and every
handler in the tree that meets it does the same thing: **returns early and says nothing.**

- `apps/lab/sim/src/commands.cpp:57-66` decodes the payload against the live cell bound and
  returns on failure, with a comment explaining that it is refusing on state. The kernel counts
  that command as applied, records it into the replay as applied, and the hash proceeds as if
  it had been.
- `engine/simulation/tests/synthetic_scenario.hpp:48-63`, the fixture every lockstep proof runs
  on, returns silently on a missing table and on a row out of range.
- `engine/net/tests/net_harness.hpp:188-209` returns silently on a cell out of range, and — the
  interesting one — **models a state-dependent refusal by writing the refusal into the world**,
  because that was the only way M14 could make one observable. Its own comment says why: *"A
  rejection that were a silent no-op would be indistinguishable from the command never
  arriving."*

That comment is the finding. M14 needed to prove that two peers refuse identically and had to
invent a hashed side effect to prove it, because the kernel offers no way to say "this command
was refused" that a test, a monitor or a peer could see. Chess needs the third fate roughly
sixty times a game — every move a remote peer submits is checked against the position and the
side to move, and the check is authoritative — and a probe that hid each one inside `apply`
would be the exact workaround ADR-0018 exists to refuse.

### Why the decline is not a rejection

A decline is different in kind from both existing fates, and the difference is what makes it
safe. A late command depends on *when it arrived*, which peers do not share. An invalid
command depends on *bytes*, which they do. A declined command depends on **world state and
the payload**, both of which every peer holds identically at the same tick — so **a decline is
deterministic by construction**. Two peers that would apply a command identically will decline
it identically. It is not a protocol violation, not an error, and not a divergence. It is a
result.

That is also why it needs its own count. Folded into `commands_invalid`, a monitor could not
tell a mod sending bad bytes from a player trying an illegal move; folded into
`commands_applied`, a replay would carry commands that did nothing and say they did something.

## Decision

**D1. `apply` receives the context and returns a verdict.**

```cpp
/// Who submitted the command and at which tick it is being applied.
struct ApplyContext {
    SourceId source;
    Tick tick;
};

struct CommandHandler {
    std::function<Status(std::span<const std::byte>)> validate;                    // unchanged
    std::function<Status(World&, const ApplyContext&, std::span<const std::byte>)> apply;
};
```

An `ok()` from `apply` is *applied*. An error is a **decline**: the handler looked at the world
and said no, and the error carries the reason in words. `validate` is untouched; its job — a
bad payload refused whole before anything is touched — is exactly what it was.

**D2. A handler that declines has changed nothing.** That is the contract, stated in
`command.hpp` and enforced by every test of a handler that can decline: hash before, decline,
hash after, equal. The kernel cannot check it in general, because a handler owns its own tables;
it is written where the lab learned it. A handler decides *before* it writes, and a partial write
followed by a decline is a bug in the handler, not a kind of decline.

**D3. Counted as its own fate.** `TickReport` gains `commands_declined`, beside
`commands_late` and `commands_invalid`. `commands_rejected()` keeps its meaning — late plus
invalid — so no assertion written against it moves; a decline is not a rejection. `Kernel`
gains the cumulative `declined_commands()` beside `late_commands()` and `invalid_commands()`.

**D4. Not recorded, not hashed.** A declined command does not enter `applied_commands` and so
does not reach a replay, for the same reason an invalid one does not: a recording is of what
changed the state, and a decline changed nothing. On replay the same world at the same tick
declines the same command it did not see, which is the same outcome. It is logged at **debug**
with its reason, not at warning: a decline is ordinary and expected, and a chess game would
otherwise warn on every illegal attempt.

**D5. `ApplyContext::source` is what the chess rules library needs, and it is not game
knowledge.** A rules table that says which source holds which colour can refuse a move from the
wrong peer *inside the simulation*, where it is hashed and identical everywhere, rather than in
the application, where a modified client could skip it. `tick` lets a handler stamp when a
thing happened without a second table. Neither field means anything the engine has to
understand.

**D6. `CommandQueue::apply` reports the outcome rather than collapsing it.**

```cpp
/// What became of a command that reached its handler.
struct ApplyOutcome {
    /// `ok()` when applied; the handler's reason when declined.
    Status verdict;
    [[nodiscard]] bool applied() const noexcept { return verdict.has_value(); }
};
[[nodiscard]] Result<ApplyOutcome> apply(World& world, const Command& command) const;
```

The outer error is what it was — no handler, or `validate` failed — and stays *invalid*. The
inner verdict is the handler's, and the kernel counts it.

**D7. The hidden refusals in the tree become real ones**, in the same milestone, so the change
has three consumers on the day it lands rather than none:

- The lab's `set_color_index` declines a cell outside the bound, and its early return is
  deleted. The lab's synthetic generator never produces such a cell, so **every golden hash is
  unchanged**, and a new lab case submits one deliberately and asserts it is counted declined
  and the hash unmoved.

  *Corrected during M19, 2026-09-23.* The bound check is `validate`'s, and the queue re-runs
  `validate` an instant before `apply` against the same bound, so the early return this bullet
  describes was unreachable on one thread and could not be the decline. What the handler never
  checked was **the table it was about to index**: the bound is the application's claim about
  the grid and the table is the world's, they agree in every run the lab makes, and a bound
  that lagged a load would have passed validation and written past the end of the table. That
  is the state-dependent refusal, it is the one the handler now makes, and the case above
  provokes it by inflating the bound. The early return stays and returns its reason.
- The synthetic scenario's `bump` declines a row out of range. The missing-table return stays
  an assertion rather than a decline: a handler registered against a table that does not exist
  is a setup mistake, not a state the world can be in.
- The net harness gains `kClaimIfFree` beside `kClaimCell`: a claim that **declines** when the
  cell is owned, with no side effect. The M14 case that needed a hashed refusal keeps it — it
  is a legitimate applied command that records a contest — and a new case asserts that two
  peers report identical `commands_declined` at every tick with equal hashes, which is the
  proof M14 could not write.

**D8. The migration is mechanical and its exit criterion is silence.** Twelve `apply` lambdas
take the new signature and return `ok()`; nothing declines yet; every golden hash is
byte-identical; every existing test passes. Only then do D7's three consumers land, each in its
own commit.

## Alternatives

**Leave `apply` as it is and let the game refuse before submitting.** The application checks
legality, and only a legal move is ever submitted. This is what a chess client should *also*
do, because a decline arrives `input_delay` ticks after the attempt and a person should not
wait for it. But it cannot be the *only* check: a remote peer's client is not this peer's to
trust, and a move that reaches the simulation must be judged there or a modified client wins
every game. The authoritative check has to be inside the tick, and inside the tick it has to be
reportable.

**Widen `validate` to take the world.** Then "is this legal in this position" is checked where
"is this well-formed" is. Rejected because `validate` runs at submission too, against a world
that is `input_delay` ticks younger than the one the command will meet, so a state check there
is a guess; and because it would give the validator a `const World&` that a handler could use
to make validity depend on state that changes between submission and application, which is the
category of bug ADR-0003 separated the two to prevent.

**Fold declines into `commands_invalid`.** One counter fewer. Rejected because the two mean
opposite things to a monitor — bad bytes from a mod versus a legal attempt at an illegal thing
— and ADR-0014 split late from invalid for a weaker version of the same reason.

**Record declined commands into the replay, flagged.** A complete record of what was
*attempted*. Rejected as scope: a replay is a recording of what changed the state, and a
declined command is exactly as absent from the state as an invalid one, which is not recorded
either. If a game wants its attempts, it has the log and the count. Recorded in `DEFERRED.md`
with a trigger — a consumer that needs to replay the *decisions* rather than the *state*.

**A dedicated `ErrorCode::Declined`.** A name for the verdict in logs and tests. Rejected as
unnecessary: the outcome is decided by *which value* the handler returned, not by its code, and
a handler's reason is better named by the code that fits it — `OutOfRange` for a cell, and
whatever the chess library chooses for a move — than by a code that says only "no".

## Consequences

- `command.hpp`, `command.cpp`, `kernel.hpp` and `kernel.cpp` change. Twelve lambdas in
  `engine/`, `apps/`, `benchmarks/` and `tests/` are migrated. `apply`'s signature is a public
  API shape and this record is the asking.
- `TickReport` gains one field and `Kernel` one accessor. The overlay's statistics row and the
  lab's summary gain a `declined` count, so the third fate is visible where the other two are.
- `docs/DETERMINISM.md`'s sentence *"a rejected command is logged and dropped rather than
  partially applied"* gains a second sentence naming the decline and why it is deterministic;
  `docs/ARCHITECTURE.md`'s tick contract gains the same. `CLAUDE.md`'s tick rule gains one
  sentence: a handler that declines has changed nothing.
- **A handler can now allocate on the decline path**, because an `Error` carries a formatted
  reason. Declines are rare by construction — a peer attempting an illegal thing — and the
  applied path allocates nothing new. A handler expected to decline thousands of times a tick
  would be a handler doing validation's job in the wrong place.
- The M14 proof gains the case it could not write, and the lab loses the one place it counted a
  refusal as an application.

## Rollback cost

Moderate in code, low in meaning. Reverting the signature is twelve mechanical edits back and
the deletion of one field, one accessor and one struct; nothing serialised changes, no format
version is spent, and every golden hash is the same on both sides of the change. What would be
lost is the three consumers in D7, each of which would go back to hiding a refusal inside
`apply` — which is the state this record exists to end.
