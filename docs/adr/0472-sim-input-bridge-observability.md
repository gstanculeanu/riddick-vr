# ADR-0472: Make the sim input bridge observable, so the reach is decidable

- Status: Accepted (scheduled autoloop wake 71, GH #71 req 1 / GH #80 follow-up)
- Date: 2026-10-18
- Related: GH #71, GH #80, GH #81, ADR-0466 (input is DirectInput/Raw Input),
  ADR-0467 (install point), ADR-0468 (interception mechanism), ADR-0471 (shadow
  install)

## Context

GH #71 requirement 1 -- reach live gameplay from the harness so every
player-visible fix can be validated without a headset -- is the loop's top
blocker, and the sim input bridge (GH #80) is the only remaining route to it.
Wakes 63-70 built the bridge and, with the read-only shadow install (ADR-0471),
can finally exercise current hook source live. But the bridge was
**unobservable**: its only report line was a one-shot counter dump

```
XRSIM input bridge: gate on route=keyboard installed=yes getProcCalls=90
inputProcRequests=1 createDevicePatched=0 keyboardWrapped=0
```

which cannot answer the questions the next change depends on:

- *which* input proc the engine actually resolved through the swapped
  `GetProcAddress`, and whether the bridge owned it;
- whether the engine ever *polls* the wrapped entry point (a fabricated state
  that is never read is indistinguishable from a broken wrapper);
- what the real API returned (a never-connected XInput device returns `1167`
  `ERROR_DEVICE_NOT_CONNECTED`, so the ORed button bytes are invisible to an
  engine that checks the pad first);
- how many confirms the bridge actually fabricated.

The counter dump also invited a **misreading**. `inputProcRequests` counts only
requests the *route* owns (`SimBridgeOwnsProcName` is route-scoped), so the
wake-70 run above -- `route=keyboard` -- could only have owned
`DirectInputCreateA`, not `XInputGetState`; the recorded conclusion ("it was
`XInputGetState`, not `DirectInputCreateA`") is not derivable from that line.
The observable consequence is the opposite one: the engine *resolved* the
DirectInput entry point through the swap but never *called* it
(`createDevicePatched=0`), i.e. it took the XInput path on a machine with no
pad.

## Decision

Make the bridge report what it observed, and let the harness parse it.

1. **A closed input-proc table in the pure core** (`sim_input_bridge.h`):
   `SimInputProcNameAt` / `SimInputProcNameIndex` / `SimBridgeIsInputProcName`
   over the seven device entry points the engine can resolve
   (`XInputGetState`, `XInputSetState`, `XInputGetCapabilities`, `XInputEnable`,
   `DirectInputCreateA`, `DirectInputCreateW`, `DirectInput8Create`).
   Exact-match and case-sensitive, like `GetProcAddress` itself: a near-miss name
   must never be reported as one of ours. Unit-tested headlessly.
2. **The interceptor records every input proc the engine asks for** -- whether
   the route owns it or not -- and logs each name once
   (`engine requested <name> (owned=yes|no)`). Ownership is unchanged; only the
   observation is new.
3. **The wrappers count their own traffic**: XInput polls / fabricated press
   edges / the real call's last return code, and the DirectInput keyboard
   device's polls / injected `DIK_RETURN` presses.
4. **A single parseable `summary` line** carries the counters plus the observed
   proc list. It is emitted on a **doubling poll cadence** *from the wrappers*
   (`SimShouldLogPoll`: 1, 2, 4, ... 128, then every `kSimPollLogEvery` = 120),
   **once from the deferred-init report**, and from `DLL_PROCESS_DETACH`. The
   cadence matters twice over: the harness kills the game with
   `TerminateProcess`, so the detach line never runs on a scripted run; and a
   fixed cadence (or a first-poll-only line, whose logger may not exist yet) would
   make "the engine never read the device" and "the engine read it 12 times and
   stopped" look identical. With the doubling cadence two consecutive logged
   counts are never more than a factor of two apart, and the deferred-init line
   reports any pre-logger poll *by value* -- so the true count is always within
   one doubling of a logged one.
5. **The harness parses it** into the `input_bridge` report key
   (`parse_input_bridge`), flags the two decisive readings
   (`xinput_connected` / `xinput_device_not_connected`), and prints one summary
   line per run. It is *reported*, not graded: there is no pass criterion yet,
   only a measurement.

## Consequences

- A live run now *states* which input path the engine took, whether the wrapped
  entry point is polled, and whether the real API reports the pad absent -- so
  the next change (whatever it is) is chosen from evidence instead of inference.
- The wake-70 "it was `XInputGetState`" reading is corrected: with the route
  scoping, that run's `inputProcRequests=1` proves `DirectInputCreateA` was
  resolved and owned, while `createDevicePatched=0` proves it was never called.
- Output volume is bounded (a doubling cadence over the first 128 polls, then one
  line per 120; plus one per unique proc name and per return-code change), so a
  60 Hz poller does not flood the log.
- It observes; it does not fix. The reach can still fail with every counter
  looking correct -- that is the next wake's question, now answerable.

## Validation

- `sim_input_bridge_pure_core` (CTest): the new table is total and inverse
  (`SimInputProcNameAt(i) -> i`), case-sensitive, and rejects near-misses; and
  `SimShouldLogPoll` is tested for the properties that matter -- a power-of-two
  poll is logged, a count in the doubling cadence's gap is not, every
  `kSimPollLogEvery` is, and over 1..4000 consecutive logged counts are never
  more than a factor of two apart.
- `sim_input_bridge_win32_intercept` / `_inert` (CTest): the summary emission is
  safe before/without a live engine, and the interception/inertness contract is
  unchanged.
- `tools/test_xrsim.py`: the parser's absent case (`None`, not `{}`), the
  gate/proc/summary lines, the connected/disconnected flags, the one-line stats,
  and the `input_bridge` report key written by an `--evaluate-only` run.
- Live (allowed sim exception `RIDDICKVR_SIM=1`, read-only shadow install, no
  deploy, `--require-fresh-hook` = the run exercised the built hook): see
  `tools/autoloop/STATE.md` wake 71 for the measured counters. The headline
  reading is `inputProcRequests=2` (both `XInputGetState` and
  `DirectInputCreateA` resolved through the swap, both owned) with `polls=0` —
  reported *by value* at deferred init, so the engine holds the bridge's pointers
  and never calls them in a 120 s session that does reach a full-bleed 3D state.
  That retires the ADR-0468 "1167 keeps the bytes invisible" hypothesis: no call
  happens at all, so the confirm cannot be the reach's mechanism.
