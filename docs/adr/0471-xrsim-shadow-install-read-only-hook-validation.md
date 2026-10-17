# ADR-0471: Validate the current hook live from a read-only shadow install

- Status: Accepted (scheduled autoloop wake 70, GH #81 follow-up)
- Date: 2026-10-18
- Related: GH #81, GH #80, GH #71, GH #68 (iteration vehicle), GH #79, ADR-0470
  (hook-build identity), ADR-0468 (sim input bridge), ADR-0467 (install point)

## Context

ADR-0470 made every live XRSIM run *identify* the hook build it exercised, but
it could not change the fact that the game loads `opengl32.dll` from **its own
executable directory** (implicit imports resolve the app directory first) -- a
*deployed* copy the loop must never write. So the whole ladder (#71 req 1, #72
req 1, #67 step 3, #68 req 5, #79 hook half, #76 step 1) stayed "one interactive
deploy" away, and each wake could only add offline scaffolding around a claim it
could not test.

## Decision

Give the harness a **read-only shadow install**: a scratch tree that mirrors a
real install and puts the repo-built hook in the hook slot.

1. **Plan (`shadow_plan`).** List the source install and mirror it structurally:
   every directory becomes a **directory junction** (reparse point; crosses
   volumes, needs no admin), every file becomes a **copy** -- *except* the
   program directory `System\Win32_x86`, which is materialised as **real files**
   so the loader's app-directory resolution picks the shadow's hook. The
   deployed hook and the stale hook log are **never** carried over.
2. **Build (`build_shadow`).** Idempotent: remove and rebuild the destination,
   create the junctions (`mklink /J`) and copies, then copy the built
   `dist\opengl32.dll` into the hook slot. It **never writes to the source**.
3. **Manifest (`shadow_manifest`).** Records source/dest/game dir/exe plus the
   hook and built-hook sha256 and `fresh` (`in_shadow == built`), so a run can
   prove which build it exercised. `xrsim.py --shadow-install <source>` builds it
   and sets `--game-dir`; `--shadow-dir` overrides the scratch location
   (default `%TEMP%\riddickvr-shadow`); the report gains a `shadow` key.

This is **not a deploy**: no source byte is created, modified or removed.

## Consequences

- `--require-fresh-hook` (ADR-0470) is now *satisfiable* offline: a live run can
  exercise current source, so a deploy-gated claim becomes attributable without
  the forbidden write. The one interactive deploy is no longer required to
  validate hook source.
- The bulk of the install (the 10 GB of `Content*`) is a junction, so a shadow
  costs ~14 MB (the program directory) and builds in well under a second.
- Honest limits: it needs an NTFS source with junctions (or developer mode);
  `System\Win32_x86` is copied, so a run's log/session files land in the shadow,
  not the install; and a game that *wrote* through a `Content` junction would
  touch the source (no such write was observed -- the install's program directory
  hashed identically before and after four runs).

## Validation

`tools/test_shadow_install.py` (7 cases): the plan mirrors directories as
junctions and files as copies; `System`/`System\Win32_x86` are materialised (not
junctioned); the deployed hook and stale log are skipped (and the plan carries
names, not bytes); a missing source is rejected; a built shadow routes the hook
slot to the built hook while assets read through the junction to the source; a
rebuild is idempotent and **leaves the source byte-identical**; the manifest
flags a stale hook; the summary line carries the verdict.

Live (allowed sim exception `RIDDICKVR_SIM=1`, **no deploy**, 2026-10-18):
`python tools/xrsim.py --shadow-install "F:\The Chronicles of Riddick - Assault
on Dark Athena" --reach-gameplay --duration 40 --sim-confirm keyboard
--require-fresh-hook` printed
`[xrsim] shadow install … (14 junction(s)) -> hook 14867cf370f0… = current build`
and the graded check **PASSED**: `deployed_hook_matches_build … run exercised the
current build`. The same run turned the previously deploy-gated
`comfort_vignette_applied` **PASS** (`max=1.000 while moving`, 569/630 frames)
where the stale-hook baseline was `max=0.000`. The install's
`System\Win32_x86` hashed byte-identical before/after four runs, and its
`riddick_vr.log` was untouched (the hook logged into the shadow).

First live datum from the GH #80 bridge, which no stovepiped offline test could
give: `XRSIM input bridge: gate on route=keyboard installed=yes getProcCalls=90
inputProcRequests=1 createDevicePatched=0 keyboardWrapped=0` -- the engine
resolved **one** of the bridge's procs *through* the DllMain-swapped
`GetProcAddress`, i.e. the swap **beats** `CInputContext_DInput`'s constructor
(the ADR-0467 residual), and it was **XInputGetState**, not `DirectInputCreateA`
(`createDevicePatched=0`), so the `keyboard` route had nothing to wrap. Recorded
on GH #80; the `gamepad` route was tried and the 40 s run still never left the
letterboxed page (GH #71), so the reach question is *not* closed by this ADR.
