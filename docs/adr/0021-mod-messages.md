<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0021 — A mod may speak to a person, one way, by key

## Status

**Accepted**, 2026-09-24, implemented in M24.

Proposed the same day at M24's gate. **Four things changed by implementing it**, each marked at
its own heading with what changed and why rather than edited to read as if it had always said
it: D1's refusals are more precise than written, D4's budget lives somewhere else, D5's lab has
no panel, and the Consequences' `--text-check` line was wrong about what can be checked. The
import, its direction, the namespace rule and every refusal of text *into* a mod held.

Extends [ADR-0015](0015-sandboxed-mods.md)'s guest interface by one import and its limits by one
budget. Nothing in 0015 is superseded: every decision about what a mod may not reach stands, and
this record exists partly to show that the new import does not reach any of it.

## Context

M16 was justified by a consumer that did not exist. The series plan said *"M15 gives it the first
real one, since a mod that adds strings needs somewhere to put them"*, and M15's interface had
eight imports, none of which touches text. `docs/DEFERRED.md` recorded a string API for mods with
its trigger: *"a mod that has something to say to a person — a panel, a command name, a
message."*

**That trigger has not fired, and this record says so first.** No mod in this repository has
anything to say to anybody. The owner chose on 2026-09-24 to finish the three items M17 left open,
in order, and this is the second. What is built here is built on that decision, against the only
consumer available — the lab's own demonstration mod — exactly as M17's transport was built with
two processes of the lab as its proof. That is a weaker consumer than a game and a stronger one
than none, and the report will say which parts it proves.

### What a string API for mods could mean, and the half of it that is ruled out

There are two directions text could cross the boundary, and they are not symmetrical.

**Text into a mod is ruled out, and not as a preference.** An import that handed a mod the
resolved text for a key would hand it bytes that depend on the locale of the machine it runs on.
Two peers in a lockstep session need not share a language. A mod that branched on a string's
length, or compared it, or merely hashed it into a decision, would decide differently on each
peer, and the session would diverge — the exact failure ADR-0015 withheld a clock to prevent, now
entering through the interface's words instead of its time. Nothing a mod needs from a string
table is worth that: a mod has no use for words; only a person does.

**Text out of a mod is what the trigger describes**, and it is safe for the reason the other
direction is not: presentation never reaches simulation state. A mod names what it wants said;
the application resolves the name through its catalogue, in its own language, and shows it. Every
peer's mod says the same things at the same ticks, because every peer runs the same mod on the
same state — and if one did not, nothing would diverge, because nothing said is hashed.

**Raw text out of a mod is ruled out too**, for ADR-0016's reason: a string a person can see
comes from a table, by a key, so it can be translated. A mod that wrote English into an interface
would be the one place in Atlas that could not be localised. `atlas_log` already exists for text
meant for whoever reads the build, and stays exactly as it is.

## Decision

**D1. One new import, `atlas_say`.**

```c
/** Put a message in front of the person watching, named by a key in this mod's own table. */
ATLAS_MOD_IMPORT("atlas_say")
int32_t atlas_say(const char* key, int32_t key_len, const int64_t* args, int32_t arg_count);
```

The key is the **suffix** of a key: the host prefixes `mod.<name>.` itself, so a mod names only
its own strings. Claiming another mod's words, or the engine's, is not forbidden but
unrepresentable — the arrangement ADR-0015 D6 used for a mod's source identifier, applied to its
voice. Arguments are **integers**, up to four, formatted in decimal and substituted positionally
by `text::substitute`; a mod has no text to pass, by D2's reasoning, and integers are what the
interface already speaks at every other boundary. Returns 0, or a negative code: the key is empty
or too long (`ATLAS_ERR_RANGE`), too many arguments (`ATLAS_ERR_RANGE`), or this tick's budget is
spent (`ATLAS_ERR_EXHAUSTED`).

*Made precise during M24, 2026-09-24.* Three refusals the sketch did not name. **A key suffix is
at most `ATLAS_MOD_MAX_SAY_KEY` (64) bytes of `[a-z0-9_.]`**, with no leading or trailing dot and
no empty segment, so a suffix cannot climb into a parent namespace by beginning with a dot or
produce a key that no table could spell; anything else is `ATLAS_ERR_RANGE`. **Everything that
can be judged without touching guest memory is judged first**, so a malformed call is a refusal
the mod sees rather than a trap that disables it. And **an argument pointer outside the mod's
memory traps**, disabling the mod, which is ADR-0015's rule for a bad pointer; the arguments are
checked by the host against the runtime's own bounds rather than by the import's signature,
because the signature's buffer form counts bytes and the count here is of integers.

**D2. Nothing comes back.** No import returns resolved text, a string's length, or whether a key
exists. A mod cannot learn what it said looked like, which is the property that keeps the
interface locale-blind and therefore lockstep-safe.

**D3. A mod ships its own table beside its module.** `<name>.strings.json`, in the same
`atlas-strings` format as every other table, read from the mods directory through `VirtualPath`
and added to the application's catalogue with `text::Catalog::add_table`. **Every key in it must
begin with `mod.<name>.`**, checked before anything is added; a table that names anything else
is refused whole, and the mod still loads — a mod whose words are missing shows its keys, which
is what a missing key does everywhere else. A mod with no table is not an error.

**D4. Messages are presentation, bounded, and never hashed.** The host queues what a mod says —
the full key and its arguments, as integers — in a queue the application drains each frame.
Never in simulation state, never in a save, never in a replay, never on the wire. A per-tick
budget beside the log's, `ModLimits::messages_per_tick`, with its reason: a person reads perhaps
a message a second, so a mod saying sixteen things in one tick has gone wrong, and anything past
the budget is dropped and counted rather than queued. The queue is bounded too, because an
application that never drains it must not grow it without limit.

*Changed during M24, 2026-09-24.* **Both bounds live in `ModHostConfig`**, as
`max_messages_per_tick` (four) and `max_queued_messages` (sixty-four), not in `ModLimits`. The
limits in `ModLimits` bound what a guest can cost the *runtime* — instructions, memory, file size
— and are checked by the loader; these bound what a host is willing to *hold* for an application,
beside the log's rate limit, which already lived in the host's configuration. A message refused
because the queue is full still spends that tick's budget, so a mod cannot learn the queue's
state by retrying. `ModHostStats` counts `messages_said` and `messages_dropped`.

**D5. The application decides where messages appear.** The engine hands over keys and integers;
the lab resolves them through its catalogue and shows them in a small overlay panel of recent
messages, and prints each one resolved in a headless run so an integration case can read it.
What a game does with them is the game's.

*Changed during M24, 2026-09-24.* **The lab has no message panel.** It resolves each message,
prints it, and logs it at information level, and the overlay's log console — which already
exists, filters, and scrolls — is where a windowed run shows it. A panel would have been a second
list of recent lines beside one that is already there, built for a single demonstration mod. The
integration cases read the printed line, as D5 intended. **Only the lab's single-process path has
a catalogue**: its loopback table and its socket peer pass none, so under lockstep a mod's
messages show as their keys. That is what a missing key shows everywhere and is not a divergence,
because nothing said is compared; it is recorded so nobody reads it as a bug.

**D6. The demonstration mod speaks.** `tools/gen_mods.py` gains a third module, `herald.wasm`,
that says one keyed message with one argument every so many ticks, with `herald.strings.json`
beside it. **The synthetic mod is not changed**, so its committed bytes, the three proofs built
on it, and every hash that depends on them stay exactly as they are.

**D7. The import count becomes nine, and the clock stays last.** `atlas_say` goes into the host's
table before `atlas_debug_clock_ns`, as the table's own comment requires, and `kSafeImportCount`
becomes nine. The loader's allow-list is derived from the same table, so it follows without a
second edit.

## Alternatives

**An import returning resolved text.** Ruled out by the Context: locale-dependent bytes inside a
lockstep participant.

**Raw UTF-8 out of the mod.** Ruled out by ADR-0016: unlocalisable, and a second way to put words
on screen that no `--text-check` could see.

**Strings as arguments, not only integers.** A message like *"{0} took {1}"* would want names.
Rejected for now: a name a mod could pass is one it read from a view, and views are bytes whose
meaning is the application's — so the application is also who should turn that byte into a name.
Recorded as deferred with the trigger: a message that needs a name the application cannot supply
from an integer.

**Messages in simulation state, so a replay reproduces them.** Rejected: a replay reproduces what
changed the world, and nothing a mod says changes it. A replay re-runs no mods anyway (ADR-0015's
first proof), so a message stored there would be a message nobody could produce again.

**A message per command instead of an import** — the mod submits a command whose handler posts a
message. Rejected: it would put presentation through the simulation's queue, stamp it, hash its
effect, and make every peer's rules library handle text, which is the one thing the command path
exists to keep out.

## Consequences

- `atlas_mod.h` gains one declaration and one limit; `engine/script/src/mod_host.cpp` one import,
  one queue and one budget; `ModHost` a way to drain what was said. `kSafeImportCount` is nine.
- The lab loads `<name>.strings.json` beside a mod, shows and prints what it says, and gains
  `herald.wasm` as its demonstration; `--text-check` learns to resolve a mod's keys when given one.

  *Corrected during M24, 2026-09-24.* **`--text-check` did not learn that, and cannot.** It
  resolves a list of keys named in source, and a mod's keys are chosen at run time by a guest the
  engine never inspects: there is no list to resolve. What can be checked is checked where D3 put
  it — every key in a mod's table lies in its namespace, refused whole otherwise — and a key a mod
  says without defining shows itself, as every missing key does.
- The hostile-module suite gains cases for every refusal in D1 and D3, through a real guest.
- ADR-0015's status line gains a dated forward pointer; `CLAUDE.md`'s mod rules gain one line:
  a mod speaks by key, one way, and reads no text.
- **The risk is small and worth naming**: this widens the untrusted surface by one import that
  copies bytes out of guest memory. It reads at most a bounded key and four integers: the key
  bounds-checked by the runtime's own signature before the host sees it, and the integers by the
  host against the runtime's bounds, as D1's note describes — the treatment `atlas_log` and
  `atlas_submit` already get.

## Rollback cost

Low. Removing the import is removing one table entry, one declaration and one queue; a mod
written against it would then fail to load, which is the loader refusing an unknown import — the
behaviour it already has. Nothing is serialised, hashed or sent, so no format or protocol version
is spent.
