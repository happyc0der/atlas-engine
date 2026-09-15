<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0007 — Scene file format

## Status

Accepted, 2026-09-15.

## Context

M5 needs a scene to survive leaving the process. Something has to decide what the bytes look
like, and the decision is expensive to revisit later: once anyone has a saved scene, every
future change to the format has to either read the old one or admit it cannot.

Three properties drive the choice.

**It is read by people.** Scenes are authored in a tool and reviewed in version control. A
format a person cannot read is a format where a bad diff is invisible, and where debugging a
load failure means writing a second tool first.

**It is untrusted.** A scene file arrives from a mod, an asset pack, a download, or a
colleague. The charter treats all four as hostile input. Whatever reads it must refuse bad
input rather than trust a length field, and must not throw, because exceptions do not cross
module boundaries in this codebase (ADR-0005).

**It must be canonical.** The same scene must produce the same bytes every time, whatever
order it was assembled in. Without that, saving a file twice produces a diff, a future hash
of scene state depends on creation order, and nothing built on top can be compared.

Separately, the simulation will need its own save format later (M6), and that one has
different requirements: large, binary, integrity-checked, not read by people. This decision
deliberately does not try to serve both.

## Decision

**JSON text, indented, one object per entity.** Written with `nlohmann::ordered_json` so key
order is the writer's and not a hash's.

**The file names itself.** Every scene begins with `"format": "atlas-scene"` and an integer
`"version"`. A file whose marker does not match is refused before anything else is read, so a
JSON file that happens to be valid is not mistaken for a scene.

**A newer version is refused, not tolerated.** Reading a file from a later build would
silently drop whatever that build added, and the first sign of trouble would be data
disappearing on the next save. The loader fails with `VersionMismatch`. Older versions will
be migrated on load when a version 2 exists; there is no version 2 yet, so there is no
migration code yet.

**Entities are written in stable-identifier order, components in a fixed schema order.**
This is what makes the output canonical. The scene sorts on the way out rather than
inheriting the entity library's storage order, which is arbitrary (ADR-0004).

**Parentage is recorded on the child only.** A child names its parent; no entity lists its
children. Storing both would let a file disagree with itself and force the loader to pick a
winner. Sibling order is reconstructed by identifier on load, which is why it is not written.

**Derived state is not written.** World transforms are recomputed on load from the local
ones. A file holding both could contradict itself, and the contradiction would be silent.

**Arrays of plain numbers are kept on one line.** The indenting writer puts every array
element on its own line, which turns a two-number position into four. Moving one entity
should be one changed line in a diff, not a paragraph.

**Bounds are checked before allocation.** Entity count and name length are capped
(`kMaxEntities`, `kMaxNameLength`). Parsing is non-throwing. A file describing a cycle is
refused, because a cyclic hierarchy is not merely invalid but unwalkable: composing
transforms over it would not terminate.

**A failed load changes nothing.** The loader builds into a scene of its own and moves it into
the caller's only after every entity, component and parent link has been accepted. A
half-loaded scene would be a state every reader has to handle, in exchange for nothing.

**Asset references are stored as the identifier's raw value.** An identifier is a hash of a
path and a type; it cannot be validated without the path that produced it. One that names
nothing this build knows about resolves to the fallback texture, which is a path that already
exists and is already tested.

## Alternatives

**A binary format.** Smaller and faster to parse. It gives up reviewability entirely, which
is the property that matters most for authored data, and it buys performance for a problem
that does not have one: a scene is small and is loaded once. The simulation's save format in
M6 will be binary, for data that genuinely is large and genuinely is not read by people.
Rejected here, and the split is deliberate rather than an oversight.

**A bespoke text format.** Terser than JSON and pleasant to read. It means writing and
maintaining a parser, and a hand-written parser for untrusted input is exactly the kind of
code this project should not be writing when a hardened one is already a dependency.
Rejected.

**TOML or YAML.** Both are more pleasant to hand-edit than JSON. Both add a dependency for a
file that is written by a tool far more often than by hand, and YAML's specification surface
is large enough that its parsers have a poor security record. Rejected.

**Storing the entity library's handle instead of a stable identifier.** Simpler, and wrong:
the handle is recycled, so the file would load without error and refer to different entities.
Rejected outright; see ADR-0004.

## Consequences

- Scene files are large for what they contain: roughly 470 bytes per entity for the M5
  component set. Acceptable for authored scenes; not acceptable for a world, which is why
  the simulation gets its own format.
- Floating-point values are written through the JSON writer's shortest round-trip
  representation, so a value survives a save and load exactly. This is a property of the
  writer, not of Atlas, and it is worth knowing that the guarantee is borrowed.
- Every new component needs a schema entry and a place in the fixed order, and adding one to
  the middle changes every existing file's bytes. New components are appended.
- The format version and the engine version are unrelated on purpose. The format changes when
  the schema does, not when the engine ships.
- There is no compatibility code yet, and writing it before a version 2 exists would be
  writing untested code for an imagined problem. The version check is what makes deferring it
  safe: a file Atlas cannot read is refused rather than misread.

## Rollback cost

Low. The format is confined to `engine/scene/src/serialization.cpp`, behind `to_text` and
`from_text`, and nothing has shipped that would need migrating. Changing the representation
means rewriting that file and re-cooking any checked-in scenes; no caller changes.
