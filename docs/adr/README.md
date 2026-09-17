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
| [0009](0009-scripting-decision.md) | Scripting: deferred, with the boundary decided now | Accepted; trigger fired by 0010; decision 6 superseded by 0015 when M15 lands |
| [0010](0010-charter-amendment.md) | Charter amendment: seven subsystems by owner decision, physics kept out | Accepted |
| 0011 | Audio: SDL output, main-thread push mixing, decoders in assets | Planned, M12 |
| 0012 | Scene format version 2 and the append rule | Planned, M13 |
| 0013 | Animation clip file format | Planned, M13 |
| 0014 | Deterministic lockstep over the command queue | Planned, M14 |
| 0015 | Sandboxed mods | Planned, M15 |
