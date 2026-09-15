# 0005 — Error handling and ownership model

## Status

Accepted, 2026-09-14.

## Context

An engine fails in two distinct ways, and conflating them produces both crashes and silently
wrong behaviour.

The first is a violated programmer invariant: an index out of range, a null that the
contract said could not be null, a call from the wrong thread. These are bugs. Continuing
past one produces undefined behaviour and unreadable failures later.

The second is a legitimate runtime failure: a missing file, a malformed asset, a shader that
will not compile, a GPU device that cannot be created, a save file from a future version.
These are expected. They must be reported with enough context to act on, and they must not
crash.

Atlas also spans module and thread boundaries where exceptions are a poor fit, and depends
on C libraries such as SDL that report failure by return value.

## Decision

**`atlas::Result<T>` is `std::expected<T, atlas::Error>`,** with `Status` as the void case.
The standard type is used directly rather than a first-party equivalent: it is available on
every target toolchain, and a home-grown alternative would be work with no benefit.

**`Error` carries** a code from a module-blocked enumeration, an owning message, the source
location where it was produced, and an optional native error code from a third-party API. It
accumulates context as it propagates, so a failure deep in an importer reaches the caller
describing what was being attempted, not just what went wrong. The category is derived from
the code rather than stored separately.

**Exceptions are never thrown by engine code and never cross a module or thread boundary.**
They remain enabled, because third-party code and the standard library use them. `try` and
`catch` appear only in commented boundary wrappers: filesystem operations where the
`error_code` overloads are unavailable, allocation failure, the test framework, and any
future scripting integration. Allocation failure is fatal; there is no recovery policy for
it in v0.1. A lint check greps for `throw` and `catch` under `engine/` against an explicit
allow-list, so the rule is mechanical rather than remembered.

**Assertions are for violated invariants only.** `ATLAS_ASSERT` checks a programmer
invariant and aborts; it compiles out in release. `ATLAS_VERIFY` keeps its side effect in
release builds. Neither is ever used for a recoverable user or data error, which must return
an `Error`. Untrusted input is validated with real checks, never with assertions.

**Ownership is explicit by default.** RAII for every owned resource. Values and
`unique_ptr` by default. `shared_ptr` requires a written reason; the single sanctioned case
is immutable presentation snapshots, which are pinned by a renderer across a frame while the
simulation moves on.

**Long-lived engine resources use generation-counted handles.** A `Handle<Tag>` is an index
plus a generation; a pool bumps the generation on release, so a stale handle is detected
rather than dereferenced. Generation zero is the null handle. This replaces raw pointers
wherever a reference can outlive what it refers to.

**No hidden global mutable state and no service locators.** The log sink registry is the one
permitted process-wide object, initialised and destroyed explicitly by the composition root.

## Alternatives

**Exceptions throughout.** Idiomatic C++ and genuinely good at propagating failure through
deep call stacks without ceremony. They interact badly with the parts of this engine that
matter: they cannot cross a thread boundary usefully, they make cost hard to reason about in
hot loops, and they make failure paths in a plugin or scripting boundary fragile. Rejected
for engine code; still caught and translated at third-party boundaries.

**Error codes only, in the C style.** Cheap and universally compatible, but an integer
carries no context, and out-parameters for results are easy to misuse. The whole benefit of
context accumulation is lost. Rejected.

**A first-party `Result` type,** or an external library such as `outcome` or `tl::expected`.
More control over the interface and richer combinators. `std::expected` is standard,
available everywhere Atlas targets, understood by tooling, and requires no maintenance.
Rejected in favour of the standard type.

**`shared_ptr` broadly, for simplicity.** Makes lifetime questions disappear by making them
unanswerable, and hides ownership exactly where it needs to be visible. Rejected.

## Consequences

- Failure propagation is verbose. Every fallible call returns a `Result` that the caller
  must handle or forward. This is the intended trade: visible failure paths.
- The distinction between bug and failure must be made consciously at every fallible call
  site, which is the point.
- `Error` allocates for its message. This is acceptable on failure paths and unacceptable in
  hot loops, so hot loops must not produce `Error` objects per element.
- Generation handles cost an indirection compared to raw pointers, and pay for it by turning
  a class of use-after-free into a clean, reported failure.
- The rules are enforced mechanically where possible: the exception grep, the SPDX check,
  and the module boundary check all run in CI and in the pre-completion hook.

## Rollback cost

Medium to high. The error type appears in every fallible signature in the engine, so
changing it is a mechanical but wide edit. The ownership and handle rules are more
structural: abandoning them would not require a rewrite so much as give up the guarantees
they provide. Neither is expected to be reversed; both are cheap to extend.
