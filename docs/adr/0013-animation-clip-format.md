<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0013 — Animation clip file format

## Status

**Accepted**, 2026-09-17, implemented in M13. Authorised by
[ADR-0010](0010-charter-amendment.md). The scene file's own format and versioning rules are
[ADR-0007](0007-scene-file-format.md) and [ADR-0012](0012-scene-format-v2.md); this record
follows both rather than restating them.

## Context

An animator names a clip. Something has to say what a clip is.

Three properties settled most of it before any preference entered.

**A clip is tuned by hand.** Its entire content is timings and offsets that somebody adjusts
until the motion looks right, and the loop of that work is edit, look, edit again. Anything that
needs a tool between the editing and the looking makes that loop longer.

**A clip is untrusted input**, like every other file the engine reads. The charter treats an
asset pack, a download and a colleague's file identically.

**The animation module's own header already made claims about it.** `Clip::duration_ns` said a
clip shorter than its last key "is refused at import" and `transform_keys` said they are
"ordered by time, first key at zero". Both were written in the slice before an importer existed,
and nothing enforced either. Sampling binary-searches the keys, so an unordered track does not
fail — it returns the wrong pose.

## Decision

**1. JSON, named and versioned, read by the library the scene file already uses.** `"format":
"atlas-animation-clip"` and an integer `"version"`, both checked before anything else. Comments
are permitted, matching the scene reader, because a note beside a key is exactly what somebody
tuning one would write.

This makes `assets` the second module to link the JSON reader. The alternative was a second
bespoke text format in the same milestone that added a bespoke binary one for audio, and a
format nobody can hand-edit for a file whose whole purpose is to be hand-edited.

**2. Integer milliseconds in the file, nanoseconds in memory.** Milliseconds because that is
what a person types and what every sprite tool exports; nanoseconds because a frame is sixteen
and two-thirds milliseconds and truncating it loses two-thirds every frame. Integer at both
ends, so the clock is exact after an hour rather than a float sum that has drifted.

**3. Transform keys carry offsets, not placements.** Position and rotation are added to what the
author typed and scale is multiplied by it, per ADR-0012. A clip is therefore a description of
movement that any entity can use, rather than a description of where one particular entity is.

**4. Frames address cells of a uniform grid.** Columns, rows, and a cell index counted left to
right then top to bottom. Normalised rectangles are hostile to write and change meaning when a
sheet is re-exported at another size; pixel rectangles need a texture size, which the animation
module never sees because it does not know what a texture is.

**5. Easings are named words from a fixed set, and a name this build does not know is refused.**
Guessing would play a clip differently from what its file says while looking as though it
worked, which is the failure that takes longest to find.

**6. The bound that matters is on the document's own length, checked before the parse.** This
is where the discipline differs from the audio reader, and the difference is not a weakening.
That reader checks a declared size against the bytes actually present *before* allocating. A
document parser cannot: by the time any count inside the document is readable, the whole thing
is already in memory. So an enormous input is refused up front, and everything after the parse
is about refusing a document that means something impossible rather than one that would
allocate too much.

**7. Every claim the animation header already made is now enforced**: keys ordered, the first at
zero, no key past the clip's own duration, every cell inside the grid, every number finite.

**8. A clip does not name the sheet it cuts.** See Consequences; this is a recorded hazard
rather than an oversight.

## Alternatives

**A bespoke text format.** No new module dependency, and it would follow the audio reader's
hardening precedent directly. Rejected because it means specifying and hardening a second
format in one milestone, and because hand-editing it would be worse than JSON in every respect
that matters for a file that exists to be hand-edited.

**Binary, like the simulation's save format.** Fastest to parse and the most direct reuse of
existing hostile-input discipline. Rejected outright: it makes a clip uneditable without a tool.
The save format is binary because it is large, machine-written and never read by people; a clip
is none of those.

**Absolute placements rather than offsets.** Rejected by ADR-0012's reasoning: a clip would then
be usable on exactly one entity, and the authored position would be dead while it played.

**Normalised or pixel frame rectangles** rather than a grid. The first is unwritable by hand and
breaks on re-export; the second would make the animation module depend on knowing a texture's
size, which is a dependency it does not have and should not want.

**The clip naming its own sheet.** Would make a mismatch impossible by construction. Rejected:
it makes a clip usable on exactly one sheet, turns clips and textures into a dependency the
registry cannot express, and means re-exporting a sheet under a new name edits every clip that
referenced it.

## Consequences

**A clip and the sheet it cuts can disagree, and nothing says so.** A clip written for a
four-by-two sheet, attached to an entity whose sprite is a three-by-three one, shows the wrong
frames. Import refuses a cell outside the clip's *own* grid, which catches a clip disagreeing
with itself, but it cannot check a grid against a texture it never sees.

**This is accepted knowingly.** The fix would be an asset dependency graph — a clip declaring
what it needs and the registry resolving it — which the registry has no support for and which a
deferred entry has covered since M4, though that entry's stated reason ("with one asset type,
nothing depends on anything") expired the moment there were four. The condition for building it
is recorded with the hazard.

**No cache.** A clip is a few hundred bytes of text that parses faster than a cache entry would
read, and the artifact cache is texture-shaped end to end. The trigger is the same as audio's:
an import measured above five milliseconds.

**A finaliser was mandatory, not optional.** Since M12 the registry reports an asset that
decodes and is never claimed. A clip type without something to convert it would have decoded,
sat unclaimed, and been reported ten seconds later — which is the check working, and not a state
to ship.

**Two enumerations agree by position and one array says so.** The easing names in the importer
and the easing enumeration in the animation module are separate declarations in separate
modules, because a payload that named the module's type would put file decoding and clip
evaluation in the same place. The importer refuses an unknown name, and the converter guards the
index anyway.

**The scene reader has the same missing bound and does not get it here.** It caps the entity
count but not the document's own length. Widening that file is a separate change with its own
risk, so the gap is recorded rather than fixed in a milestone about animation.

## Rollback cost

**Low.** The format is confined to one private translation unit behind one function, and the
conversion to the module's own types is confined to one finaliser. Nothing outside those two
files knows what a clip file looks like.

The parts that would not roll back are the ones this record shares with ADR-0012: the asset type
number, which is folded into every identifier's hash and reaches scene files, and the JSON
reader now having a second consumer. Neither is affected by the format's own shape.
