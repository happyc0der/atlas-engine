<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0023 — Mods are authored in freestanding C, and a mod may play chess

## Status

**Accepted**, 2026-09-26, implemented in M26.

Proposed on 2026-09-25 at M26's gate. **The decisions held; five of them gained or changed a
detail by being built**, each marked at its own heading rather than edited to read as if it had
always said it: D2's features are named, D4's fourth comparison needed a chess binary and the
Linux build lanes, D6's application check has no call site, D7's layout lives in one header both
languages read, and D8's quota came down from four to two once the worst tick was measured.
Building it also found two defects in the engine's mod host, older than this record, recorded
under Context.

Answers the question [ADR-0015](0015-sandboxed-mods.md) left open by design, and that
[ADR-0018](0018-chess-probe.md) D7 said a mod opponent would force. Supersedes nothing: every rule
about what a mod may reach stands, and the guest interface does not change.

*2026-09-26:* **the opponent and its rules left with chess** for its own repository
([ADR-0024](0024-install-and-export.md)). The toolchain stayed and grew: `tools/build_mods.py`
reads a list of mods rather than a table of its own, gives a mod an include directory holding
`atlas_mod.h` and nothing else, and ships in the installed package; the engine's own compiled mod
is now `painter.wasm`, whose behavioural check runs it in the lab (ADR-0024 D8). Rebuilt that way,
the opponent was the module M26 committed, byte for byte. D6 to D9 are chess's and are carried on
in atlas-chess.

## Context

**Nothing non-trivial can be authored for the sandbox.** Every mod in the tree is written byte by
byte by `tools/gen_mods.py`, which knows sixteen opcodes and has no loops, no branches that
repeat, and no locals. That was M15's honest choice: a toolchain in the build would have been a
dependency for three demonstration modules, and hand-written bytes are bytes this repository
decides, so their check can compare them exactly. ADR-0015 named the authoring routes a real mod
would need — AssemblyScript, Rust, or C through clang — and chose none.

ADR-0018 predicted that a chess opponent would expose this, and the M18 survey confirmed it: a
legal-move generator cannot be written that way. `docs/DEFERRED.md` recorded the milestone with
its condition, *M22 done, and the toolchain chosen by its own record*. The owner scheduled it on
2026-09-25 and chose, among the options put to them: C through clang; a mod carrying its own
rules and a two-ply search; local play only; and Homebrew's `lld` for the linker on this machine.

### What the survey found

- **The compiler is here and the linker is not.** Homebrew LLVM 23.1.1 targets wasm32 and
  compiles `atlas_mod.h` cleanly. It ships no `wasm-ld`. No workflow in CI can build wasm, but
  the lint workflow already adds apt.llvm.org's LLVM 23 repository for its formatter.
- **A committed compiler output walks into M13's trap.** Its bytes are the compiler's, not this
  repository's, so a check comparing them passes by coincidence until two machines disagree.
  `tools/cook_shaders.py` already solves this for shaders, and ADR-0015 prescribed the same for a
  compiled mod: record the tool versions, compare bytes only when they match, skip loudly
  otherwise.
- **The runtime is configured narrowly.** WAMR 2.4.5 runs its classic interpreter with
  instruction metering, bulk memory and reference types on, and SIMD off. LLVM 23's default
  wasm32 features include some the runtime has never been shown to accept.
- **The chess rules refuse a mod as a player, on purpose.** `chess.players` refuses a source
  with bit 31 set when a save loads, because mod identifiers are never sent on the wire. Written
  in memory it works, but a saved game against the mod would not load.
- **A mod's commands land at least one tick later** than a person's, and local chess steps only
  when a person moves. A game with a mod needs a tick-driven loop.

### What building it found, which predates this record

*Added during M26, 2026-09-25.* **The first compiled module exposed two defects in the mod host,
both from M15.** WAMR sorts the import array it is given in place, and the host handed it one
shared array whose first nine entries were the safe set: a runtime created with the debug clock
sorted all ten, and the next runtime in the same process then offered the clock and lost
`atlas_view_size`. No program creates two runtimes, so nothing shipped with it; the tests do, and
failed one run in three. The host now hands WAMR fresh copies. And the host passed a module's
declared maximum memory as an override, which WAMR's loader had already reshaped for a module that
never grows its memory, so every load of a compiled module drew a warning. The host now leaves the
maximum to the module, which its own check has already bounded.

A smaller finding is recorded, not fixed: `atlas_command_type` returns a type's hash as a signed
integer, so a type that hashed to −3 would be indistinguishable from `ATLAS_ERR_REFUSED`.

## Decision

**D1. Mods are written in freestanding C, compiled to wasm32 by clang and linked by wasm-ld.**
No libc: `-ffreestanding -nostdlib`, with the few functions a compiler may call for itself
provided by the mod. `atlas_mod.h` is the guest's only header from the engine, which is what it
was written to be. C, because it is the language this repository's own tools already understand,
it needs nothing beyond LLVM, and what it compiles to is close enough to the source that a
determinism question can be answered by reading it.

**D2. The target features are pinned, never defaulted.** `-mcpu=mvp` plus exactly the features
the runtime is shown to accept, written into the build script. A compiler upgrade must not start
emitting an instruction the runtime refuses, and a default feature set is a promise the compiler
makes about itself rather than about this runtime.

*Made precise during M26, 2026-09-25.* The features are bulk memory, sign extension and mutable
globals over `mvp` — what the runtime is built with or accepts, and what the compiled module is
proved to load with. The build also strips every name and custom section, so nothing in the module
records where it was built, and two builds from different directories are identical.

**D3. The compiled module is committed, with a manifest.** `tools/build_mods.py` compiles and
links a mod — no entry point, the four exports ADR-0015 requires, a declared maximum memory
within the sixteen-mebibyte cap, a stack within sixty-four kibibytes — and writes the module
and a manifest of the clang and wasm-ld versions and a hash of every source. A person running
the chess app needs no compiler, exactly as a person drawing a frame needs no shader compiler.

**D4. Its check compares three things, and says which.**
1. **The source hash, always**, whatever machine runs it: a source edited without a rebuild fails.
2. **The bytes, only where the toolchain matches the manifest.** Anywhere else the comparison is
   skipped, loudly, naming both versions — the shader rule.
3. **Behaviour, wherever a toolchain exists.** The check rebuilds the module with the local
   toolchain and requires the rebuilt module and the committed one to play the same headless game
   to the same final hash. This is what lets CI verify a module it cannot reproduce byte for byte,
   and it is the second remedy CLAUDE.md names: compare something other than bytes.

*Made precise during M26, 2026-09-26.* The check has four parts, not three: the committed module
is also compared with the hash its manifest records, on every machine, so a module replaced by
hand fails without a compiler. And the behavioural comparison needs a chess application as well as
a toolchain, which no single CI job had: the lint job has the toolchain and no binary, the build
jobs the binaries and no toolchain. **So the Linux build lanes install `clang-23` and `lld-23`**,
and CTest passes the lane's own `atlas_chess` to the check, which rebuilds the opponent and plays
it against the committed one. A module built differently but matching its manifest passes the
byte-skipping part and is caught by this one, which is shown.

**D5. CI builds it.** The lint workflow installs `clang-23` and `lld-23` from the repository it
already adds, and runs the check as it runs the shader check. The three hand-written mods and
their exact check are unchanged.

**D6. `chess.players` may hold a mod.** A seat may be held by `sim::mod_source(i)`, and loading
one is accepted. The refusal existed because a mod's identifier is never sent on the wire; it is
nonetheless the same on every machine, a pure function of the mod's index, and a saved file is
not the wire. A save that names a mod loads only when that mod is attached to the same seat,
which the chess app checks and refuses with its own message. Hot-seat and socket games leave
both seats as they are, so every golden hash holds. This is chess's rule, in `apps/chess/sim`,
and no engine code changes for it.

*Changed during M26, 2026-09-26.* **The application check has no call site**, because the chess
application neither saves nor loads a game; recorded in `DEFERRED.md` with its trigger. The table
accepts any identifier, a mod's included, because one that nobody runs only stalls a side. A test
saves a game against the mod, loads it into a fresh world with the mod attached, and finishes at
the uninterrupted game's hash. It saves on the person's turn: a save does not hold a mod's memory,
so one taken mid-search would restart the search at a different tick, which is also recorded.

**D7. The mod reads bytes the chess app defines.** Two views: `chess.position`, the sixty-four
squares in the board table's own encoding followed by side, castling, en passant, the two clocks
and the outcome, written byte by byte and little-endian; and `chess.seat`, the colour or colours
the mod plays. They are built in `apps/chess/sim`, which needs only the world. Nothing typed
crosses into the engine (ADR-0015 D5).

*Made precise during M26, 2026-09-26.* The layout is one C header,
`apps/chess/sim/include/atlas/chess/mod_view.h`, of macros both sides include — the C mod and the
C++ builder — and `tools/build_mods.py` hashes it into the mod's manifest, so neither side can
change it without the other failing. Seventy-two bytes and one.

**D8. The opponent does a fixed amount of work per tick.** It carries its own legal-move
generator, verified by compiling the same C for the host and running perft against the published
counts and against `chess_sim` itself. It searches two plies on material, and **evaluates a fixed
number of root moves each tick**, keeping its state in linear memory between ticks, then submits
the best as a `chess.move` with ties broken by `atlas_random`. A quota counted in moves bounds the
worst tick's instructions, which is measured against the budget rather than assumed, and keeps
the decision identical on every machine. It never resubmits for a position it has answered.

*Changed during M26, 2026-09-26.* **The quota is two moves a tick, not four.** At four the worst
tick measured 2.16 M instructions, inside the prediction and 4.6 times under the budget — on the
positions measured. One root move facing the most replies any position allows would cost about a
million, and four of them would leave a margin of two and a half. At two the measured worst is
1.16 M and that extreme stays near two million. A leaf where the mod is in check is also asked
whether it has a move, so a capture that walks into mate is seen, which the first version missed
and a mutation found.

**D9. Local play only.** A person against the mod, or the mod against itself headless. Playing the
mod over a socket is deferred with its trigger.

## Alternatives

**AssemblyScript.** Friendlier to a modder than C. Rejected because it brings Node and npm into
the build and CI, and its compiler is an npm package whose output moves between releases without
a version a manifest could pin as simply as a compiler's.

**Rust.** Nothing is installed here or in CI; it would need rustup, the wasm32 target and a
pinned toolchain everywhere. The heaviest option, for no property C lacks at this size.

**Lua compiled to wasm, as ADR-0015 sketched.** It still needs a C toolchain to build the
interpreter, so it is this decision plus an interpreter. Its trigger — a mod author who will not
write C — has not fired.

**Growing `gen_mods.py` into an assembler.** Every byte would stay this repository's, and the
check exact. Rejected because a chess move generator in hand-assembled WebAssembly is very large,
and nobody else would ever author a mod that way, so it would answer the milestone's question
with "no".

**A host import that returns legal moves.** The mod would need no rules of its own. Rejected
outright: an import that knows chess puts chess in `engine/`, which ADR-0018 forbids, and the app
publishing the moves as a view would leave the authoring gap exactly where it is.

**Emscripten's bundled `wasm-ld`**, which is already installed. It works, but it is LLD 24 beside
clang 23, so the manifest would never match CI's and the byte comparison would never run here.

## Consequences

- `tools/build_mods.py`, `assets/mods/chess_opponent.wasm` and its manifest; a lint test and a
  step in `lint.yml`; a row in `assets/source/PROVENANCE.md`. Homebrew `lld` on this machine.
- `apps/chess/mod/` holds the opponent and its rules library, with a native test target.
- `apps/chess` links `atlas::script` for the first time; the lab's mod wiring moves to
  `apps/common`, as its second call site requires. `chess_sim` gains the view builder and the
  players-table rule.
- The first module larger than 295 bytes fires `DEFERRED.md`'s load-measurement trigger, and
  the measurement is taken with its prediction written first.
- `CLAUDE.md`'s mod rules gain one: mods are written in freestanding C, and a compiled module is
  checked by source, by bytes where the toolchain matches, and by behaviour.
- **The risks are two, and each has a slice.** That WAMR refuses something clang emits, which slice
  one proves on a trivial module before anything is written on top. And that the opponent's worst
  tick is closer to the budget than a quota suggests, which slice three measures by lowering the
  budget until the mod traps.

  *Corrected during M26, 2026-09-26:* Homebrew's `lld` depends on its `llvm`, so installing it
  moved this machine's LLVM from 23.1.1 to 23.1.2, which is also the clang-tidy the precheck
  uses. The Linux build lanes install the toolchain as well as the lint job (D4).

## Rollback cost

Low for the toolchain: deleting the script, the committed module and the CI step returns the
repository to hand-written mods, and nothing in the engine depends on how a module was made.
Moderate for chess: the players-table rule would revert, and a save of a game against the mod
would stop loading, as it does today.
