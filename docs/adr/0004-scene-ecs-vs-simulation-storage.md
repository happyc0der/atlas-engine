# 0004 — Scene ECS versus specialised simulation storage

## Status

Proposed, 2026-09-14. Becomes Accepted when M5 implements the scene layer.

## Context

Atlas has two storage problems that look similar and are not.

The first is scene composition: a modest number of presentation entities with heterogeneous
optional components, a transform hierarchy, and mutation driven by tools and editor actions.
Entities appear and disappear, component sets vary, and the access pattern is irregular.

The second is the future world simulation: a large number of uniform records, iterated in
bulk by many systems, with known query patterns, aggressive performance requirements, and a
hard determinism constraint. This data is also the thing that gets hashed, serialized, and
replayed.

A single entity-component-system for both is the obvious unifying idea, and it is a trap.
EnTT's registry is not generally thread-safe, its iteration order depends on storage
compaction rather than on anything stable, and its entity identifiers are not durable across
runs. Every one of those properties is fine for scene composition and disqualifying for
deterministic simulation.

## Decision

**Two storage models, chosen deliberately.**

`atlas::scene` uses **EnTT**, for presentation entities only, with a deliberately small
component set: a stable entity identifier, a name, local and world transforms, hierarchy
links, sprite or mesh render data, and a camera. EnTT is permitted in `atlas/scene` public
headers, which is one of only two exceptions to the third-party header rule.

`atlas::simulation` uses **first-party structure-of-arrays tables**, with layouts chosen
from measured query patterns rather than decided in advance. Tables are addressed by a
stable table identifier, iterated in index order, and hashed and serialized in that same
canonical order.

**Rules that follow.**

- Scene serialization writes stable identifiers and versioned component data. EnTT entity
  values are never written to a file, never hashed, and never used as a durable reference.
- Scene iteration and mutation are explicit phases; mutation during iteration is not
  permitted by convention but by structure.
- Hierarchy changes are validated for cycles; transform updates run in topological order
  from roots.
- Components hold asset and resource handles, never GPU resource ownership.
- The scene registry is main-thread only.
- Simulation tables are never stored in the scene registry, and simulation systems never
  touch EnTT.

All EnTT usage is confined to the `scene` wrapper, so the pin can move or the library can be
replaced without touching anything above it.

## Alternatives

**One universal ECS for everything.** Conceptually clean, and it is what many engines do. It
would force the simulation into EnTT's iteration order, threading constraints, and identifier
semantics, none of which survive contact with determinism requirements. It also encourages
the mistake this project most needs to avoid: treating a province or a market as an entity
with components, before anything is known about how they are actually queried. Rejected.

**No ECS at all; hand-written scene structures.** Honest, and for the tiny component set of
v0.1 it would work. It loses composition flexibility exactly where flexibility is wanted, in
tooling and editor work, and reimplements the one part of the problem a library solves well.
Rejected.

**flecs** instead of EnTT. Richer feature set including relationships and its own query
engine. More surface area to keep private behind a boundary, and the additional features
target problems Atlas does not have in the scene layer. Rejected, without prejudice.

**A custom ECS.** A project-sized subsystem, explicitly deferred by the charter until a
recorded limitation justifies it. Rejected.

## Consequences

- Two storage models to learn, document, and keep distinct. The boundary must be stated
  repeatedly, because the pull toward unifying them is constant.
- The simulation is free to choose layouts per measured query, which is the entire point:
  the data-oriented work that matters happens where it matters.
- The scene layer gets library-quality composition for tooling without paying for it in the
  simulation's constraints.
- EnTT is pinned at 3.16.0 while upstream is at 4.0.0, which requires C++20 and changes the
  API. Because usage is confined to the wrapper, moving later is contained.
- Some duplication is unavoidable: an entity that exists in both worlds needs an explicit
  projection, which is friction that makes the coupling visible rather than accidental.

## Rollback cost

Low to medium. Replacing EnTT means rewriting the `scene` wrapper, which is bounded by
design. Moving simulation data into an ECS after the fact would be far more expensive, since
hashing, serialization, and replay all depend on canonical ordering that an ECS does not
provide. The asymmetry is deliberate: this decision is cheap to reverse in the direction it
is likely to need reversing.
