<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0011 — Audio: SDL output, main-thread push mixing, decoders in assets

## Status

**Accepted**, 2026-09-17, implemented in M12. Authorised by
[ADR-0010](0010-charter-amendment.md), which lifted audio out of the charter's v0.1
exclusions by owner decision.

## Context

Atlas had no audio at all. The charter excluded it from v0.1 and ADR-0010 scheduled it as a
milestone, so the question was never whether to build it but what shape to build.

Three properties of the existing engine narrowed the answer before any preference entered.

**SDL's lifetime belongs to one module.** `platform::Platform`'s destructor calls `SDL_Quit`,
which shuts down every subsystem at once regardless of who started it. A second module bringing
up a subsystem would be sharing a lifetime with no way to see the other half of it, and the
failure would appear at shutdown, which is where failures are least legible.

**`assets` links no SDL and must not.** Importing runs on worker threads inside that module,
and CLAUDE.md confines SDL types to the platform and graphics implementations. So the window
system's own audio loader is unreachable from where importing happens, and moving importing to
reach it would put file decoding inside a module that owns a window.

**Nothing in the engine may reach simulation state except through a command.** A sound is
triggered by observing state, never by state itself, and `atlas_lab_sim` is fenced at configure
time to link nothing but `atlas::simulation`, so this is enforced by the build rather than
asked for in a comment.

The decision that remained genuinely open, and that CLAUDE.md requires asking about, is
threading: the window system can either drain a stream the main thread fills, or call into
Atlas from an audio thread it owns.

## Decision

**1. Output is a pushed stream, mixed on the main thread.** Once per frame, `update()` measures
what the device still holds, mixes enough to reach a 60 ms target, never exceeding a 120 ms
cap, and pushes it. No callback is registered, so the window system's audio thread drains the
stream itself and **never enters Atlas code**. Every entry point asserts main-thread affinity,
as the platform and graphics modules already do.

**2. The platform owns the audio subsystem**, behind `PlatformConfig::audio`, exactly as M11
put the gamepad subsystem there. Off by default, because every application works without sound,
and a failure to start it is a warning rather than a refusal. The audio module opens a device on
a subsystem someone else brought up, the way the renderer opens a graphics device on a window it
did not create.

**3. Decoders live in `assets`, and the WAV reader is first-party.** About 150 lines, following
`artifact_cache.cpp`'s hardening rather than the simulation's `SaveReader`, which has exactly
the right discipline and lives in a module `assets` does not depend on. **Format is chosen by
the leading bytes and never by a path's extension**, because an extension is a claim made by
whoever named the file and the input is untrusted.

**4. The mix format is fixed: 48 kHz, stereo, float32.** Everything converts to it on the way
in, so the mixer branches on no rate, no format and no channel count, and is therefore a pure
function that tests exactly.

**5. `create` never reports success for a device that will not make a sound.** An application
that wants to continue anyway asks for `AudioDevice::null()` and logs that it did, so "there is
no audio here" is a decision in a log rather than a silence nobody can explain.

**6. `play` returns a null handle on refusal rather than a `Result`.** Dropping a sound when the
voice limit is reached is a normal outcome under load, and an error at every call site is an
error every call site would ignore. Refusals are counted and logged once.

**7. Audio is presentation and is hashed nowhere.** A sound is triggered where a command is
submitted or where a snapshot is observed, never inside a system or an `apply`. Every golden
hash and every headless integration case is byte-identical after M12, and that was an exit
criterion rather than an expectation.

## Alternatives

**A callback mixer on the window system's audio thread.** Commands would reach it through a
lock-free queue; nothing in the callback could allocate, log, or assert. It is immune to a long
frame and gets latency down to roughly 20 ms against this design's 70 to 80.

It was rejected for M12 on cost rather than on merit. It adds a thread with a new class of rule
attached to it — a rule that is invisible until violated, and violated most easily by someone
adding a log line to diagnose something else. Against that, a strategy game's clicks and beds
do not need 20 ms. **This is the recorded rollback path**, and the trigger is a latency
complaint above about 80 ms, or a consumer that needs feedback tied tightly to an input.

**The window system's own WAV loader.** Unreachable from where importing happens; see Context.
Adopting it would mean importing inside a module that links SDL, which is a boundary this
project has held since M1 for reasons that have nothing to do with audio.

**A third-party audio library** — miniaudio, SDL3_mixer, dr_libs. Each would have supplied
decoding and mixing together. Rejected because the parts Atlas needs are the parts it already
had to decide for itself: the threading model, the error model, the handle model, and where the
boundary with `assets` falls. What was left is a mixer that is a hundred lines of arithmetic.
Recorded in DEPENDENCIES.md under "considered and not adopted" so the next person sees that the
question was asked.

**Ogg Vorbis in M12.** The decoder is already installed on every platform by the `stb` port, at
no cost. It was deferred by owner decision because it forces a committed binary test fixture —
the repository has none, and an Ogg cannot be produced by a checked-in standard-library script
the way a WAV can. The trigger is a track long enough that 64 MB decoded against 4 MB encoded
matters, which requires a game.

## Consequences

**A frame longer than the queued audio is heard as a gap, and that is measured rather than
assumed.** Ten seconds of the Strategy Lab at a million cells with a sound playing: at thirty
ticks a second the worst frame was 19.7 ms and there were no underruns; at sixty ticks a second
— a configuration M8 already recorded as beyond what that tick sustains — one frame reached
70.9 ms and produced exactly one underrun. The trade works as described, and the case where it
does not is a case that was already over budget for other reasons.

Both bounds are `AudioConfig` fields, so the response to a complaint is a tuning rather than a
rebuild.

**Input-to-ear latency is 70 to 80 ms worst case.** Right for a strategy game. Wrong for
anything rhythm-tight, which is the trigger above.

**`atlas::audio` depends on `platform`.** A module that could have depended only on `core` and
`assets` now has an edge to the window system, as the price of keeping SDL's lifetime in one
place. The alternative — the module bringing the subsystem up itself and relying on reference
counting — was rejected for the reason in Context.

**Mixing costs 31 µs for thirty-two voices** over a 1024-frame block, against a budget of 100,
with zero allocations per update once the voice count has peaked. Measured, with the prediction
written first; two of three predictions were about ten per cent low and are recorded as low.

**Four modules now link SDL** rather than three. CLAUDE.md's rule named the platform and
graphics implementations and had already drifted — the overlay uses SDL types too — so it is
corrected and widened in the same edit.

**A new error domain.** Codes numbered 500 report as audio, which required a rung above the
existing top of an open-ended ladder; a 500 code previously reported itself as a serialization
error. Both sides of every boundary are now pinned by a test.

## Rollback cost

**Low in code, and the replacement is designed.** The mixer is pure functions over buffers and
does not move. What changes is who calls them and when: a callback registered with the device,
a bounded single-producer queue carrying play and stop requests, and a rule that the callback
allocates nothing, logs nothing and asserts nothing. `update()` becomes the producer end.

Nothing outside the audio module would change. `play`, `stop`, the handles, the buses and the
statistics all keep their shapes, because none of them says where the mixing happens.

What cannot be rolled back is the dependency edge and the error block, neither of which is
affected by the threading choice.
