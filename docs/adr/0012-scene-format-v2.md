<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0012 — Scene format version 2, the append rule, and two writers on one entity

## Status

**Accepted**, 2026-09-17, implemented in M13. Extends
[ADR-0007](0007-scene-file-format.md), which chose the format and deferred the migration
policy. Authorised by [ADR-0010](0010-charter-amendment.md), which scheduled animation as a
milestone.

## Context

Two questions arrived together, and they are one decision because the answer to each is the
reason for the other.

**The format gains a component.** An entity can now carry an animator: which clip it plays,
where playback starts, how fast, and whether it loops. That is authored — a person chooses it,
the edit history owns it, and a save records it — so it belongs in the file, and putting it
there changes the schema for the first time since M5.

ADR-0007 anticipated exactly this and declined to write the code: *"Migration is written when
the first real schema change happens; the policy is recorded now, the code is not, because a
migration with nothing to migrate is untested by construction."* This is that moment.

**And an entity gains a second writer.** A clip moves something; a person drags the same thing
in the inspector. `CLAUDE.md` resolved that conflict in M9 by forbidding it: *"An application
that animates the scene directly must say so and must not animate what the user can edit."*

That rule was honest about a real hazard and it made the sandbox's editor useless on the thing
it exists to edit. With the animation running, an edit to an animated entity was overwritten
within a frame. Paused, it was invisible — because pausing also stopped the only call that
recomposed world transforms, so the authored number changed, the panel showed it, and the
picture did not move. **Both halves of the key that existed to make editing possible were
broken**, and the panel was already displaying the contradiction: a local position that had
moved beside a world translation that had not.

## Decision

**1. The version becomes 2, and the `animator` key is appended after `camera`.** New components
go at the end, per ADR-0007: a component inserted into the middle changes the bytes of every
file already written.

**2. The migration rule: a version that only appends needs no migration code.** The loader
accepts any version in `[kMinSceneFormatVersion, kSceneFormatVersion]` and **there is no branch
on the version after that check**. A component added at version N+1 is read when its key is
present and left absent when it is not; a writer at version N never produces the key. So an
older file is already a valid newer one with some components missing.

Migration *code* becomes necessary only for a change that is not a pure append: a renamed key,
a changed unit, a split field. The day one of those happens is the day the branch appears, and
not before.

The rule is asserted rather than described. A version 1 document loads, re-saves, and equals
itself with only the version number changed — so a change that is not a pure append fails that
test, which is the test telling whoever made it that they now owe a migration.

**3. Animation writes an offset, never the authored pose.** A separate component, never
serialised, composed with the authored transform in `update_transforms`: position adds,
rotation adds, scale multiplies, so an untouched pose is the identity in all three channels.

**4. `CLAUDE.md`'s rule about two writers is replaced rather than satisfied.** It said an
application must not animate what the user can edit. It now says the authored components belong
to the history and the derived pose belongs to the animator, and that neither writes the
other's fields. The change of mind is recorded here rather than made quietly in the rules file.

**5. The clock lives in the pose, not in the animator.** So a saved scene records what the
author chose and never how long the application happened to be open, and the sandbox's
save-load-save byte check stays meaningful while clips are playing.

**6. A file that means something this build cannot is refused, not guessed.** A loop mode that
is not one of the three, a speed outside what the animator clamps to, a start time that would
be silently truncated by the field it is narrowed into. Guessing would round-trip a file into
something that plays differently from what it says.

## Alternatives

**Keep the rule; the editor pauses.** Simpler: no new component, no composition step. Rejected
because it is what the project already had, and what it had did not work — pausing froze the
editor along with the animation. It also leaves a revert able to fail when a frame lands
between an apply and an undo, because both would be writing the same field.

**The animator writes the authored transform.** One writer, no composition. Rejected: it puts
the animator and the undo history in a fight over one field, and it makes the inspector's
position control have no visible effect while a clip plays — the same confusion, with the undo
breakage removed and the confusion kept.

**Animation submits undoable commands.** Perfectly consistent with the old rule and immediately
unusable: a 256-entry undo stack fills in about four seconds of playback and every undo steps
one frame backwards.

**An absolute pose rather than an offset.** A clip would then say where a thing is rather than
how it moves, which makes the same clip unusable on two entities in different places, and makes
the authored position dead while a clip plays.

**Refuse version 1 files.** Considered for about as long as it takes to write down: there are
no version 1 files anywhere except ones this project wrote, and refusing them would be
abandoning them for no gain.

## Consequences

**A scene can be edited while it plays.** Dragging an animated entity moves it by the drag on
top of the playback, an undo undoes the drag and never the playback, and the file is unchanged
by having been watched. That last is a test rather than a claim: a hundred poses written and
composed leave the saved text byte-identical.

**Nothing in the repository would catch a migration regression except that test.** There is no
committed scene file anywhere — the only one is generated each run and ignored by git — so the
inline documents in the serialisation tests are the whole corpus. That is stated here so the
next person knows the test is load-bearing rather than illustrative.

**`Destroy` had to learn about the component**, as it must for every component: it captures a
fixed list, and one added to the scene without being added to that list is silently lost when
an entity is destroyed and the destroy undone. The pose is deliberately **not** captured — it
is what playback made of an entity, not something an undo owes it, and restoring an animator
restarts the clip, which is what loading a file does too.

**An asset type had to be fixed one slice before its importer existed.** An identifier carries
its type in its hash and those identifiers go into scene files, so the number for an animation
clip could not wait. Requesting one in the build that introduced it fails with a message saying
so, rather than decoding into nothing.

**Five places change when a component is added**, and this record names them because finding
them again costs more than writing them down: the component itself, the scene's accessors, the
entity view's flags, the serialiser's fixed key order, and `Destroy`'s record with both its
loops.

## Rollback cost

**Moderate, and asymmetric.** The pose component and the composition step could be removed with
no trace: nothing is serialised, so no file would need changing.

The format version could not. Any scene saved by a build that writes version 2 is refused by
one that writes version 1, and the refusal is correct — that is what the version check is for.
Reverting would mean either abandoning those files or writing the downgrade that this record
argues is unnecessary in the other direction.

The rule change in `CLAUDE.md` is the part that cannot be undone by deleting code. A project
that has once decided its animator and its editor may touch the same entity will keep deciding
that, and the next component with two writers will cite this.
