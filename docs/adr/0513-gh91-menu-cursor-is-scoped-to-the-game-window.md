# ADR 0513: Native stereo presentation (epic #57 item 1) — the display branch is held CINEMA by a *global* desktop cursor; scoping the menu cursor to the game window presents in-level gameplay as FULL-BLEED (45/52 frames) and lets `--require-parallax` grade (GH #91 / GH #76)

- Status: Accepted (scheduled autoloop wake 111, GH #91 / GH #76)
- Date: 2026-10-18
- Related: GH #91 (this — the display-branch latch, which **this ADR fixes**), GH #76
  (native stereo, epic #57 DoD item 1), GH #90 (the scoped option-1 unit), GH #72 (the
  display-branch signal and its cold-start latch), GH #43/#44 (cinema letterbox vs
  full-bleed gameplay), ADR-0512 (**whose Next 1 this implements**, and whose live line
  measured the single CINEMA `DisplayMode:` line), ADR-0478 (the cold-start latch A/B),
  ADR-0459 (the branch bit)

## Context

ADR-0512 finished the per-eye **use gate** (`use=1`, `used/frames` 99.6%) and measured the
next blocker: the renderer's **presentation branch**. In an 80 s in-level run the
`DisplayMode:` line was emitted **exactly once** (`DisplayMode: CINEMA screen …`), so
`if (!isCinema && dualUsedThisFrame)` was never taken, the harness labelled every frame
`cinema`, and `--require-parallax` could not be graded. The line is emitted **only on a
change** of the verdict, so it says nothing about *why* the branch stayed CINEMA; ADR-0512
had to infer that the surviving term was `HasMenuCursor()` (the only non-3D term left
after ruling out `Has3DThisFrame()`).

GH #91 asked for the inputs to be logged, then the term fixed.

## Decision

1. **Name the term (a pure, unit-tested helper).** `display_branch.h` gains
   `CinemaTerm`/`CinemaHoldTerm(...)`/`CinemaTermName(...)`: given the *same* inputs the
   verdict uses, it returns which term holds `isCinema` — `no3d`, `menuCursor`,
   `coldStartLetterbox`, or `none`. Its unit test pins `CinemaHoldTerm(...) != None` to
   equal `ComputeDisplayBranch(...)` across all 32 input combinations, so the probe's
   `term=` token can be trusted as the verdict's cause.
2. **Log the inputs on a cadence.** `stereo_renderer.cpp` emits a throttled
   `DisplayBranch:` line (every `kDisplayBranchProbeEverySwaps = 60` swaps, **not only on a
   transition**) carrying `cinema`, `term`, `has3d`, `has3dLatch`, `menuCursorRaw`,
   `cursorFrame`/`cursorAgo`, `cursorMenu`, `haveFill`/`fill`/`rect`, `letterboxed`,
   `fullBleedSeen`. `OpenGLProxy::GetLastCursorFrame()` exposes the cursor sighting swap.
3. **Fix the term: a menu cursor must be *over the game window*.** `CameraHook::Update`
   fed `NoteMenuCursor(GetCursorInfo().flags & CURSOR_SHOWING)` — a **global desktop**
   signal. For any windowed/sim run with a stray desktop cursor that is true on **every**
   frame, so the sustained-cursor detector latches and demotes the whole in-level run.
   The sighting is now scoped to the game window's client rect (`GetGameWindow()` /
   `s_gameHwnd`, `ScreenToClient`, `GetClientRect`), with a throttled `MenuCursor:` line
   (`showing`, `overWindow`, `pos`, `client`, `fg`) so a run states the reason.
4. Off by default unchanged (`StereoDualRender=0`): the shipped shifted capture is what a
   player gets unless the opt-in is on; this change only governs *presentation*.

## Evidence

**Offline (this wake).** `build-all.ps1 -Config Release` clean: x86 `opengl32.dll`
528896 B `a78d782073df` + x64 host; CTest **32/32 (x86)** / **30/30 (x64)**; the new
`display_branch` section 8 passes.

**Live, run 1 — the probe only (hook `5d34b07a2a72` = repo build).** Same command as
ADR-0512 (`RIDDICKVR_SIM=1`, read-only per-run shadow, `--shadow-install
"F:\The Chronicles of Riddick - Assault on Dark Athena" --reach-gameplay --duration 80
--advance-menu --advance-min-taps 8 --require-fresh-hook --require-parallax
--dump-branch any --downscale 4 --ini StereoDualRender=1`; `deployed_hook_matches_build`
PASS). `DisplayMode:` printed **one** line (CINEMA), and the new probe names the cause on
**285 of 286** samples:

```
DisplayBranch: swap=31 cinema=1 term=menuCursor has3d=1 has3dLatch=1 menuCursorRaw=1
cursorFrame=31 cursorAgo=0 cursorMenu=1 haveFill=0 fill=-1.000 rect=0 letterboxed=0 fullBleedSeen=0
…
DisplayBranch: swap=8693 cinema=1 term=menuCursor has3d=1 has3dLatch=1 menuCursorRaw=1
cursorFrame=8693 cursorAgo=0 cursorMenu=1 haveFill=1 fill=0.951 rect=1 letterboxed=0 fullBleedSeen=0
```

`menuCursorRaw=1` and `cursorAgo=0` on **286/286** samples: the desktop cursor is reported
visible on every frame. `term=menuCursor` on 285, `term=no3d` on the cold-start sample
only. `has3d=1` throughout — confirming ADR-0512's inference that `Has3DThisFrame()` is
not the cause. A second-order effect is visible too: at the end the active-rect fill is
`0.951` (full-bleed) yet `fullBleedSeen=0`, because the latch's state update is gated on
`!menuCursor` — the cursor term also blocks the cold-start latch from clearing.

**Live, run 2 — the fix (hook `a78d782073df` = repo build).** Same command;
`deployed_hook_matches_build` PASS; 80.0 s, 52 frames. The branch now moves and the
harness grades the run:

```
DisplayMode: CINEMA screen (menus/cinematics) (3Dfresh=0 menuCursor=1 latch=1 fill=-1.00 fullBleed=0)
DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0 latch=1 fill=-1.00 fullBleed=0)
DisplayMode: cold-start latch demoted a letterboxed 3D frame to CINEMA (fill=0.40 latch=1 fullBleed=0)
DisplayMode: CINEMA screen (menus/cinematics) (3Dfresh=1 menuCursor=0 latch=1 fill=0.40 fullBleed=0)
DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0 latch=1 fill=1.00 fullBleed=1)
```

- `stereo: gameplay_captures_present` **PASS** (**45 gameplay / 7 cinema** of 52) — was
  `0 gameplay / 53 cinema` (FAIL) in ADR-0512.
- `capture: gameplay_full_bleed_reached` PASS (max BarScan fill 1.00).
- `stereo_parallax_present` **now graded** (was skipped/un-gradable): `stereo_kind=shift-only
  (gameplay frames=45, max per-tile disparity spread=0)` — see the verdict.
- The probe's `term` counts: `none` 242 (gameplay), `coldStartLetterbox` 37 (the
  letterboxed splash, still correctly demoted), `no3d` 1.
- The scoping is mechanically confirmed by the new diagnostic: `MenuCursor: showing=1
  overWindow=0 pos=777,1619 client=1280x605 fg=1` — the cursor is visible on the desktop
  but sits at y=1619, **below** the 605-tall game client, so it is not a menu cursor.

## Verdict (honest)

- **GH #91 is fixed and measured**: a live in-level frame now logs `FULL-BLEED gameplay`,
  the harness labels gameplay frames `gameplay` (45/52), and the full-bleed presentation
  path is taken — so the per-eye pair is no longer blocked by the branch, and
  `--require-parallax` can grade. The root cause is measured, not inferred: a *global*
  desktop cursor was read as a sustained menu cursor.
- **No regression**: `StereoDualRender=0` stays the default; the letterboxed startup page is
  still demoted (`term=coldStartLetterbox`, `fill=0.40`); menus/cinematics still reach
  CINEMA; `gameplay_full_bleed_reached`, `eye_views_scene_faithful`,
  `both_eyes_non_black` / `same_eye_resolution` / `no_vertical_disparity` PASS.
- **Native stereo is still not *visible* parallax**: `stereo_parallax_present` FAILs with
  `stereo_kind=shift-only`, per-tile `spread=0` — the presented per-eye pair still differs
  by a single uniform horizontal shift, not depth-dependent disparity. The gate (`use=1`,
  `used=7251/7282`) and the branch are no longer the blockers; the remaining one is the
  **per-eye image itself** (GH #76/#90). Filed as a new issue.

## Limits (honest)

- The fix removes the *global* cursor term; a real pause menu whose cursor is over the game
  window still demotes (window-scoped), but no headless run exercises a real menu cursor, so
  that path is reasoned, not re-measured here. Conversely, a menu the player navigates only
  by keyboard, with the cursor outside the window, would no longer demote — the run's
  letterbox term (`fill <= 0.75`) and `!has3d` still cover the 2D/splash menu case.
- The probe samples every 60 swaps; a term that holds for fewer than 60 swaps can be missed
  between samples (the verdict itself is evaluated every swap).
- One live run per build; the harness still leans on the GH #72 label flake for the exact
  gameplay/cinema split (45/7 here). `Has3DThisFrame()` remains a 2-frame grace; the
  2004 build is untouched; no headset.

## Next

1. **The per-eye image is now the only stereo blocker (GH #76/#90):** explain why the
   presented per-eye pair is a uniform shift (`spread=0`) — read the two eye FBOs' per-tile
   disparity, and the eye-space translation actually applied per draw, so the pair carries
   depth-dependent disparity. (New issue filed from this wake.)
2. Gate on the **queried** `GL_COLOR_WRITEMASK` at the draw (ADR-0511 Next 2) or reconcile
   the tracker.
3. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0` defaults.
4. Player-visible queue GH #63/#64/#61, then the P1 bugs #45/#49/#50/#51/#65.
5. `DEFAULT_GAME_DIR` 2004-vs-2009; GH #83/#88/#89/#74.
