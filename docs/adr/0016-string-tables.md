<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0016 — String tables: one lookup, one substituter, English only

## Status

**Proposed**, 2026-09-19. M16 builds it; this record becomes Accepted when it is in force, as
the legend in this index requires.

Authorised by [ADR-0010](0010-charter-amendment.md), which lifted localisation out of the
charter's v0.1 exclusions and made it a scheduled milestone. Its decision 6 is the reason the
first section below exists rather than being a formality.

**This record numbers 0016 and the lockstep transport becomes 0017.** Numbers follow landing
order, and [ADR-0014](0014-deterministic-lockstep.md)'s open transport question is M17's, which
runs after this one.

## Context

### The consumer this milestone was scheduled against does not exist

ADR-0010 D6 set the bar for all seven subsystems: each is built when "its milestone is scheduled
and has a consumer to build against", and dropped when "no consumer strong enough exists when
its turn comes". For localisation the consumer was named in the series plan and repeated in
`docs/reports/M15.md`: *"M15 gives it the first real one, since a mod that adds strings needs
somewhere to put them."*

That is false, and it was checked rather than assumed. M15's guest interface is eight imports —
`atlas_tick`, `atlas_command_type`, `atlas_submit`, `atlas_random`, `atlas_log`,
`atlas_view_count`, `atlas_view_size`, `atlas_view_read` — and `atlas_mod.h` declares that the
list *is* the interface rather than a summary of it, because WebAssembly has no ambient
authority. None of the eight touches a string table. The two that take a `const char*` are
`atlas_log`, whose lines are engineer diagnostics dropped under load, and `atlas_command_type`,
which resolves an identifier. A mod cannot read a file either: "no filesystem, no network, no
environment". Giving a mod strings would mean a ninth import, a wider untrusted surface that
[ADR-0015](0015-sandboxed-mods.md) governs, and a materially larger milestone.

So the consumer M16 was scheduled against was a milestone that was planned and not built. The
accounting was put to the owner on 2026-09-19 together with the option of dropping the
milestone, which ADR-0010 D6 makes legitimate. **The decision was to build.** This record
therefore replaces the justification rather than inheriting it.

### The consumer that does exist is duplication, not translation

The same survey that found the missing consumer found a real one. The log severity words exist
in **three independent copies** — the overlay's combo array at `debug_ui.cpp:774`,
`core::to_string(Severity)` at `log.cpp:165`, and `to_short_string` beside it. `speed_name`
exists in **two byte-identical copies**, at `engine/tools/src/panels.cpp:133` and
`apps/lab/main.cpp:1057`. CLAUDE.md's own bar is "two real call sites need the generalisation",
and that is met twice over by strings that have already been written down more than once and
can already drift apart.

This is a weaker consumer than any other milestone in the series had, and the record says so
rather than dressing it up. It is also the honest one.

### What is actually there to localise

About 135 to 140 distinct strings a person can see. Roughly 73 are in
`engine/tools/src/debug_ui.cpp`, the only file in the repository that calls `ImGui::`; the rest
are stat labels and panel titles the two applications hand to the overlay, and four
`to_string`-shaped tables under `engine/core`, `engine/assets`, `engine/scene` and
`engine/edit`. The vocabulary is `state hash`, `acquire+present`, `awaiting finalisation`. Not
one is a sentence addressed to a player, because there is no game and no player.

Fifteen or so are already `std::format` templates rather than literals, and one composes two
translatable strings into a third: `std::format("Undo {}", history.undo_label())` at
`debug_ui.cpp:634`. That line is the hardest case in the milestone and it is the reason D4 is
shaped the way it is.

## Decision

**1. A module, `engine/text`, depending on `core;assets`, with `tools` as its only module
consumer.** Named for what it will still be when something draws text, rather than
`localisation`, which would be the wrong name the moment a text renderer arrives. Not folded
into `assets`, which owns loading rather than meaning.

The single-consumer edge is deliberate and it is a constraint on the design, not an observation
about it. `core::to_string(Severity)` and `assets::to_string(AssetState)` keep their words and
gain no dependency, because those words are *also* log text and routing them through a table
would make every integration case's grep depend on a locale. The overlay maps enum to key on its
own side. So `text` is a leaf with one edge instead of an edge threaded through five modules.

**2. JSON, named and versioned, read by the library the scene and clip files already use.**
`"format": "atlas-strings"` and an integer `"version"`, both checked before anything else, and a
`"locale"` recorded so a table says what it is.

```json
{ "format": "atlas-strings", "version": 1, "locale": "en",
  "strings": { "ui.controls.pause": "Pause", "ui.undo_with": "Undo {0}",
               "edit.command.rename": "rename" } }
```

This makes `assets` link the JSON reader for a third consumer. Three statements in
`docs/DEPENDENCIES.md` and one in `docs/ARCHITECTURE.md` say it is private to two modules; they
are corrected in the same change, and ARCHITECTURE's is reworded rather than appended to,
because "private to exactly one module" followed by a list of two was already strained before
this record made it three.

**3. A missing key returns the key itself.** Never an empty string, never an error. A panel
reading `ui.controls.pause` where it meant "Pause" is visibly wrong and still usable, which is
the right failure for presentation — the same instinct as a missing asset resolving to a
fallback rather than stopping the engine. Misses are counted and logged **once per key**, so a
gap is findable without a log line per frame.

**4. Substitution is positional, hand-written, and cannot throw. `std::vformat` is refused.**

```cpp
[[nodiscard]] std::string substitute(std::string_view pattern,
                                     std::span<const std::string_view> args);
```

`{0}`, `{1}` and so on, replaced by position. Positional rather than sequential because word
order differs between languages and `{}` in sequence cannot express that. **Anything malformed
is copied to the output verbatim** — an unclosed brace, a non-digit, an index with no argument —
rather than raised, because a translation file is untrusted input and the failure that shows the
translator their own mistake is better than the one that stops the frame.

The refusal of `std::vformat` is the load-bearing half. The engine's logging path is
compile-time-checked on purpose: `log.hpp` takes `std::format_string<Args...>`, which is
consteval-constructed from a literal, so a `std::string` loaded from a file cannot enter it at
all. The documented escape hatch, `std::vformat`, **throws `std::format_error`**, which
[ADR-0005](0005-error-and-ownership-model.md) forbids crossing a module boundary. And that is
mechanical rather than advisory: `tools/check_module_deps.py`'s exception allow-list has exactly
one entry today, so a `try`/`catch` in `engine/text/src` fails `precheck.sh` until a second is
written with a reason.

Writing the substituter is perhaps forty lines and needs no allow-list entry at all. That is
the cheaper side of the trade, and the allow-list staying at one entry is an exit criterion
rather than a hope.

**5. `edit::Command::label()` returns a catalogue key.** `"edit.command.rename"` rather than
`"rename"`. It is documented today as *"For 'Undo move', 'Redo rename'"* and it is **also**
formatted into two `History` log lines and asserted by three tests — a string that is
simultaneously a display label, a log token and a merge discriminator is exactly the drift this
milestone exists to remove. `edit` gains no dependency on `text`: it produces keys and the
overlay resolves them, and an identifier in a log line is more greppable than prose.

This is what makes the hardest case tractable. `std::format("Undo {}", undo_label())` becomes
`substitute(lookup("ui.undo_with"), {lookup(undo_label())})` — two catalogue entries composed
through the substituter, with the order of the two the translator's to choose rather than
English's.

**6. `AssetType::StringTable = 5`, appended, with its finaliser in the same change.** Appended
because the value is folded into every asset identifier's hash and those identifiers reach saved
scene files, so inserting one would invalidate every persisted reference after it. The finaliser
is `text::Catalog::finalise_pending(assets::Registry&)`, the shape `TextureCache`, `ClipCache`
and `AudioDevice` already share.

The finaliser is not a nicety to follow later. Since M12 the registry counts an asset that
decodes and is never claimed and reports it after about ten seconds, and `AssetType::Shader` has
been sitting in that state since M4 as the standing example of what omitting one costs.

**7. The bound that matters is on the document's own length, checked before the parse**, with
[ADR-0013](0013-animation-clip-format.md)'s reasoning inherited rather than re-derived: a
document parser cannot check a declared size against the bytes present, because by the time any
count is readable the whole document is in memory. After the parse: a key count, a key length, a
value length, a newer version refused, and **a duplicate key refused rather than last-wins**,
because which of two identical keys wins is not a question a translator should have to know the
answer to.

**8. The catalog is optional at the point of use, and absence is the miss path.** The overlay
takes a `const text::Catalog*` and null is a supported value: every key then resolves to itself,
by D3. So every existing unit and graphics test keeps working without a registry wired into it,
and the miss path is exercised by the tests that already exist rather than being a second branch
nobody runs.

**9. What is outside the scope, said here so it is not ambiguous later.**

- **The two command-line usage blocks.** `apps/lab/main.cpp:123` and
  `apps/sandbox/main.cpp:97` are two raw literals, 102 lines and about 990 words — seven times
  the prose of the whole interface — column-aligned at 25 characters and printed to standard
  output. They are developer documentation, and a catalogue would either hold them as two
  untranslatable blobs or split them per line and destroy the alignment.
- **Every `ATLAS_LOG_` message** — 157 of them in `engine/` and `apps/`. A log line is a
  diagnostic for whoever reads the build. Routing them through a table would make every
  integration case's grep depend on a locale, which is a worse property than the one it would
  buy.
- **Runtime locale switching.** One locale exists. The catalog takes what it is given and
  switching is a reload.

**10. The claim "a second language is a data change" holds for Latin-1 and no further, and this
record says so rather than letting the milestone imply otherwise.** The overlay renders Dear
ImGui's default baked atlas — Basic Latin and Latin-1 Supplement. Nothing in the tree touches
`io.Fonts`, and `imgui` is pinned without the `freetype` feature. French, German, Spanish and
Italian would render from a table alone; Polish, Czech, Turkish, Greek, Russian, Hebrew, Arabic
and every CJK language would show blanks, because the glyphs are not there.

So for those a string table is necessary and not sufficient, and the missing half — an atlas
configured from the loaded locale and a bundled font with its licence, provenance and megabytes
— is deferred with the trigger written down in `docs/DEFERRED.md`: the first language outside
Latin-1. Recorded before the milestone was built rather than discovered after somebody shipped a
Polish table and saw boxes.

## Alternatives

**A catalog and a substituter with the table compiled in, and no file format.** Much less
machinery: no asset type, no parser, no fault suite, no registry arm, no finaliser. It would
close the duplication the survey found, which is the consumer this milestone actually has.
Rejected because the milestone's whole purpose is that a second language is a data change, and a
table that is compiled in makes it a code change — the one property worth buying, not bought.
The owner chose the fuller scope with this alternative in front of them.

**Extending the mod ABI so the claimed consumer becomes real.** A ninth import, a host
implementation, tests that call it through a real guest, and `kSafeImportCount` bumped. It would
close the loop M15 left and make the series plan's sentence true rather than corrected. Rejected
as scope: it widens the untrusted surface, which is ADR-0015's business rather than this
record's, and it would make the smallest milestone of the series one of the larger ones. Left in
`docs/DEFERRED.md` with its trigger — a mod that has something to say to a person.

**`std::vformat` behind a commented boundary wrapper.** ADR-0005 permits exactly this shape, and
the allow-list exists for it. Rejected because the wrapper would be the *only* thing standing
between untrusted file content and a throw, on a path that runs every frame for every visible
string; because a forty-line substituter that cannot throw needs no wrapper, no allow-list
entry, and no argument about whether the wrapper is airtight; and because `std::format`'s
sequential `{}` cannot express word order anyway, so positional indices would have to be built
on top of it regardless.

**A real internationalisation library — ICU, gettext, or Boost.Locale.** Plural rules, gender,
collation, number and date formatting, and a solved answer to every question this record defers.
Rejected on cost against need: ICU is tens of megabytes and would be the largest dependency in
the project by an order of magnitude, for one locale, one hundred and forty engineer-facing
strings, and a plural rule English does not need. The trigger is recorded: a second language
whose plurals or gender the substituter cannot express.

**Dropping the milestone**, which ADR-0010 D6 makes a legitimate outcome once the consumer it
was scheduled against turned out not to exist. Put to the owner as such. Not chosen.

## Consequences

**Every visible string gains a level of indirection**, and one of them is now two lookups and a
substitution where it was a `std::format`. At a hundred and forty strings a frame this is a
hash lookup each, which `bench_text` measures against a prediction committed before it.

**A new asset type costs five edits inside `assets` and one outside it**, and the enum is now
five values deep with the append rule load-bearing rather than theoretical.

**`edit::Command::label()` changes what it returns**, so two log lines read
`edit.command.rename` where they read `rename`, and three tests change with them. Anything
outside this repository reading those labels would break; nothing does.

**The catalogue can be wrong in a way the compiler cannot see.** A key with no entry renders as
itself: visible, logged, and not a crash — but a key *misspelled at the call site* is
indistinguishable from one missing from the table, and both look the same in the interface.
That is why the keys are constants in one header and why `--text-check` resolves all of them
against the shipped table as an integration case, rather than the milestone resting on having
looked carefully.

**A second language is now a file, for languages the atlas can draw.** For every other language
it is a file and a font, and D10 is where that is written down.

## Rollback cost

**Low in code, moderate in data.** Reverting `text` means reverting one module, one asset type,
and the call sites — mechanical, because every one of them is a lookup with a key that is
already the English string's meaning.

What does not revert cheaply is `AssetType::StringTable = 5`. The number is folded into every
asset identifier's hash, so once a scene file references a string table, removing the enumerator
renumbers nothing but leaves a value that must stay reserved for ever. That is the same
one-way door `AudioClip` and `AnimationClip` went through, and the append rule exists precisely
so the door only ever opens outward.

The file format is version 1 and has no readers outside this repository, so changing it is a
version bump and a migration the ADR-0012 append rule already covers.
