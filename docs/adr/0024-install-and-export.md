<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0024 — The engine installs as a package, and chess moves to its own repository

## Status

**Proposed**, 2026-09-26, at M27's gate. Nothing is installed, exported or moved under it before
it is read.

Fires the trigger [ADR-0018](0018-chess-probe.md) D6 recorded for lifting chess out: *"the engine
gaining an install and export target"*. Supersedes nothing. When built, it amends ADR-0018 D1,
because chess stops being an exception in this tree and starts being a consumer outside it.

## Context

The charter's v1.0 criterion is a game project that **links the engine, loads data and runs a
deterministic simulation without patching engine internals** (`PROJECT_CHARTER.md`, "After
v0.1"). ADR-0018 D2 explained why chess in `apps/chess` cannot prove that: a game inside the tree
can see everything, so it cannot show that the *public* surface is sufficient. D6 deferred moving
it out, because without an install target an outside consumer would be a submodule pointing at a
source tree. **Nothing in the build installs or exports anything today.**

The owner asked for the next milestone on 2026-09-26 and chose this one. Among the options put to
them, they chose:
- a new public repository, `happyc0der/atlas-chess`, starting fresh;
- third-party libraries delivered as an SDK prefix rather than a second vcpkg setup;
- `apps/common` installed as an app-kit component;
- a proof on both sides;
- version 0.27.0;
- an engine-owned fixture mod, so the mod toolchain stays exercised here.

### What the survey found

- **Most of the export already exists.** `atlas_add_module` writes include directories as
  `$<BUILD_INTERFACE:…>` and `$<INSTALL_INTERFACE:include>` (`cmake/AtlasModule.cmake:66-71`).
  Every third-party library is linked PRIVATE, so a static export names it only as
  `$<LINK_ONLY:…>`. Warnings, sanitizer flags and floating-point flags are PRIVATE and reach no
  consumer. The presets already set an `installDir`. No public header includes a generated file.
  The only third-party include in any public header is Tracy, behind `ATLAS_TRACY_ENABLED`, in
  `core/profile.hpp`.
- **Five things would stop an export today, and none of them is chess:**
  - `atlas_add_app_library` writes raw absolute include paths (`AtlasModule.cmake:210-212`),
    which `install(EXPORT)` refuses.
  - `atlas_platform_internal` and `atlas_rhi_internal` have include directories that exist only
    in the build tree. `rhi` links `platform_internal` PUBLIC only because `DEPENDS` always links
    PUBLIC. Only `rhi/src/device.cpp` and `tools/src/debug_ui.cpp` include those headers.
  - `atlas::sdl3` is a first-party wrapper that chooses SDL3's static or shared target.
  - MSVC's `/std:c++latest` is a directory setting (`CMakeLists.txt:47-51`), so a consumer would
    compile against the libraries under a different standard. ADR-0001 records why that standard
    is not ABI-stable.
  - Three headers are missing from their modules' `HEADERS` lists (`core/time.hpp`,
    `core/handle.hpp`, `renderer/shader_loader.hpp`), so an install driven by those lists would
    leave them out.
- **The vcpkg tree can be moved.** Every package's config under
  `vcpkg_installed/<triplet>/share` locates itself relative to its own file. None contains an
  absolute path, and each records both Debug and Release locations. So a consumer can put that
  directory straight onto `CMAKE_PREFIX_PATH`.
- **Chess touches only public engine headers.** It includes nothing from `src/`, `internal/` or
  the engine's test helpers, and nothing under `engine/` or `cmake/` mentions chess. What ties
  chess to this tree is:
  - **Build machinery:** `atlas_add_app`, `atlas_add_app_library` and `atlas_add_test`.
  - **One test include path** built from `PROJECT_SOURCE_DIR`.
  - **Data locations:** asset, shader and mod defaults relative to the working directory, and the
    engine's `en.json` read from chess's own strings directory.
  - **Shared tools:** a texture generator that also draws the sandbox's sheets, `build_mods.py`
    (a chess-only table, and a manifest of repository paths), and the integration harness it
    shares with the lab.
- **Two things outside chess depend on a chess asset.** `bench_script` loads
  `chess_opponent.wasm` as its large module, and the mod check plays with `atlas_chess`.

### Predictions, written before anything is built

Each is scored in M27's report, as ADR-0018's were.

1. The first `install(EXPORT)` fails on `platform_internal`, `atlas::sdl3` and the app libraries'
   include paths, and on nothing else.
2. Chess's first configure against the install fails only on the build machinery, the
   `PROJECT_SOURCE_DIR` test include and the missing harness. No engine header is missing.
3. Chess's first run fails `--text-check` and the window cases, because its data paths are
   relative to the working directory.
4. On MSVC, without the exported flag, a consumer compiles under a different `/std` and still
   links. Only an assertion on `_MSVC_LANG` catches it.
5. On Windows, a consumer's executables fail at start-up until the DLLs are copied beside them.
6. The golden hashes are unchanged in both repositories, because installing changes no code.
7. The fixture mod changes one benchmark row and nothing else.
8. No public header needs `_CRT_SECURE_NO_WARNINGS`.
9. A chess CI run whose cache misses takes about forty minutes, most of it vcpkg building the
   dependencies from source.

## Decision

**D1. The package is `Atlas`, version 0.27.0, and the minor number follows the milestone.**
- The namespace is `atlas::`, the aliases every target already has.
- The version's minor number is the milestone that last changed what is installed. So a pin
  says which engine milestone it was built against.
- Compatibility is `SameMinorVersion`: while the major version is zero, any minor release may
  break the installed API, and a request for 0.27 accepts only 0.27.x.
- This record does not declare v1.0. Whether the charter's test is met is for M27's report to
  say and for the owner to decide.

**D2. Every engine module is installed, as the static library it already is.**
- The exceptions are `runtime`, which has no directory, and the two `*_internal` targets.
- Headers are installed by directory rather than from `HEADERS` lists, so the three missing
  headers are included, and so is `atlas_mod.h`.
- `atlas::sdl3` is exported because the export needs it, but it is not API.
- The export set is `AtlasTargets`, and the files are `cmake/AtlasInstall.cmake` and
  `cmake/AtlasConfig.cmake.in`.

**D3. Internal stays internal, and the build says so.** `atlas_add_module` gains
`INTERNAL_DEPS`.
- `INTERNAL_DEPS` is checked against `cmake/ModuleGraph.cmake` like any other edge. It links
  `PRIVATE $<BUILD_LOCAL_INTERFACE:atlas::<dep>>`, which the export leaves out.
- `rhi` and `tools` move their internal dependency to it.
- No `internal/` header is installed. A consumer that includes one fails to compile, and one
  that names the target fails to configure.
- The rules about where SDL types may appear are unchanged.

**D4. `apps/common` is installed as the `app` component.**
- `find_package(Atlas COMPONENTS app)` loads a second export set, `AtlasAppTargets`, holding
  `atlas::app_common`, `atlas::app_lockstep` and `atlas::app_mods`.
- It stays outside the engine module graph, as `atlas_add_app_library` has always said, and is
  versioned with the engine.
- It has two real consumers, the lab here and chess outside, which is the bar for sharing
  rather than copying.
- `atlas_add_app_library` writes its include directories with `BUILD_INTERFACE` and
  `INSTALL_INTERFACE`.

**D5. The config file checks what can go wrong in someone else's build.**
- It calls `find_dependency` for SDL3, nlohmann_json, EnTT, unofficial-enet, iwasm, imgui and
  Threads. A failure names the `vcpkg_installed/<triplet>` directory to add.
- It records the triplet, compiler, build type and macOS deployment target Atlas was built
  with, and stops the configure on:
  - a different compiler;
  - on MSVC, a different build type, because the C runtime would not match;
  - a lower deployment target.
- It sets `Atlas_DATA_DIR`.

*Made precise during M27, 2026-09-26.*
- **It also requires every dependency to come from one tree.** When the vcpkg tree was missing
  from the path, SDL3 was found in Homebrew's copy on this machine, and only nlohmann_json's
  absence stopped the configure. A tree that lacked one package would have linked a stranger's
  build of it without a word. `cmake/Dependencies.cmake` guards against the same thing inside
  this tree.
- **Threads is not on the list.** No Atlas target names it, and a dependency that needs it
  finds it in its own config.
- **"A different compiler" means a different compiler ID or major version,** as D9's contract
  says.
- **The MSVC check is made only for a single-configuration generator.** A multi-configuration
  generator chooses its configuration after the config file has run, so the linker's own
  runtime-mismatch check (LNK2038) is what stops it there.

**D6. MSVC's language standard is part of the interface.** `atlas_core` gains an INTERFACE
`/std:c++latest` for C++ sources on MSVC, so every consumer compiles against the libraries under
the standard they were built with. The root setting stays for this tree's own targets.

**D7. Only a plain build installs, one configuration per prefix.**
- A profile build or a sanitizer build refuses to install and says why. The sanitizer flags are
  PRIVATE, so a consumer could not link. A profile build puts Tracy into every consumer's
  compile through `core/profile.hpp`.
- Both limits, and a prefix holding several configurations, go in `DEFERRED.md` with their
  triggers.

**D8. Data ships in `share/atlas`.**
- The installed data is the sprite shaders, the engine's `en.json`, and `tools/build_mods.py`.
  The lab's own shaders are not installed.
- `build_mods.py` becomes generic. A consumer passes its list of mods and a root. The script
  finds `atlas_mod.h` relative to itself, and the manifest records it as
  `atlas/script/atlas_mod.h` rather than as a path in this repository. So moving a pin across a
  change to the guest interface fails the consumer's check, which is the point of that check.
- The engine's defaults stay relative to the working directory: consumers pass
  `Atlas_DATA_DIR`. Changing an engine default would be a change to public behaviour made for
  one consumer.
- ADR-0023's toolchain stays exercised here through a small fixture mod the engine owns. It has
  no game meaning, is built and checked as the opponent was, and replaces `chess_opponent.wasm`
  in `bench_script`.

*Made precise during M27, 2026-09-26.*
- **A mod list is a JSON file** (`atlas-mod-list`, version 1): the output directory, the manifest,
  and each mod's sources, headers, memory, and how to run it for the behavioural comparison — the
  arguments to pass and the lines of output to compare. The engine's is
  `assets/source/mods/mods.json`, chess's is `apps/chess/mods.json`, and chess's manifest is now
  `chess_manifest.json`. The manifest format is version 2.
- **A mod can reach no engine header but `atlas_mod.h`, mechanically.** The compiler is given an
  include directory holding that header and nothing else. A mod that includes another engine
  header fails to compile, which was tried. Rebuilt this way, the chess opponent is byte for byte
  the module M26 committed.
- **The fixture is `painter.wasm`**: the C counterpart of `synthetic.wasm`, which recolours one
  cell of the lab's grid a tick and reads both of the lab's views to choose a colour the cell does
  not already have. Its behavioural check runs it in the lab. It is 457 bytes, so `script/load`
  no longer measures a large module; `docs/PERFORMANCE.md` has what the row measured before and
  now, and a prediction that missed for a reason worth reading.
- **Chess's engine data is two defaults, not one directory.** In this tree the sprite shaders and
  the engine's `en.json` live in different folders, so the build sets
  `ATLAS_CHESS_SHADER_DIR` and `ATLAS_CHESS_ENGINE_STRINGS_DIR`, and `--engine-data-dir` names a
  directory laid out as `share/atlas` for both. Built against a package, both point into
  `Atlas_DATA_DIR`. An integration case proves the engine's table is read from there.

**D9. The SDK is two prefixes and a contract.**
- A consumer puts two directories on `CMAKE_PREFIX_PATH`: the Atlas install and the exact
  `vcpkg_installed/<triplet>` tree it was built with. It has no vcpkg of its own.
- It uses:
  - the same compiler family and major version, and the same standard library;
  - the same triplet, and so on macOS the same deployment target;
  - on MSVC, the same build type.
- On Windows it copies `$<TARGET_RUNTIME_DLLS:…>` beside its executables.
- `tools/sdk.py <preset>` installs Atlas and prints both paths and these rules.
- Catch2 is in the tree only because it is an unconditional dependency of this repository. A
  consumer that tests with it is relying on that, and this record says so.

*Made precise during M27, 2026-09-26.* **A consumer also turns off CMake's scan for C++20
modules.** Atlas is headers, and turns the scan off for itself in its root `CMakeLists.txt`, but
that is a directory setting a package cannot carry. Left on, the scan runs under Ninja with clang
or GCC and needs `clang-scan-deps`, and the Linux container's clang has none: every compile of
the package test failed there first. AppleClang is never scanned, so macOS could not show it.

*Decided by the owner, 2026-09-26.* **A consumer compiles its own code with `-ffp-contract=off`
on Clang and GCC and `/fp:precise` on MSVC. That is a rule of the contract, and the package does
not impose it.** Atlas compiles itself so from M0 (`docs/DETERMINISM.md`), and the flags are
PRIVATE, so the package does not pass them on. Nothing the engine itself hashes depends on a
consumer's flags. But a game's systems are the consumer's code: a game with a float in its
authoritative state, built without the flag, contracts multiply-add into a fused instruction on
arm64 and not on x86_64, and two peers diverge. An INTERFACE option on `atlas_core` would change
how every consumer's code compiles, float or not, which is a larger thing for a package to do
than to state a rule. atlas-chess, whose state is integers, sets both flags anyway in its
`cmake/ChessBuild.cmake`. `tools/sdk.py` prints the rule with the rest of the contract.
Revisited if a consumer is found diverging for want of it.

**D10. The package is proved from both sides.**
- **Here:**
  - A separate CMake project in `tests/package/` finds Atlas in an install prefix with no vcpkg
    toolchain and no package registry. It runs a golden scenario, a script runtime, a loopback
    link, a string lookup from `Atlas_DATA_DIR` and one app-kit call.
  - It also compiles every installed header on its own.
  - A guard fails it if any of its include paths lies in this source tree outside the prefix.
  - It is a CTest case labelled `package`, so every build lane runs it.
- **There:** chess is built against a pinned Atlas and runs every test it had here, including
  the Opera Game's three golden hashes.

**D11. Chess moves to `happyc0der/atlas-chess`, and is deleted here only after it is green there.**
- The repository is public and GPL-3.0-or-later. Its first commit names the atlas-engine SHA it
  came from, and the history stays here.
- It builds with plain CMake against the SDK. It pins the engine by SHA and checks that the
  binary's `--version` reports it. Its CI builds that SHA's SDK, cached per SHA.
- Its rules library keeps its fence: `chess_sim` links `atlas::simulation` and nothing else.
- When it is green, `apps/chess`, its assets, its integration cases and its benchmark leave this
  repository, and `CLAUDE.md`'s exception becomes a pointer to the repository.
- Creating the repository and every push happen on the owner's word.

## Alternatives

**Chess with its own vcpkg manifest.** It would mirror Atlas's baseline, overrides and the WAMR
overlay port. Rejected because two repositories would then resolve the same pins independently,
and an ABI mismatch between them would link and fail at run time. The SDK prefix makes the match
true by construction. Its cost is that the engine's pins decide chess's library versions.

**Atlas as a vcpkg port.** This is the conventional route. Rejected for now because it needs a
port file, a registry and a version bump for every release, to serve one consumer. Its trigger
is a consumer that does not want Atlas's pins.

**A submodule of this source tree.** Rejected by ADR-0018 D6 for the reason D10 exists: a
consumer that can see the source tree does not test the package.

**Chess as a standalone project inside this repository.** This is cheaper, and a
`find_package`-only build would still test the package. Rejected by the owner, because the
charter names a game project in its own repository.

**Shared libraries.** The engine has never built them, and nothing needs them. Exporting symbols
would add a policy to every public type for no consumer.

**Installing the internal headers.** They exist so that exactly one module can use them.
Installing them would make them public API with a comment asking people not to use it.

**A copy of `apps/common` in chess, or `apps/common` promoted to engine modules.** A copy
drifts from the lab's. Promotion makes app-level plumbing, such as log options and a PPM writer,
subject to every engine rule. The component is the middle course, and the owner chose it.

**Version 0.2.0, or 1.0.0.** 0.2.0 counts releases and needs a table to map a version to a
milestone. 1.0.0 declares the charter's test met before the report has scored it.

**Proving the package only from chess.** Atlas could then break its own install and find out
when chess next moved its pin.

## Consequences

- New: `cmake/AtlasInstall.cmake`, `cmake/AtlasConfig.cmake.in`, `tools/sdk.py`,
  `tests/package/`, the fixture mod, and a label `package`.
- Changed:
  - `atlas_add_module` gains `INTERNAL_DEPS`; `atlas_add_app_library` gains install-ready
    include paths.
  - `atlas_core` carries MSVC's standard.
  - `project(Atlas VERSION 0.27.0)`.
  - `build_mods.py` becomes generic and ships.
  - `gen_textures.py` loses its chess sheet.
- Removed, after atlas-chess is green: `apps/chess`, the chess assets and provenance rows,
  `chess_checks.py`, `bench_chess`, and the chess cases in `tests/CMakeLists.txt`.
- `CLAUDE.md` changes where it names `apps/chess`. The rule that no chess reaches `engine/`
  becomes trivially true. The text rules keep chess as their worked example, by link.
- **The public surface becomes a promise.** From here, renaming a public header, a target or an
  exported CMake variable breaks someone else's build. `SameMinorVersion` makes that allowed
  while the major version is zero, and the minor version says when it happened.
- **The risks are three.**
  - MSVC's standard and DLLs are unverified until a Windows lane builds a consumer (predictions
    4 and 5).
  - A cold chess CI run is long (prediction 9), and its cache is keyed on the pinned SHA.
  - An engine change that alters hashing now breaks chess's golden test in another repository,
    visible only when chess moves its pin. That is the golden test working, not a defect. It is
    recorded here so nobody mistakes it for one.

## Rollback cost

**Low for the package.** Removing the install files, `INTERNAL_DEPS` and the consumer returns the
build to what it is today. No engine code depends on being installed.

**Moderate for chess.** Its code would come back into the tree from the new repository, with the
build machinery it had here. The data-directory option and the generic mod script would stay,
because they are improvements on their own.
