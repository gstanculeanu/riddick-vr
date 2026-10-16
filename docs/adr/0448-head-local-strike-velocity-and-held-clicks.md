# ADR 0448: Head-Local Strike Velocity, Held Melee Clicks, and Guard Hysteresis

## Status
**Accepted, unvalidated on a headset.** Addresses GH #65, #62, #52, #50 and #51
(all "Refs": each needs a Quest session to confirm) plus the input-safety part
of GH #68. Builds on ADR-0434 (guard envelope) and ADR-0437 (strike thresholds).

## Context
Swinging the controller did not reliably punch, and guarding moved the camera.
Three causes were found in code:

1. The strike classifier compares hand velocity with head-local thresholds
   (-Z is forward), but `gripPose.linearVelocity` is in world/stage space. A
   punch only registered while the player faced stage -Z.
2. Melee sent mouse DOWN and UP back to back in one frame. The engine polls
   input once per tick, so a zero-length click is often never seen.
3. The fist guard sits inside the ADS corridor, and a left-trigger block counts
   as "aim held", so moving the arms while guarding steered the camera. With no
   melee weapon the slot defaults to 1, so the old `meleeEquipped` check did not
   help.

## Decision
- `ToHeadYawLocal` (`combat_strike.h`) rotates a world velocity into the HMD
  yaw-only frame (+X right, +Y up, -Z body-forward). Yaw only, so looking up
  does not turn a forward punch into an upward one. A degenerate quaternion
  passes the velocity through. `Update` applies it to `rVel`/`lVel` when
  `HeadLocalVelocity=1` (default); `0` restores the old world frame.
- `InputHoldScheduler` (`input_hold.h`) holds a click for `MeleeClickHoldMs`
  (default 60, 0-250, 0 = old instant pair). DOWN is sent at once, UP at the top
  of a later `Update`, without sleeping. A second press of the same input only
  extends the hold. Releases skip the foreground guard (but honor sim mode), so
  a focus change cannot leave a button stuck. `CameraHook::Shutdown` releases
  everything. 214 adjacent DOWN/UP pairs became `SendMouseTap`.
- Direction keys in the generic classifier are held with the click
  (`SendKeyHeld`). The dual cross-slash sends one held slash, because A and D
  held together cancel. A classified strike that no branch dispatches now falls
  back to a plain click.
- `UpdateGuardPose` (`combat_guard.h`) is the old engage/release timer block
  (40 ms engage, 80 ms release, instant release on a punch), now tunable
  (`GuardEngageMs`, `GuardReleaseMs`, `GuardSteadySpeed`) and unit-tested.
- `UpdateCombatStance` takes `CombatStanceThresholds` (`StanceEnterTrigger`,
  `StanceExitTrigger`, `StanceHoldMs`). `MeleeRequiresStance=1` (default 0)
  drops swings outside the stance.
- `ComputeAimPull` (`aim_pull.h`) keeps the old constants and adds
  `guardPoseLike`: both hands in the guard envelope, or within 300 ms of a
  swing, means no aim pull and no ADS acquisition.
- `SendKey` now honors `RIDDICKVR_SIM` like the mouse helpers, and the hook
  logs once that desktop input is disabled (#68).

## Consequences
- Punches work in any facing. Every other gesture that reads `rVel`/`lVel`
  also moves to the head frame; their thresholds were already head-local.
- A held click can overlap the next frame's gesture. Pairs interleaved with
  gesture taps, and the grenade cook, were left as they were.
- Several taps in one frame (for example `SendMouseTap` twice) collapse into one
  held click.
- 60 ms is a first guess. Tune `MeleeClickHoldMs` and the `Strike*` keys on a
  headset.
