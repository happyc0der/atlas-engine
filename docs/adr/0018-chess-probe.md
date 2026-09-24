<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0018 — Chess as the first consumer: an in-tree probe of the engine, not v1.0

## Status

**Accepted**, 2026-09-23, on the day it was written, by the reasoning
[ADR-0010](0010-charter-amendment.md) recorded for itself: a record whose decisions are about
*where* and *under what rules* work proceeds has no implementation step of its own, and every
later record in the series cites it, so it cannot be Proposed while they proceed. The work it
governs is M18 to M23; the two engine changes it names are decided by their own records, 0019
and 0020, which are Proposed until built.

*2026-09-24:* M18 to M22 landed. The mod opponent D7 numbers M23 stays deferred by the owner's
decision and loses the number, because milestone numbers follow landing order and M23 went to
the first of the three items M17 had left open. It is numbered when it is scheduled.

## Context

The owner's request, 2026-09-23: *"make the first ever game on this engine … chess, since a map
based game engine should easily generalize to be able to make any board games. So plan out how
you will use it, I imagine this would surface many bugs within the game engine."*

Two things are asked for at once. The first is a game — chess, complete, playable by two
people over a socket, saved, replayed and hashed like everything else on the engine. The
second is a test of the engine, and it is the one the project has been waiting for: the
charter's own criterion for v1.0 is *"a game project links the engine, loads data, and runs a
deterministic simulation without patching engine internals"*, and in seventeen milestones
nothing has tried.

**Three things the tree says today make this a decision rather than a task.**

`apps/CMakeLists.txt:3` says applications are *"composition roots and acceptance harnesses,
not games and not libraries"*. Chess is both. That rule was written in M7 to stop the lab
growing rules of its own, and it was right; it did not anticipate a game being built *as* a
harness.

`CLAUDE.md`'s first rule is *"Build the engine. Never implement the game"*, with a list —
countries, wars, diplomacy — that is about the grand-strategy game the engine exists for. Chess
is not that game and touches none of that list, but a reader of the rule would be right to ask
whether a chess application in the tree violates it. This record answers.

`docs/DEFERRED.md`'s `runtime` entry, re-deferred in M7, names its trigger as *"a third
application, or the two loops converging in shape"*. Chess is the third application. A deferral
whose trigger fires and is not answered is a deferral nobody is reading.

### The predictions, and how they scored

The plan for this series wrote nine predictions about where the engine would bend **before**
the code was surveyed, so that they could be wrong, and then scored them. They are recorded here
because they are the evidence for the decisions below, and because the discipline is the same
one `docs/PERFORMANCE.md` applies to numbers: a claim made after the measurement is not a claim.

| # | Prediction | Verdict |
|---|---|---|
| 1 | Command validation cannot see the world, so an illegal move has nowhere to be refused | **Confirmed**, and worse: the lab already hides a state refusal inside `apply` and counts it as applied (`apps/lab/sim/src/commands.cpp:57-66`) |
| 2 | There is no clean game over; a session ends only in failure | **Confirmed**: `Session::quit()` is reported as an `Unavailable` error, and `PollReport::closed` exists and is never delivered |
| 3 | The interface's text keys are engine-owned, so a game cannot add a string | **Wrong in mechanism**: `Catalog::insert` already lets an application own keys; the gap is that `--text-check` walks only the engine's list |
| 4 | The grid fits: a board is the lab's cell model with different values | **Half**: the simulation half holds, the presentation half (`CellField`) does not — no texture, a fixed palette, and it drags `lab_sim` |
| 5 | Picking and the cell field are lab-owned and should be extracted | **Confirmed as a finding**, remedy changed by 4: chess draws on `QuadBatch` directly |
| 6 | A mod opponent will expose that nothing non-trivial can be authored for the sandbox | **Confirmed**: `tools/gen_mods.py` hand-assembles bytes with no loops, branches or locals |
| 7 | Pieces need glyphs and there is no text renderer | **Confirmed**: the sheet generator draws solid rectangles only |
| 8 | Save, replay and the golden hash need nothing | **Confirmed**: chess is integer throughout |
| 9 | Lockstep's tick-per-turn shape is fine | **Confirmed**: M14 sends empty turns explicitly |

Seven confirmed, one half, one wrong in mechanism. The surveys also found three things nobody
predicted: the placement contradiction above; the `runtime` trigger firing; and **a regression
M16 shipped** — every panel title, every statistic label and the mode buttons showed their keys
rather than their text, because the resolver was defined below the first panel that needed it,
and `--text-check` passed throughout because it checks that keys resolve and not that widgets
ask. Fixed in M18 slice 0, with a test that counts lookups. The first consumer of M16 found it
in an afternoon, which is the whole argument for building a consumer.

**Where the hypothesis stands.** A map engine generalises to a board game in its *simulation*
and not in its *presentation*, and that is the useful shape of the answer. The kernel, the
queue, the hashing, the replay, the save, lockstep and the transport take chess without change.
What does not generalise is the lab's view and the lab's command shapes, which were never engine
code. The two engine changes chess forces — a command that can be declined on state, a session
that can finish — are changes any game would force on its first day.

## Decision

**D1. Chess lives in the tree, at `apps/chess/`, as a recorded exception to the rule at
`apps/CMakeLists.txt:3`.** That line is amended to say so and to name this record. It is an
exception and not a repeal: the rule still binds `apps/lab` and `apps/sandbox`, and a second
game in the tree needs a second record.

**D2. The probe is not v1.0.** The charter's v1.0 criterion names *"a game project"* — the
charter's second user, in its own repository, linking the engine as a dependency. A game inside
the tree cannot prove that the engine's *public* surface is sufficient, because it can see
everything. What it can prove is narrower and still worth having: that a game can be written
against the command queue, the tables, the hashing, the save and the session **without a line
of it reaching `engine/`**. That is the exit criterion for the series, not v1.0.

**D3. No chess reaches `engine/`, and the fence is mechanical.** `apps/chess/sim` — the rules
library — may link `atlas::simulation` and nothing else, enforced at configure time by the same
`FATAL_ERROR` loop `apps/lab/sim/CMakeLists.txt:21-29` uses. `CLAUDE.md`'s "never implement the
game" is amended by one sentence naming this record, and its list of forbidden things is
untouched: chess has no countries.

**D4. The engine changes chess forces are made properly, each by its own record, before the
code that needs them.** A command that may be declined on world state is
[ADR-0019](0019-declined-commands.md) and M19. A session that can finish is
[ADR-0020](0020-session-finish.md) and M21. Neither knows what a bishop is; both are things the
lab already needed and worked around. The third gap — `--text-check` blind to an application's
own keys — is an additive change to `text::Catalog` and a second check, made in M22 under this
record, because it is not an irreversible shape.

**D5. The `runtime` module is not built, and its deferral is re-recorded with the count at
three.** Chess's composition root is a few hundred lines, and what it shares with the lab's is
the same ~195 lines of utilities that already live in `apps/common` plus a loop that differs in
every ingredient that matters: chess has no snapshot channel, no cell field, no mods, and a
board that changes sixty times a game. Three loops that agree only in their ingredients are not
a shape. The condition for building it is unchanged in kind and sharper in number: the loops
converging, or a fourth application.

**D6. Lifting chess out into its own repository is a later milestone, with its trigger
recorded.** The trigger is the engine gaining an install and export target, which it does not
have, and without which an out-of-tree consumer would be a submodule pointing at a source tree.
When that exists, chess is the obvious first thing to build against it, and *that* build is the
one the charter's v1.0 criterion is about.

**D7. A mod as the opponent is its own milestone, M23, after the game exists.** It forces a
mod-authoring toolchain decision that ADR-0015 deferred by design, and that decision is larger
than chess. Recorded in `docs/DEFERRED.md` with its trigger.

**D8. Everything, including draws.** Castling, en passant, promotion, check, checkmate,
stalemate, the fifty-move rule, threefold repetition and insufficient material. A probe that
skipped the awkward rules would prove that the easy ones fit, which was never in doubt.

## Alternatives

**A separate repository now.** The honest v1.0 test, and the owner's first instinct was asked
about. Rejected for the series because the engine has no install target: the game would consume
a source tree by relative path, every engine change in M19 and M21 would be a two-repository
change, and the continuous-integration matrix would double. D6 records it as the destination.

**A game in the lab.** The lab has the grid, the picking and the panels. Rejected because the
lab's rule at `apps/CMakeLists.txt:3` exists precisely so that it never grows rules, and because
the lab's cell field is the wrong presentation (prediction 4). A lab with a chess mode would be
two applications in one binary.

**Working around the two engine gaps in the game.** An illegal move as a silent no-op inside
`apply`; a finished game as two peers calling `quit()`. Both are what the lab does today, and
both were put to the owner and refused: the point of the probe is to find what the engine lacks,
and a workaround is a way of not finding it.

**Repealing the `apps/` rule.** Simpler than an exception. Rejected because the rule is right
about the lab and the sandbox, and a repeal would leave nothing to stop the next harness growing
a game by accident.

## Consequences

- `apps/CMakeLists.txt:3` names the exception. `CLAUDE.md` gains one sentence. Neither weakens
  the rule for `engine/`.
- ADR-0019 and ADR-0020 are Proposed and gate M19 and M21. The chess milestones are M18 to M22,
  with M23 deferred.
- A third golden hash — a famous game's final position — joins the two engine goldens in M20,
  and is compared on three platforms like them.
- The `runtime` deferral is answered rather than left firing. The lift-out and the mod opponent
  are recorded with triggers.
- **The risk, stated.** A tree-internal game can quietly use what an external one could not —
  a private header, a test helper, a build-only target. D3's fence catches the link-level case
  and nothing catches an include of `engine/*/src`. The mitigation is the module-deps check,
  which already refuses a private header from outside its module, and the M22 report, which must
  list every engine symbol chess uses.

## Rollback cost

Low in code: `apps/chess/` is one directory, and removing it and the exception at
`apps/CMakeLists.txt:3` restores the rule as written. **The two engine changes do not roll back
with it**, and are not meant to: ADR-0019 and ADR-0020 each stand on their own reasons, and each
names a consumer the lab already had. What cannot be undone is the same thing ADR-0010 said of
itself: the tree has once held a game by decision, and the next request to hold one will cite
this record.
