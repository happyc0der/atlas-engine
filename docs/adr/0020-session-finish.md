<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0020 — A session can finish

## Status

**Proposed**, 2026-09-23, written at M21's gate under [ADR-0018](0018-chess-probe.md) D4.
Accepted when M21 lands it. Nothing in `engine/net` changes before this record is read.

Amends [ADR-0017](0017-lockstep-transport.md) decision 5's list of the ways a session ends, by
adding the one that is not a failure. Nothing in 0014 or 0017 is superseded: a silent peer
still ends the session, the gate still reads no clock, and a divergence still stops.

## Context

A lockstep session ends five ways today, and every one of them is a failure: a protocol
violation, a version mismatch, an inbox overflow, a divergence, a peer that went quiet. A peer
that *chooses* to leave sends `Bye{Quit}`, and the peer that receives it reports an error —
*"peer 1 left"* — and exits non-zero (`session.cpp:424-428`). There is no way for a session to
end because the thing it was running is over.

**The lab has been living with this since M17.** Its socket peer runs to `--ticks`, prints its
hash and returns; the session is destroyed without a word and the hub's destructor says goodbye
for it, delivering what was queued first, over a bounded drain. The *other* peer must reach its
own bound before it notices the disconnect, or it fails with *"peer disconnected"* — and it
notices through `hub->status().ended`, which it checks at the top of every frame, before the
poll that would have applied the turns that just arrived. M17 narrowed that window by making the
drain deliver the queued turns; it did not close it, because there is nothing in the protocol
that says *"I am done at tick T and so should you be"*. The two processes agree on every hash
and then part by one of them hanging up.

**Chess cannot live with it.** A game ends at checkmate, at the same tick on every peer, with
the result in a hashed table. Both players' processes should exit zero having agreed on the
whole game. Today the first to stop would make the other fail, and a socket integration case
that plays to mate would have to accept a non-zero exit as success — which is the exact kind of
test this project refuses to write.

**Two smaller facts, found while surveying.** `sim::PollReport::closed` exists so a source can
say it will produce nothing further, and `Session::poll` sets it in four places immediately
before returning an error, so the report it is set on is discarded and no caller has ever seen
it (`session.cpp:443-488`). That is dead code with a meaning attached, and the meaning is
wrong for a session anyway: under ADR-0017 D5 a peer leaving *ends* the session, so it is an
error and not a closed source. And `Bye` carries a reason and a string, no tick — so even a
`Finished` reason would not say *when*.

## Decision

**D1. A new message, `Finish`, and protocol version 2.**

```cpp
/// This peer will run no tick after `last_tick` and needs no turn after it. Sent once, when
/// the application says the run is over — a game reached its result, a bound was reached.
struct Finish {
    Tick last_tick = 0;
};
```

`MessageType::Finish = 6` joins the five types; `kProtocolVersion` becomes 2, so a peer on
version 1 is refused at the handshake as the protocol already refuses a version it does not
know. A finish is *not* a `Bye`: a goodbye means the sender is gone and nothing further will be
accepted from it, while a finishing peer is still there, still delivering the turns and hash
checks its partner needs, and still comparing the ones it receives. Overloading `Bye` with a
reason that means the opposite of goodbye was the plan's sketch and is rejected below.

**D2. Two new states, and `finish(last_tick)`.** `SessionState` gains `Finishing` and
`Finished` after `Running`. `Session::finish(last_tick)` is allowed only while `Running` and
only once this peer has announced every turn up to `last_tick` — a peer that finished before it
had said what it would do on tick `last_tick` would leave its partner unable to run it. It sends
`Finish{last_tick}` and moves to `Finishing`. In `Finishing`, `send_turn` refuses a tick after
`last_tick` and accepts one at or before it; `send_hash_check` likewise; `poll` continues to
deliver everything that arrives, exactly as in `Running`.

**D3. `Finished` is decided from messages, in order, by each peer on its own.** A session is
`Finished` when all of these hold: this peer has called `finish`; every other peer has sent
`Finish` with the **same** `last_tick`; every other peer's turns for every tick up to
`last_tick` have been received; and every other peer's hash check for the last checkpoint at
or before `last_tick` has been received and compared. Two peers that finish at different ticks
have disagreed about what the run was, which is a protocol violation and ends the session as
one — the lab's two processes must be given the same `--ticks`, and chess's two peers reach the
same result at the same tick or their hashes already differ.

Because the condition is decided from messages that arrive in order on a reliable channel, a
disconnect that follows a peer's `Finish` and its last turn is *expected*, not a failure: the
finishing peer has said everything it will say, and the application ignores the link ending
once the session is `Finished`. That is what closes M17's window rather than narrowing it — the
protocol now says *"I am done at T"*, so a hang-up after it carries no information.

**D4. `PollReport` gains `finished`.** True on the poll in which the session became `Finished`
and on every poll after, which are not errors. A finished session's `poll` delivers nothing and
fails nothing. `running()` is false in `Finishing`; a new `finishing_or_running()` is not
added — callers that need the distinction ask `state()`.

**D5. No new clock.** A peer in `Finishing` is still subject to the transport's silent-peer
deadline: a partner that never sends its `Finish` and never disconnects is a silent peer, and
the session ends as it would have in `Running`. Nothing here adds a timer, and the gate still
reads none.

**D6. `PollReport::closed` is not the session's to report, and the session stops pretending.**
The four assignments to it before an error return are removed. `command_source.hpp`'s comment
on `closed` gains one sentence naming who reports it — a replay that ran out, a mod that broke —
and saying that a peer leaving is an error under ADR-0017 D5 and a run ending is `finished`.
This corrects the M18 plan, which listed *"`PollReport::closed` never delivered"* as a bug to
fix by delivering it; the honest fix is to stop setting it, because a session has nothing that
it means.

**D7. The lab finishes rather than hanging up.** Both the socket peer and the loopback table
call `finish(max_ticks - 1)` when the last tick has run, then poll until `finished()` and exit
zero. `hub->status().ended` after `Finished` is ignored. The killed-peer case is unchanged: a
peer that dies sends no `Finish`, and the deadline ends the session as before. A new case
starts two processes with **different** `--ticks` and asserts both exit non-zero naming the
mismatch, which is the proof that D3's equality is enforced rather than assumed.

**D8. Chess calls `finish(tick)` on the tick its result is set**, in M22. Every peer sets the
result at the same tick from the same hashed tables, so every peer finishes at the same tick
by construction, and D3's equality holds without the application arranging it.

## Alternatives

**`ByeReason::Finished` carrying a tick.** The plan's sketch, and one message type fewer.
Rejected because a `Bye` ends the receiver's session on arrival — that is what every handler of
it does — while a `Finish` must leave the session running until the finishing peer's remaining
turns and checks have arrived. Making `Bye` mean "gone" in five cases and "still here" in one
would put a branch on the reason into every place a goodbye is handled, and a reader of the
protocol would be right to ask why a farewell is not one.

**A finish decided by the transport: stop after a bound and close.** What the lab does today.
Rejected because it is the window this record exists to close, and because a socket game to
mate cannot be expressed as a tick bound known in advance.

**A coordinator that declares the end.** Peer zero says the run is over and everyone stops.
Rejected because it makes peer zero's failure a special case in a design where no peer is
special, and because it needs the same *"has everything up to T arrived"* condition anyway.

**Finishing implicitly when the result table is set.** Rejected because `net` has no idea
what a result is and must not acquire one; the application knows when its run is over and
says so. `Session::finish` is the one call, and the lab's bound and chess's checkmate both go
through it.

## Consequences

- `protocol.hpp`, `message.hpp` and `message.cpp` gain the type, the codec and the version
  bump; every peer must be the same build, which the handshake already requires.
- `session.hpp` and `session.cpp` gain two states, `finish`, `finished()`, the per-peer
  bookkeeping D3 needs — highest turn received, last hash check compared — and the finished
  condition. Four dead assignments go.
- `command_source.hpp` gains `finished` and a corrected comment on `closed`.
- `apps/lab/main.cpp` finishes in both session modes; `lab_checks.py` gains the mismatched-bound
  case; the socket-agree case asserts both peers say they finished rather than merely both
  exiting zero.
- ADR-0017 D5's list of endings gains a dated forward pointer to this record.
- **The condition in D3 is the risk.** A finish that fires one message too early leaves a
  partner waiting for a turn that will never come, which the deadline turns into a failure
  rather than a hang — safe, but it would make a finished game look like a lost peer. Every
  clause of the condition gets a test that removes it and watches a peer fail.

## Rollback cost

Moderate. The message type and the version bump are additive and reverting them is deleting
them; the states and the bookkeeping are contained in `Session`. What would return is the lab's
hang-up at the end of a run and the window that goes with it, and chess would have no way to
end a game over a socket with both peers exiting zero.
