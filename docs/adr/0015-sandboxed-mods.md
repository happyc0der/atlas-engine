<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0015 — Sandboxed mods: WebAssembly, behind the command queue

## Status

**Proposed**, 2026-09-18, to be implemented across the rest of M15.

Proposed rather than Accepted, by this index's own legend: a milestone still has to build it,
which is why ADR-0003 and ADR-0004 were Proposed and why ADR-0009 was not — a deferral has no
implementation step and this has five. It becomes Accepted when M15 closes, and if D2's
fallback trigger fires before then, what becomes Accepted will be a different record rather
than this one quietly re-scoped.

Authorised by
[ADR-0010](0010-charter-amendment.md) D1, which fixed "plugin support" as *sandboxed mods:
untrusted scripts that reach simulation state only through `sim::CommandQueue`*. Inherits
[ADR-0009](0009-scripting-decision.md)'s decision 2 — the command-queue boundary — unchanged,
and supersedes its decision 6. Its decisions 3 and 5 are amended rather than inherited, and
each amendment is recorded below with the reason. The lockstep requirements this record is
answerable to are [ADR-0014](0014-deterministic-lockstep.md)'s.

**Nothing reaches `vcpkg.json` in this record.** The evaluation is the deliverable; the
dependency lands in the slice after this one, behind a second gate, and that slice's first job
is to prove the port builds on all three platforms before anything is built on top of it.

## Context

### What changed since ADR-0009, and why the question is not the one it asked

ADR-0009 deferred scripting and said plainly what would settle it later: *"When the trigger
fires, WebAssembly is evaluated against Lua before either is adopted, on determinism grounds."*
Two things have moved since, and both change the answer rather than merely the evidence for it.

**M14 made runtime determinism a hard requirement, where ADR-0009 had made it a preference.**
Its own reasoning was that a script outside the tick cannot harm determinism, "because its
output is commands and commands are recorded: a nondeterministic script produces a different
*recording*, not a divergent replay". That is still true of replay and is no longer the whole
picture. Under lockstep every peer runs the same mods, and ADR-0014 decided that **a mod's
commands are local to each peer and never sent on the wire** — the hash check is what catches a
mod that decided differently. So the same mod, on arm64 and on x86_64 and under MSVC, must reach
the same decisions in the same order, or peers diverge in their *inputs* and the session stops.
The guest language's cross-platform semantics are now load-bearing.

**The dependency landscape is not what the planning conversation assumed**, and all three of
these were checked against the tree rather than recalled:

- **There is no WebAssembly runtime port in vcpkg.** Not at our pinned baseline `9e593bb`
  (2026-07-29), and not at upstream `master` as of 2026-09-15. No `wasm-micro-runtime`, no
  `wasmtime`, no `wasm3`, at any commit reachable in our own submodule. The one runtime present,
  `wasmedge` 0.13.5, is disqualified on its own terms: `"supports": "!windows"` with Windows a
  tier-one platform, and it pulls Boost and spdlog. **Moving the baseline would not help.**
- **Lua 5.5 no longer needs an overlay port.** ADR-0009 decision 5 refused stock Lua partly
  because the per-state string-hash seed made `pairs()` order vary, and fixing it "requires
  overriding `luai_makeseed` at build time, which means maintaining a vcpkg overlay port rather
  than consuming the stock one". `external/vcpkg/ports/sol2/lua-5.5.diff` rewrites
  `lua_newstate(f, ud)` as `lua_newstate(f, ud, 0)`: **5.5 takes the seed as an argument.** That
  objection has expired, and stock `lua` 5.5.0 is one line and an override.
- **`luau` 0.729 is in the pinned baseline and was never in the comparison.** MIT, no `supports`
  restriction, no transitive dependencies, static-only. It is the one candidate built
  specifically to run untrusted user scripts, with a sandbox mode and an interrupt hook for
  instruction budgets in the library rather than assembled from parts.

So the evaluation is three-way, not two-way, and the cost ordering is the reverse of what the
original framing assumed: the WebAssembly option is now the expensive one to acquire and the
Lua-family options are free.

### The trigger accounting ADR-0010 requires

ADR-0010 recorded the three conditions of ADR-0009 decision 6 and said **"ADR-0015 must restate
this accounting when it lands, against what M14 actually did rather than against what it planned
to do."** Restated, and checked:

1. *A recorded limitation, written by whoever hits it.* **Not met, and not claimed.** Nobody hit
   one. It is replaced by the owner's decision in ADR-0010, exactly as that record says.
2. *`CommandQueue`, `CommandHandler`, `SnapshotHeader`, `kReplayFormatVersion`,
   `kSaveFormatVersion` and `kHashAlgorithmVersion` unchanged for two consecutive milestones.*
   **Met.** Verified rather than assumed: `command.hpp` and `snapshot.hpp` have a zero-line diff
   across M13 and M14 together, `kReplayFormatVersion` and `kSaveFormatVersion` are still 1, and
   `kHashAlgorithmVersion` has been 2 since before M13. M14 added types — `TurnGate`,
   `CommandSource`, `command_codec` — and changed none of the six.
   One qualification, so this is not read as broader than it is: **M15's own opening sweep
   changed `drain`'s tie-break**, adding a comparison on command content below `(source,
   sequence)`. No signature moved and no hash changed, but the ordering of two commands that
   share a key is now defined where it was previously whatever `std::sort` did. That is M15's
   change, not M14's, and it is named here rather than left for someone to find in a diff.
3. *The edit infrastructure exists.* **Met**, since M9.

The soft signal ADR-0009 added — *"a third command-line flag whose purpose is to shape the
synthetic command stream is a scripting language trying to be born. Two exist today."* — has
**not** clearly fired. `--commands-per-tick` and `--seed` shape the stream and `--pick` scripts
an interaction; M14's six new flags shape the *transport* and not the stream. Called a draw
rather than a hit, because reading it as fired would be this record marking its own homework.

## Decision

**D1. The runtime is WebAssembly, hosted by an interpreter-only WAMR build through a vcpkg
overlay port.** This confirms the preference recorded in planning, but it is confirmed against
the three-way comparison below rather than carried forward, which is what this gate existed for.

The decisive argument is one this repository already made, in its own words.
`docs/DETERMINISM.md` says: *"Transcendental functions from libm are forbidden in authoritative
simulation code: their results are not specified to the last bit and differ between platforms."*
A mod that decides what to submit is producing authoritative input, so that rule binds it. In
Lua and in Luau, `math.sin` and `^` are the host's libm, one call away from a mod author, in a
mod that would look entirely reasonable. The only defence is removing library functions — and
ADR-0009 already identified why that defence cannot be trusted: *"a 'tiny end-to-end script'
would exercise none of that, so it would demonstrate that embedding works while proving nothing
about the part that is hard."* A WebAssembly guest has no libm: its transcendentals are compiled
into the module, and they are the same bytes on every machine. The property comes from the
specification rather than from subtraction, and subtraction is what cannot be tested.

Two WebAssembly features are non-deterministic by specification and are therefore switched off
rather than avoided: **relaxed SIMD** (SIMD is disabled outright) and **NaN payload
propagation**. The second is not fully closed by an integer-only boundary — a guest could
branch on reinterpreted NaN bits internally — so it is recorded as a known residual hazard with
its detection: the hash check stops the session, and the divergence is attributed.

**D2. The named fallback, with a concrete trigger, is Luau.** The overlay port is the real risk
in this decision and pretending otherwise would be dishonest: this project has never maintained
one, and the port must build on macOS arm64, Linux x64 with clang-19, and Windows x64 with MSVC.
**If it cannot be made to do so within the next slice, the decision falls back to Luau**, on a
stock port, with an integer-discipline profile, and with mods **documented as unsupported under
lockstep** until a cross-architecture golden test exists for the profile. That trigger is
written here so the fallback is a decision already taken rather than one made under pressure
halfway through a milestone.

**D3. The module is `engine/script`, deps `core;simulation;assets`.** ADR-0009 decision 3 called
it `scripting`; the module table's names are short and this record settles on `script`. No `net`
edge: `sim::CommandSource` lives in `simulation` precisely so a mod need not depend on
networking for an interface about the command queue.

**D4. A mod is called once per kernel tick, before that tick — not once per frame.** This
**amends ADR-0009 decision 3**, which specified "once per frame on the main thread, between
event handling and `TickAccumulator::advance`". Under lockstep peers run different numbers of
frames per tick, so a per-frame call would make the number of mod invocations depend on frame
rate, and therefore the commands, and therefore the state. The amendment is not a refinement of
the old wording; it contradicts it, and says so.

**D5. A mod reads a tick-boundary view of the world, never the presentation snapshot.** This
**amends ADR-0009 decision 3** again, which specified "a read-only view over the latest
snapshot". The snapshot channel is latest-wins, published once per frame, and — in the only
application that has one — published only when there is a graphics device. So it is absent
headless, which every proof and integration case requires, and its age depends on frame rate,
which is the divergence D4 exists to prevent. Instead the application registers named read-only
byte views computed from the `World` at the tick boundary.

They are **byte views and not typed ones**, and ADR-0009's own strongest objection is why:
*"the snapshot type and the command encoders are application-owned, not engine-owned"*. The
survey confirms the engine has nothing to offer here even if it wanted to — `sim::Table` exposes
`row_count`, `hash_into`, `write_to`, `read_from` and `clear`, and no accessor of any kind. The
engine provides the mechanism and the bounds; the application provides the bytes; anything typed
is the game's, per ADR-0010 D7.

**D6. The restriction M14 deferred lives here.** M14 built a command source bound to a single
identifier, found it wrong for a lockstep session, removed it, and recorded that the constraint
belongs at the boundary with the untrusted thing. That boundary is the mod host. It is enforced
by the ABI rather than by a check: **`atlas_submit` takes a type and bytes and no source at
all**, and the host stamps identity and target tick. Naming another peer is unrepresentable to a
guest rather than forbidden to it. The mod's own identifier has bit 31 set, as ADR-0014
anticipated — which until now has been prose and nothing else, since `SourceId` is a bare
enumeration with no reserved range.

**D7. Limits, each with its reason.** Linear memory and instruction count per tick, as two
independent bounds, following `net::CommandInbox`'s rule that *"either limit alone is not a
limit"*. **The instruction budget is counted in instructions and never in wall time**: a
wall-clock watchdog fires at different points on different machines, which under lockstep is
divergence rather than safety. Log output and commands per tick are bounded too. All runtime
allocation goes through a counted allocator local to `script`; there is none in `core`, and
`docs/DEFERRED.md` records that absence deliberately, so this one does not pretend to be
general.

**D8. The failure policy is device loss, not recovery.** A trap, an exhausted budget, a refused
allocation, a bad pointer or a non-zero `mod_init` **disables that mod for the session and logs
once**. Other mods continue. The engine never stops because a mod did, and it never restarts one
in the hope that it behaves differently, because under lockstep "tries again" is a decision one
peer might make and another might not.

**D9. Precompiled bytecode is accepted, which reverses part of ADR-0009 decision 4.** That
decision said "text chunks only, never precompiled bytecode, which is not validated by the
interpreter and is a straightforward path to arbitrary execution" — true of Lua, whose bytecode
loader trusts its input. It is the opposite for WebAssembly, where the binary format *is* the
distribution format and validation before instantiation is specified behaviour. The reversal is
recorded because the sentence it reverses is otherwise still good advice.

**D10. `script::Runtime` is the second process-wide object, and the only one this record
adds.** CLAUDE.md permits exactly one, the log sink registry. WAMR has a process-global
initialisation, so `Runtime` is an RAII object the composition root creates once and asserts is
unique, exactly as `Platform` wraps SDL's. **This is the one place the rule bends**, and naming
it here is the price of bending it. If the Luau fallback is taken this consequence disappears,
because it has no global initialisation — a point in its favour that did not outweigh the rest.

## Alternatives

**Lua 5.5, on the stock port.** The cheapest option by a wide margin: one line, an override, MIT,
no transitive dependencies, nothing to maintain. Its 5.5 seed argument retires ADR-0009's
overlay-port objection, and the `cpp` port feature answers the other structural worry — compiled
as C++, its error path unwinds with exceptions and runs destructors, where Lua-as-C uses
`longjmp` over the host's RAII frames, which is undefined behaviour and a direct conflict with
this project's ownership rules. Rejected on the libm argument in D1, and on ADR-0009's own
observation that an integer-only profile is exactly the part a demonstration script never
exercises. **It remains the right answer if mods are ever scoped to single-machine play**, and
that is its trigger.

**Luau 0.729.** The strongest of the three on sandboxing, and it is not close: it exists to run
untrusted user scripts, it freezes globals rather than deleting them one by one, its instruction
budget is a library facility instead of a hook to assemble, and its standard library omits
`io`, `os` and `package` by construction rather than by stripping. It is also stock, MIT,
all-platform and dependency-free, and its error handling is C++ exceptions contained inside
`lua_pcall`. Rejected on determinism and on nothing else: **every Luau number is a double**,
with no integer subtype at all, so arithmetic that looks integral is exact only to 2⁵³ and the
libm surface of D1 remains. Kept as the named fallback in D2, which is a stronger position than
"considered and rejected" and is meant to be.

**WasmEdge**, the one WebAssembly runtime actually in the baseline. Rejected on its port
metadata rather than on its merits: `"supports": "!windows"`, and Boost and spdlog as transitive
dependencies for a sandbox.

**Adopting two — WebAssembly for determinism and Lua for authoring.** Rejected, and worth naming
because it is the tempting compromise: two sandboxes, two budgets, two threat models, and twice
the surface that has to be proven safe. The authoring problem has a better answer that costs no
second sandbox — a Lua interpreter compiled to wasm32 runs *inside* this one, where even its
address-dependent behaviour becomes deterministic because linear memory is deterministic. That
is deferred with its trigger, which is a mod author who asks.

**Keeping the deferral.** ADR-0009's position, and it was the right one for a year of work.
Overtaken by ADR-0010's decision rather than by argument, and recorded that way.

## Consequences

**The authoring story gets worse before it gets better, and this is the real cost.** Strategy
game modders are mostly not compiler users. A WebAssembly mod needs AssemblyScript, Rust, or C
through clang, and Atlas's own demonstration mod will be written in WAT because forty lines of
it is honest and a toolchain in the build is not. ADR-0009 called the authoring case "the
strongest argument for Lua and it is real"; it still is, and it lost to determinism rather than
being answered.

**The project acquires a vcpkg overlay port and owns it for ever.** Every compiler bump, every
baseline move and every new platform is ours to fix, and CI's binary-cache key
(`hashFiles('vcpkg.json', '.gitmodules')`) does not cover overlay sources, so that key must be
widened in the same change or a stale cache will serve a stale port.

**A committed `.wasm` fixture inherits M13's trap.** CLAUDE.md: *"A `--check` that compares
bytes must compare bytes only this repository decides."* A `.wasm` produced by `wat2wasm` is
third-party compiler output, exactly like the SPIR-V that passed by coincidence from M2 to M13.
The escape already exists and is copied rather than reinvented: record the tool versions, compare
bytes only when they match, skip loudly otherwise. A Lua or Luau mod would be source text and
would need no such check at all, which is a cost on this side of the decision and is counted here.

**`docs/DEFERRED.md` gains M15's section**: mod threads, hot reload of mods, a mod manager panel,
Lua-compiled-to-wasm as an authoring route, mod signing, per-mod storage, and typed snapshot
views in the engine — the last of which is the game's, per ADR-0010 D7.

## Rollback cost

**Low in code and high in commitment, which is the opposite way round from ADR-0014.**

The code is contained: `script` is a leaf module nothing else depends on, the mod host is one
`CommandSource` among what will be several, and the engine runs identically with no mod loaded —
which is the configuration every existing test already uses and which the golden hashes pin.
Removing the module is deleting a directory and three table entries.

The dependency is the part that does not roll back cheaply. An overlay port is a standing
maintenance obligation, and the fallback in D2 exists because the moment to discover that
obligation is unaffordable is the slice that takes it on, not a milestone later. If that trigger
fires, the fallback costs a rewrite of the host and the ABI's implementation but not of the
boundary, the limits, the loader's shape, or the failure policy — all of which are decided here
and are runtime-independent on purpose.
