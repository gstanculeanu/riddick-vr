# VR Scenario Testing

The repository does not ship a custom game map or a reliable checkpoint-warp
mechanism. Use the cell block/yard when available as the repeatable quiet test
area; the exact location depends on campaign progress. `tests/scenarios/manifest.json`
is the scenario inventory. It distinguishes the headless automated smoke test
from interactions that still require an in-game manual run.

## Automated smoke test

The current mock host synthesizes tracking, controller motion, and a small
reload/NanoMED/grab interaction loop. It does **not** simulate campaign state,
spawn targets, or prove that a game-side gesture was accepted.

```powershell
python tools/scenario_tests.py --check
python tools/scenario_tests.py --list
python tools/scenario_tests.py --run mock-smoke --frames 360 --timeout 120
python tools/scenario_tests.py --run reload --frames 300 --timeout 120
python tools/scenario_tests.py --run heal --frames 300 --timeout 120
python tools/scenario_tests.py --run climb-mantle --frames 330 --timeout 120
python tools/scenario_tests.py --run grab-throw --frames 120 --timeout 120
python tools/scenario_tests.py --run melee --frames 90 --timeout 120
python tools/scenario_tests.py --run grenade --frames 100 --timeout 120
python tools/scenario_tests.py --run drone --frames 150 --timeout 120
```

This delegates to `tools/semi_play.py`; it requires the local game install and
the built mock host. Scripted controller keyframes are available for reload,
healing, climbing, throwing, melee, grenade, and drone-disconnect profiles:

```powershell
python tools/semi_play.py --scenario reload --frames 300 --timeout 120
python tools/semi_play.py --scenario heal --frames 300 --timeout 120
python tools/semi_play.py --scenario climb --frames 330 --timeout 120
python tools/semi_play.py --scenario throw --frames 120 --timeout 120
python tools/semi_play.py --scenario melee --frames 90 --timeout 120
python tools/semi_play.py --scenario grenade --frames 100 --timeout 120
python tools/semi_play.py --scenario drone --frames 150 --timeout 120
```

These profiles drive mock poses only. A matching campaign object/NPC/station is
still required before gesture log markers can be expected. The scenario
validator does not treat synthetic controller input alone as proof that the
game accepted an action.

## Manual scenarios and evidence

For a manual scenario, follow its `steps` in the manifest while the game is
running. Save a fresh hook log and at least one eye-frame capture. Record the
log's starting byte length before the attempt, then validate only new log text:

```powershell
$mark = (Get-Item '<GameFolder>\riddick_vr.log').Length
$frameMark = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
# Perform the scenario and capture an eye frame.
python tools/scenario_tests.py --validate reload `
  --log '<GameFolder>\riddick_vr.log' --since-byte $mark `
  --artifacts '<repo>\tests\artifacts' --frame-since $frameMark
```

The validator checks the scenario's log markers and that captured frame files
exist and are newer than `--frame-since`. A passing marker check proves the hook observed
the gesture; it does not replace visual inspection of the frame or in-headset
comfort testing.

**Caveat (GH #69, ADR-0446):** the hook opens `riddick_vr.log` with
`std::ios::out | std::ios::trunc` on every launch, so `--since-byte $mark` is
unreliable whenever the new run's log is shorter than `$mark` (the slice can be
empty, silently dropping markers). The XRSIM harness now recovers the
*current-run section* by its `=== Riddick VR Log Started at ... ===` header
(`tools/xrsim.py::read_fresh_hook_log` / `extract_run_section`); teaching
`scenario_tests.py --validate` and `semi_play.py` the same extraction is a
tracked follow-up.

**Caveat (GH #71, ADR-0448):** the mock host follows the *game's* uncapped swap
rate (~250-300 FPS on the startup splash), so a frame-counted run is only a
fraction of the wall-clock the title needs to finish its multi-page splash and
load a level — a 180-frame run is ≈0.6 s and never leaves the splash. Use
`python tools/xrsim.py --reach-gameplay --duration 45` to drive a run into live
3D gameplay; it asserts a full-bleed scene was reached from the hook's `BarScan`
active-rect fill (the splash is letterboxed to ≈0.36 of the window). Because the
frame-indexed scenario keyframes then play far faster than 90 Hz, deferring
scenario playback until gameplay is reached is follow-up work (#71/#72), and the
`cinemaMode` branch bit still mislabels the static 2D splash pages as gameplay
(#72).

**Caveat (GH #72 requirement 3, ADR-0454):** the very dark live gameplay frames
(mean luma ≈13, reference-crop std ≈16) make the stereo diff's vertical
registration **unmeasurable** — `best_dy` pins to the search edge and moves with
the analysis crop/downscale (measured: dy 6/7/10/12/8 from five crops of one
frame). A *bright* frame can also fail to register (frame 2400: `dy` and `dx`
both on their bounds, 24 px crop spread). That is an alignment-estimator
artefact, not a stereo defect: the render path applies only a horizontal half-IPD
shift, and the same blit gives a steady `dy=0` from every crop on a good frame.
`tools/xrsim.py` therefore gates `no_vertical_disparity` on the frame's contrast
(`MIN_DY_CONTRAST`) **and** on the optimum not being pinned to the `±8` search
edge (`DY_SEARCH_BOUND`): an unquantifiable `dy` is reported **inconclusive** (a
skip), and the report's `vertical_disparity` key carries the `ok` /
`inconclusive` / `failed` tally. A textured frame with a real, interior offset
still fails. The `frame_budget` check asserts the 90 Hz budget on the
frame-pacing percentiles (**p50 and p95 <= 11.11 ms**), not on the arithmetic
mean or the single worst frame: the mock host follows the game's uncapped swap
rate, so a one-off level-load hitch (~760 ms) dominates `max_ms`/`mean_ms`
without being a sustained 90 Hz regression (ADR-0455). `max_ms` and the drop
count are still *reported* so a real stall stays visible.

## Current coverage and limits

The scripted profiles provide deterministic controller poses and velocities,
but they cannot load a campaign checkpoint or create targets. Reload, heal,
grenade, and drone results therefore still depend on the corresponding game
context. In particular, issue #40 still needs an in-headset visual check that
ammo digits fit the crop for each relevant weapon/resolution. Treat profile
marker failures as useful evidence, not proof that a gesture is broken when its
game preconditions were absent.
