<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0009 — Scripting: deferred, with the boundary decided now

## Status

Accepted, 2026-09-17.

Accepted rather than Proposed, although nothing is built. ADR-0003 and ADR-0004 were Proposed
because a milestone still had to implement them. A deferral has no implementation step: it is
in force the moment it is recorded. The half of this decision that *is* enforceable — that
nothing outside the tick may reach simulation state except through a command — is already
enforced today by `const World` and by the module graph, and this record is what stops that
being given away later by accident.

Not Rejected. Lua is not refused; it is unearned.

**2026-09-17:** the trigger in decision 6 is fired by
[ADR-0010](0010-charter-amendment.md), which replaces its "recorded limitation" condition with
an owner decision; decision 6 is superseded by ADR-0015 when M15 lands. **Decisions 1 to 5
stand**, and decision 2 — the command-queue boundary — is inherited unchanged by every record
in the M10 to M16 series.

**2026-09-18:** [ADR-0015](0015-sandboxed-mods.md) is written and proposes superseding decision
6; it takes effect when M15 closes, not now. It chooses WebAssembly over Lua, which is the
comparison this record asked for and on the grounds this record named. Decision 2 is inherited
unchanged. **Decisions 3, 4 and 5 are amended in part**,
and the amendments are worth following the pointer for rather than assuming: decision 3's
once-per-frame call becomes once per kernel tick and its snapshot view becomes a tick-boundary
view of the world, both because lockstep makes frame rate an input; decision 4's "text chunks
only, never precompiled bytecode" is reversed for WebAssembly, whose binary format is validated
before instantiation where Lua's is not; and decision 5's overlay-port objection has expired,
because Lua 5.5 takes the string-hash seed as an argument to `lua_newstate`. Decision 5's
remaining analysis stands and is part of why Lua was not chosen.

## Context

The charter and the originating specification both say the same thing in different words.

The charter lists among its non-goals "game-specific scripting" and "a general-purpose plugin
ABI, or a custom scripting language", and puts "embedded Lua" on the list of things "deferred
until a recorded limitation justifies the work". It states that a scripting layer is explicitly
not required for v0.1.

The specification is more specific about sequencing: "Lua only after stable native APIs exist",
and "Lua may be added later, only after the native engine API and serialization model
stabilize." It also fixes the security requirements in advance: a script must have "explicit
exposed APIs, resource/time limits where practical, and no unrestricted filesystem or process
access by default."

M9's exit criterion asks this record to decide whether Lua is justified, to identify its API
boundary and security model, and either to implement a tiny end-to-end script or to defer
explicitly.

**What a script could touch.** The engine has no game rules and never will, so the surfaces are:
the simulation's command queue, whose own header already anticipates this — *"Input, a script, a
network peer and a replay all reach the simulation the same way. That is not tidiness: it is
what makes a replay possible at all"*; the published immutable snapshots; the scene, which is
presentation only; the asset system; and the engineering overlay.

**Three facts about the present state.**

- *No caller exists.* Both applications are composition roots driven by command-line flags. The
  only thing resembling a scripting need that has arisen is a deterministic source of synthetic
  commands for headless runs, and that is sixteen lines of C++ in `apps/lab/sim/src/commands.cpp`.
- *"Stable native APIs" is not true this fortnight.* `kHashAlgorithmVersion` went from 1 to 2 in
  M8, changing every stored hash. The `CommandHandler` shape was under active consideration in
  M9's undo work. A binding written against either would already have been rewritten.
- *ADR-0005 has been holding a place for this since M0.* It chose `Result`/`Error` partly with
  "future scripting integration" in mind, and rejected exceptions partly because "they make
  failure paths in a plugin or scripting boundary fragile". The error model a scripting layer
  would need is decided; what is missing is a reason to build the layer.

## Decision

**1. No embedded scripting language in M9, and no new dependency.**

**2. The boundary is decided now and is in force immediately.** Anything outside the tick —
a script, a tool, a network peer, a replay — changes simulation state only by submitting to
`sim::CommandQueue`, reads it only through published immutable snapshots, and **never executes
during a tick**. A script is one more command source, never a system.

This is the load-bearing half of the record. A script that runs outside the tick cannot harm
determinism at all, because its output is commands and commands are recorded: a nondeterministic
script produces a different *recording*, not a divergent replay. A script running inside a
system is the dangerous kind, and it is ruled out here rather than discovered to be impossible
later, when something has already been built assuming otherwise.

**3. The API boundary a future scripting module would have**, recorded so that the trigger below
can be evaluated against something concrete rather than re-litigated:

- A `scripting` module depending on `core`, `simulation` and `assets`.
- Exposed to scripts: `submit(type_name, bytes)`, stamped `current_tick + 1` and attributed to a
  dedicated script `SourceId` so a replay shows where a command came from; `tick()`; and a
  read-only view over the latest snapshot. Sizes bounded by the queue's existing `kMaxPayload`
  and `kMaxPending`.
- Called once per frame on the main thread, between event handling and `TickAccumulator::advance`.

Worth stating plainly, because it is the strongest argument against the whole idea: the snapshot
type and the command encoders are **application**-owned, not engine-owned. `CellSnapshot` and
`encode_set_color_index` live in `apps/lab`. So most of what a script API would expose is the
*application's* API, which is precisely the "game-specific scripting" the charter excludes from
the engine. An engine-level scripting module would bind three functions and leave the
interesting surface to whoever writes the game.

**4. The security model a future scripting module would have**, since the specification requires
it be identified:

- Scripts are untrusted input, exactly like save files. Loaded through `VirtualPath` from a
  mounted root; text chunks only, never precompiled bytecode, which is not validated by the
  interpreter and is a straightforward path to arbitrary execution.
- `io`, `os`, `package`, `debug`, `require`, `dofile` and the `load` family are absent from the
  environment rather than overridden in it.
- An instruction budget through `lua_sethook(LUA_MASKCOUNT)`; on overrun the script is disabled
  and the event logged, never silently retried.
- A memory cap through a custom allocator passed to `lua_newstate`.
- `lua_pcall` at the boundary is the one permitted `try`/`catch` equivalent under ADR-0005, and
  would need an entry in `tools/check_module_deps.py`'s exception allow-list.
- `math.random` removed and replaced by a binding to `sim::RngStream`, so a script cannot
  introduce an unseeded source of numbers.

**5. Determinism analysis, recorded so it is not rediscovered.** If the trigger fires and Lua is
chosen, these are the traps, all of which make a script *inside* a tick worse than it looks:

- Lua 5.4's integer subtype is 64-bit and wraps predictably, but `/` and `^` always produce
  doubles, so a single stray division changes the type of a value flowing into state.
- `math.random` uses xoshiro256\*\* and is randomly seeded at state creation in 5.4.
- String hashing is seeded per state from addresses and the clock, so `pairs()` order over
  string keys varies between runs of the same binary. Fixing that requires overriding
  `luai_makeseed` at build time, which means maintaining a vcpkg overlay port rather than
  consuming the stock one.
- `tostring` on a float goes through the C library's `%.14g`.

Integer-only Lua is achievable, but only by removing operators and library functions — and a
"tiny end-to-end script" would exercise none of that, so it would demonstrate that embedding
works while proving nothing about the part that is hard.

**6. The trigger for reversal. All three must hold.**

- **A recorded limitation.** A consumer needs behaviour at runtime that cannot be expressed as a
  stamped command list. Written in `docs/DEFERRED.md` by whoever hits it, not anticipated here.
- **Stable native APIs**, made concrete: `CommandQueue` and `CommandHandler`, `SnapshotHeader`,
  `kReplayFormatVersion`, `kSaveFormatVersion` and `kHashAlgorithmVersion` all unchanged for two
  consecutive milestones.
- **The edit infrastructure exists**, which it now does, since editor scripting would route
  through `edit::History` rather than inventing a second path.

A soft signal worth writing down: a third command-line flag whose purpose is to shape the
synthetic command stream is a scripting language trying to be born. Two exist today.

## Alternatives

**A tiny end-to-end Lua script now**, as the exit criterion permits. Steel-manned, because it is
the strongest alternative: it would replace `submit_synthetic_commands` with `synthetic.lua`,
prove the sandbox and the exception boundary are real rather than described, force the vcpkg and
continuous-integration churn to be confronted now rather than later (the precedent being M5's
addition of a JSON library), and remove the unknown from the embedding cost.

Rejected because of what it would actually bind: three engine calls that `apps/lab/sim/tests`
already covers, plus lab code that is not engine API. It costs a dependency and a module with
one caller, against the project's own rule of two real call sites before a generalisation. And
writing a script in order to manufacture the "recorded limitation" that the charter requires
before adding scripting inverts that rule rather than satisfying it. If it is chosen anyway: the
raw C API and one dependency, not `sol2` and two.

**A data-driven command file with no language.** Already exists and is already used:
`sim::Replay` is a versioned, hostile-input-hardened list of stamped commands, and `--play`
feeds it. Anything a conditional-free script would do is a replay file with extra steps.
Conditionals are where a script starts being a program, and a program that decides what happens
in the world is the game's rules, which the charter excludes.

**C++ plugins or a plugin ABI.** A charter non-goal by name. The future game links the engine;
"plugin" would mean "another module".

**Python, for offline tooling only.** Already the case — `cook_shaders.py`, `bench_baseline.py`,
`check_module_deps.py` — and it stays that way. Host Python is a build-time prerequisite, not an
engine dependency, and nothing it does runs inside the process.

**WebAssembly** (wasmtime, WAMR, wasm3). Technically the strongest fit for a determinism-critical
sandbox, and this record should say so plainly rather than pretend Lua is the only candidate:
linear memory, fuel metering and bit-specified semantics for integers *and* floats come from the
specification rather than from stripping a language down. Against it: a large dependency, a
separate authoring toolchain for whoever writes the scripts, and the specification named Lua.
**When the trigger fires, WebAssembly is evaluated against Lua before either is adopted, on
determinism grounds.**

## Consequences

- No engine surface is added. `cmake/ModuleGraph.cmake` is unchanged. `docs/DEPENDENCIES.md`
  gains Lua under "considered and not adopted", pointing here.
- The synthetic command source stays C++ and remains the only non-interactive command producer
  besides replay.
- ADR-0005's "future scripting integration" clause stays a placeholder, now with a date on it.
- The boundary in decision 2 constrains work that is not obviously about scripting. M9's edit
  commands are the first case: they are commands too, and they are outside the tick because the
  scene is presentation state, which is why `edit::Command` and `sim::Command` are deliberately
  different types rather than one generalised over both.
- Anyone who wants scripting must first write down what they could not do without it. That is
  the cost of this decision and it is the intended cost.

## Rollback cost

**Low.** Nothing was built, so nothing is thrown away. Adopting later costs a module, a
dependency, an allow-list entry for the `pcall` boundary, and the security work in decision 4 —
all of it additive. Because decision 2 fixes the boundary now, adoption changes no existing
signature: a script becomes another caller of `CommandQueue::submit`, which already exists and
already has other callers.

The cost that would be real is the opposite mistake. Letting a script run inside a tick, or
handing one a `World&`, would end replay and cross-platform hash agreement, and would be found
months later as a divergence nobody can reproduce. That is what decision 2 is for.
