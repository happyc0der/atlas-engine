<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0008 — Numeric policy and the simulation save format

## Status

Accepted, 2026-09-15.

## Context

M6 makes the simulation kernel real, and two decisions could not be deferred past it.

**What kind of numbers authoritative state is made of.** Once a state hash is written to a
file, the arithmetic that produced it is frozen. Floating point is the obvious hazard, but
not for the obvious reason: IEEE 754 arithmetic is exactly specified. What is not specified
is whether a compiler contracts a multiply and an add into a fused instruction, what a
library's transcendental functions return in the last bit, and what happens when an
out-of-range value is converted to an integer. Those differ between architectures today, on
the two Atlas already targets.

**What a save file looks like.** ADR-0007 chose indented text for scenes, for properties
that matter when a person reads and reviews the file. None of those properties apply here.
Simulation state is generated, large, read only by the engine, and hostile when it arrives
from elsewhere.

## Decision

### Numeric policy

**Authoritative state is integer by default.** Anything hashed, saved, or replayed is built
from explicit-width integers unless there is a recorded reason otherwise.

**Floating point in authoritative state requires a recorded justification and a passing
cross-architecture golden-hash comparison.** Not a general prohibition, a gate. The
justification goes in this file; the comparison is the golden-scenario test.

**Fused multiply-add is off** (`-ffp-contract=off` on Clang and GCC, `/fp:precise` on MSVC),
project-wide since M0. arm64 contracts by default and x86_64 without the relevant
instructions does not, so leaving it on would make the same source produce different results
on the two tier-one platforms.

**Fast-math is forbidden everywhere.** It permits reassociation, which makes addition
non-associative in a way that depends on the optimiser.

**`long double` is forbidden.** Its width differs across platforms.

**Transcendental functions from the mathematics library are forbidden in authoritative
code.** Their results are not specified to the last bit and differ between implementations.
Presentation code may use them freely; it never feeds back into state.

**Float-to-integer conversion is valid only in range.** Out of range it saturates on arm64
and yields an indefinite value on x86_64. The sanitizer builds enable the
`float-cast-overflow` check.

**Floating point is hashed by bit pattern**, never through a formatted decimal.

**No container whose iteration order is unspecified may feed a hash, a save, or a
reduction.** Lookups in one are fine; iterating one is not. Every ordering the kernel
exposes is by a stable identifier, imposed on the way out rather than inherited.

### Save format

**Binary, explicit-width, little-endian.** Not the host's order, which is not a format.

**Self-describing and versioned.** A magic value, a format version, and the version of the
hash algorithm the stored hash came from. A file from a newer build is refused rather than
read, because reading it would silently drop what that build added and the first sign would
be data disappearing on the next save.

**Bounds before allocation.** Every count is checked twice: against a ceiling, and against
the bytes that actually remain. A small header claiming a million rows is the cheapest attack
on a binary format, and the second check removes it. Counts are compared as 64-bit values
before narrowing, so a hostile number cannot wrap into a plausible one.

**Trailing bytes are refused.** A file only partly understood is not a file that was
understood.

**The state hash is stored and checked after loading.** It is a statement about the state, so
it is verified against the state rather than against the bytes. A mismatch means the file is
internally inconsistent.

**A failed load leaves the world empty.** Tables are cleared before anything is read into
them, so a failure part way through cannot leave some tables holding the file's rows and
others holding the previous state. An empty world is a state a caller can recognise.

**Tables are matched by identifier, not by position.** A file written when the tables were
registered in a different order still reads. A table in the file the build lacks, or in the
build the file lacks, is an error: it means the save and the build disagree about what the
state is.

**No migration code exists.** There is no version 2 to migrate from, and writing migration
for an imagined change would be writing untested code. The version check is what makes
deferring it safe: a file this build cannot read is refused rather than misread.

## Alternatives

**Fixed-point throughout, with a project-wide fixed-point type.** Stronger than "integer by
default", and it removes a class of mistake entirely. It also imposes a numeric type on every
piece of state before anything is known about what the state looks like, and fixed-point has
its own overflow and precision traps that are worse for being less familiar. Rejected for now;
integers make the same guarantee with none of the surface, and a fixed-point type can be added
later as one more integer representation.

**Floating point with a strict-mode compiler contract.** Tempting, because much simulation
arithmetic is naturally fractional. The contract is not portable enough to rely on: MSVC's
`/fp:strict` and Clang's `-ffp-model=strict` differ in what they guarantee, and neither
addresses the library functions. Rejected as the default; permitted per case, behind the
golden-hash gate.

**Text for saves, as scenes use.** Reviewable, and worth nothing here: nobody reads a
simulation save, and the size and parse cost would be real. Rejected, and the split from
ADR-0007 is deliberate rather than an inconsistency.

**A checksum instead of a state hash.** A checksum detects corruption. The state hash detects
corruption *and* ties a file to a point in a replay, because it is the same number the replay
compares. One mechanism serving both is fewer things to keep in step.

**Compression.** Would reduce file size and adds a dependency, a failure mode, and a decision
about which algorithm. No measurement yet says the size is a problem. Deferred until one does.

## Consequences

- Simulation authors cannot reach for floating point without a conversation. That friction is
  the point; it is also friction, and it will be felt.
- The golden-scenario test is a fixed number in the repository. A change that alters
  simulation results makes it fail, which is the question being asked rather than an
  obstruction: either the change was meant to alter results, and the number is updated with a
  reason, or it was not, and there is a bug.
- Save files are not readable by people. Debugging one means a tool, and there is not one yet.
- The format version and the engine version are unrelated, on purpose. The format changes when
  the layout does.
- Cross-architecture reproduction is measured, not promised. What has been measured is in
  docs/DETERMINISM.md, and what has not been is listed there too.

## Rollback cost

**Low for the save format.** It is confined to `engine/simulation/src/save.cpp` and the
`SaveWriter` and `SaveReader` pair, and nothing has shipped that would need migrating.

**High for the numeric policy.** It is a constraint on code not yet written rather than a
body of code, which makes it cheap to keep and expensive to abandon: relaxing it later would
invalidate every stored hash and every recorded replay produced under it.
