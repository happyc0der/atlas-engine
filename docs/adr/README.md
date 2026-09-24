# Architecture decision records

Each ADR records one decision that is expensive to reverse. Every record has the same
sections: Context, Decision, Alternatives, Consequences, Rollback cost, Status.

Status values: **Proposed** (decided in principle, not yet implemented), **Accepted**
(implemented and in force), **Superseded by NNNN**, **Superseded in part by NNNN** (a record
whose other decisions remain in force), **Rejected**.

An ADR is never edited to hide a change of mind. It is superseded by a new record.

| # | Title | Status |
|---|---|---|
| [0001](0001-language-build-platform.md) | Language, build system, and target platforms | Accepted |
| [0002](0002-rendering-backend.md) | Rendering backend: SDL_GPU behind an engine-owned boundary | Accepted |
| [0003](0003-simulation-render-separation.md) | Simulation and render separation | Accepted |
| [0004](0004-scene-ecs-vs-simulation-storage.md) | Scene ECS versus specialised simulation storage | Accepted |
| [0005](0005-error-and-ownership-model.md) | Error handling and ownership model | Accepted |
| [0006](0006-shader-toolchain.md) | Shader toolchain | Accepted |
| [0007](0007-scene-file-format.md) | Scene file format | Accepted |
| [0008](0008-numeric-and-save-policy.md) | Numeric policy and the simulation save format | Accepted |
| [0009](0009-scripting-decision.md) | Scripting: deferred, with the boundary decided now | Superseded in part by 0015; decisions 1 and 2 stand, 3 to 5 amended in part |
| [0010](0010-charter-amendment.md) | Charter amendment: seven subsystems by owner decision, physics kept out | Accepted |
| [0011](0011-audio.md) | Audio: SDL output, main-thread push mixing, decoders in assets | Accepted |
| [0012](0012-scene-format-v2.md) | Scene format version 2, the append rule, and two writers on one entity | Accepted |
| [0013](0013-animation-clip-format.md) | Animation clip file format | Accepted |
| [0014](0014-deterministic-lockstep.md) | Deterministic lockstep over the command queue | Accepted; decision 9 superseded by 0017 |
| [0015](0015-sandboxed-mods.md) | Sandboxed mods: WebAssembly, behind the command queue | Accepted |
| [0016](0016-string-tables.md) | String tables: one lookup, one substituter, English only | Accepted |
| [0017](0017-lockstep-transport.md) | A transport for lockstep: ENet, polled, direct address only | Accepted |
| [0018](0018-chess-probe.md) | Chess as the first consumer: an in-tree probe of the engine, not v1.0 | Accepted |
| [0019](0019-declined-commands.md) | A command may be declined on world state | Accepted |
| [0020](0020-session-finish.md) | A session can finish | Proposed; written at M21's gate |
