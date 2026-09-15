# Architecture decision records

Each ADR records one decision that is expensive to reverse. Every record has the same
sections: Context, Decision, Alternatives, Consequences, Rollback cost, Status.

Status values: **Proposed** (decided in principle, not yet implemented), **Accepted**
(implemented and in force), **Superseded by NNNN**, **Rejected**.

An ADR is never edited to hide a change of mind. It is superseded by a new record.

| # | Title | Status |
|---|---|---|
| [0001](0001-language-build-platform.md) | Language, build system, and target platforms | Accepted |
| [0002](0002-rendering-backend.md) | Rendering backend: SDL_GPU behind an engine-owned boundary | Accepted |
| [0003](0003-simulation-render-separation.md) | Simulation and render separation | Proposed |
| [0004](0004-scene-ecs-vs-simulation-storage.md) | Scene ECS versus specialised simulation storage | Proposed |
| [0005](0005-error-and-ownership-model.md) | Error handling and ownership model | Accepted |

Planned: 0006 shader toolchain (M2), 0007 authoring data format (M5), 0008 numeric and
determinism policy (M6), 0009 scripting decision (M9).
