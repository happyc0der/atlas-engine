<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0010 — Charter amendment: seven subsystems by owner decision, physics kept out

## Status

Accepted, 2026-09-17.

Accepted rather than Proposed, although nothing is built, for the reason ADR-0009 gives: an
amendment has no implementation step and is in force the moment it is recorded. It is also the
first record of the M10 to M16 series, and every later record in that series cites it as its
authority, so it cannot sit as Proposed while they proceed.

## Context

Engine v0.1 was declared at M7 against the charter's list item by item. M8 and M9 followed, and
`docs/ROADMAP.md`'s closing section records the roadmap's own view of what comes next: not
another milestone, but whether to start the game, because a game is the only thing that can
tell whether the engine's abstractions were the right ones.

The owner has decided otherwise. On 2026-09-17 they asked for seven subsystems before the game:
animation, networking, sandboxed mods, audio, gamepad input, input method editors, and
localisation. Physics was raised and deliberately left out.

**Two different kinds of exclusion are being lifted, and they are not equivalent.**

The charter says audio, gamepad input, IME and localisation "are out of scope for v0.1". That is
a scope boundary on a release that has shipped. Lifting it needs no justification beyond the
owner wanting the work; the sentence has already done its job.

The charter also says, under non-goals, that Atlas is not a universal engine: "There is no plan
for physics, animation, networking, a general-purpose plugin ABI, or a custom scripting
language." That is a statement of identity, and three of the seven contradict it directly.
ADR-0009, accepted two days ago, built part of its argument on the plugin-ABI clause.

**The discipline being suspended, and its exact extent.** The charter's rule was that a
project-sized subsystem needs a recorded limitation from the thing being built, not an
expectation that one will appear. For these seven items only, that bar is replaced by an
explicit owner decision. It is not abolished. A custom allocator, a custom entity-component
system, a render graph, a work-stealing scheduler, native Vulkan or Direct3D 12 backends,
filesystem watchers, multi-viewport overlay windows and physics all keep it.

**Two documents already disagree and this record reconciles them.**
`docs/PROJECT_CHARTER.md` says there is "no plan for networking"; `docs/DEFERRED.md` lists
networking under "deferred until a recorded limitation justifies the work", which is the softer
bar. Both cannot be true. The charter's text was the stricter and the deferral list had drifted.

**ADR-0009's trigger, and honest accounting of it.** ADR-0009 decision 6 set three conditions
for reconsidering scripting. The owner is firing that trigger deliberately, and the record
should not pretend the conditions were met on their own terms:

1. *A recorded limitation, written by whoever hits it.* **Not met.** It is replaced by this
   decision. Nobody hit a limitation; the owner chose to build the capability.
2. *`CommandQueue`, `CommandHandler`, `SnapshotHeader`, `kReplayFormatVersion`,
   `kSaveFormatVersion` and `kHashAlgorithmVersion` unchanged for two consecutive milestones.*
   **Will be met by M15**, provided M14 is additive. M14's design adds types and touches none
   of those six. ADR-0015 must restate this accounting when it lands, against what M14 actually
   did rather than against what it planned to do.
3. *The edit infrastructure exists.* **Met**, since M9.

## Decision

**D1. What "plugin support" means.** Sandboxed mods for players: untrusted scripts that reach
simulation state only through `sim::CommandQueue`. Never a C ABI for native code, never
dynamically loaded libraries, never "plugin modules" linked at build time. The charter's "no
general-purpose plugin ABI" therefore remains true and is reworded to say *for native code*.

**D2. What "networking" means.** Deterministic lockstep over the command queue, and nothing
else. Designed by ADR-0014 and proven on an in-process loopback before any transport is chosen.
Neither this record nor M14 adds a socket or a transport dependency.

**D3. Physics stays a non-goal, and gets a trigger.** A map-based strategy game has no rigid
bodies. What resembles physics in one — hit testing, unit movement, camera inertia — is either
picking, which exists, or a presentation tween, which M13 provides. Physics moves from the "no
plan" clause to the "deferred until a recorded limitation" list, with its trigger written down:
a consumer that needs bodies interacting through forces rather than through commands.

**D4. The exact text changes.** Each replaces the sentence named and nothing else in its
paragraph. `PROJECT_CHARTER.md` lines 28-29 ("game-specific scripting" as a non-goal) and 86-87
(the v0.1 definition) are untouched: the first because it is still true, the second because it
is history.

| Document | Change |
|---|---|
| `PROJECT_CHARTER.md` non-goals, "A universal engine…" | Loses animation and networking; keeps physics and the custom scripting language; the plugin clause becomes "for native code"; gains one sentence each defining what networking, animation and mods now mean, and a pointer here. |
| `PROJECT_CHARTER.md` non-goals, "Audio, gamepad input, IME, and localisation…" | Past tense, naming v0.1 as shipped, and recording that each is a planned milestone that may be dropped without amending the charter again. |
| `PROJECT_CHARTER.md`, the deferred list | "embedded Lua" leaves it for ADR-0015; "physics" joins it with the trigger from D3. |
| `PROJECT_CHARTER.md`, the cross-platform sentence | Both existing sentences kept; one appended about ADR-0014's golden-hash probe. |
| `PROJECT_CHARTER.md`, new section "After v0.1" | The v1.0 definition from D5. |
| `README.md`, "What is not done" and "Non-goals" | Reworded to match, keeping ADR-0009 as the record of why scripting waited. |
| `DEFERRED.md`, "Deferred by the charter…" | Split in two, per D6. |
| `adr/README.md` | Rows for 0010 to 0015; ADR-0009's status annotated; the legend gains "Superseded in part by NNNN". |
| `adr/0009-scripting-decision.md` | One appended, dated line under Status, pointing forward. |
| `ROADMAP.md` status table | Rows M10 to M16, status planned. |

**D5. The v1.0 question, defined and not answered.** Does completing M10 to M16 constitute
engine v1.0? **No**, recorded as the owner's default unless overturned. v0.1 was declared
against a checklist the charter wrote in advance, which is what made the declaration mean
something. There is no such checklist for these seven, because they were chosen by decision
rather than derived from a requirement. v1.0 is declared when a game project links the engine,
loads data and runs a deterministic simulation without patching engine internals — the
charter's own second user — because that is the only test of whether these were the right
seven. M16's close produces v0.2 at most.

**D6. Reconciling `DEFERRED.md` with the charter.** The "Deferred by the charter" section
becomes two:

- **Decided by ADR-0010, planned as milestones**: networking, sandboxed mods, animation, audio,
  gamepad input, IME, localisation. "These left the recorded-limitation regime on 2026-09-17 by
  owner decision. The condition for building each is now that its milestone is scheduled and
  has a consumer to build against; the condition for dropping each is that no consumer strong
  enough exists when its turn comes."
- **Deferred by the charter, until a recorded limitation justifies the work**: custom
  allocator, custom entity-component system, work-stealing scheduler, physics with its trigger,
  multi-viewport overlay windows, filesystem watchers. The closing sentence about
  project-sized subsystems stays exactly as it is.

**D7. `CLAUDE.md`'s "never implement the game" is untouched, and this record stresses it.** Mods
are the *game's* scripting surface. The engine provides the sandbox, the resource limits, the
command boundary and the loader; the Strategy Lab registers exactly one typed command as a
demonstration. A mod API that names countries, borders or diplomacy is the game's, and it is
written in the game's repository.

**D8. ADR-0009 is superseded in part, not replaced.** Its decision 2 — that anything outside
the tick reaches simulation state only through the command queue, reads only published
snapshots, and never executes during a tick — is reaffirmed and inherited by every record in
this series. Its decision 1 (no scripting in M9) is history and stands. Its decision 6 (the
deferral and its trigger) is superseded by ADR-0015 when M15 lands. The ADR index gains a
status value for this, because the existing four cannot express a record that is mostly still
in force.

## Alternatives

**Keep the regime and start the game.** The roadmap's own recommendation. The engine has every
capability the charter names; a game would immediately exercise what the Strategy Lab only
stands in for, and several deferrals are waiting for exactly that evidence. Rejected by the
owner's decision rather than by argument, and recorded that way so nobody later reads this as
the argument having been lost.

**Abolish the recorded-limitation regime entirely.** Rejected. Seven named items is the whole
scope; everything else keeps the bar, and physics keeps it despite being adjacent to the work.

**Amend the charter inside each feature's own ADR.** Rejected. The charter is the document the
ADR index says is never edited to hide a change of mind. One record that changes it, cited by
all seven, keeps the history legible; seven partial amendments would not.

**Declare v1.0 at M16.** Rejected, per D5.

## Consequences

- Five documents change as tabled. `CLAUDE.md` gains nothing from this record; M15 adds one
  invariant line when it lands.
- The blueprint at the repository root is the originating specification and is not edited. The
  charter now departs from it in the ways above, and this record is where that is written down.
- ADR-0011 through ADR-0015 cite this record as authority and inherit ADR-0009 decision 2.
- Each of the seven milestones is built against the strongest consumer that exists at the time —
  the lab, the sandbox, the overlay, or the engine's own tests — and ends with a whole-program
  proof rather than an API. Any may be dropped at its planning step, which changes the roadmap
  table and nothing else.
- **The risk, plainly.** Building subsystems without a game to consume them is precisely what
  the charter's discipline existed to prevent. Every one of these seven will be designed
  against a consumer that is a stand-in, and a stand-in cannot tell you that an abstraction is
  wrong in the way a real caller can. The mitigations above reduce that risk; they do not
  remove it, and this paragraph exists so that a later reader who finds an over-built subsystem
  knows the trade was made knowingly.

## Rollback cost

**High in identity, low in code.** Nothing is built by this record. Reverting it is reverting
five documents and, if any of the seven has shipped by then, either keeping that work under a
charter that forbids it or removing a working subsystem.

What cannot be reverted is that the charter has now once been amended by decision rather than
by evidence, and that the next amendment will cite this one as precedent. That is the real
cost, it is not recoverable by editing a file, and it is intended to be visible.
