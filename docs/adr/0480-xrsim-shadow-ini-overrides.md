# ADR-0480 — XRSIM: live hook-config tuning through the shadow's own `riddick_vr.ini`

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (this — step 3, the live viewmodel observation), GH #68 (iteration
  vehicle), GH #81 (shadow install / hook identity), GH #83 (per-run isolation),
  ADR-0471, ADR-0450 (the observation probe), ADR-0451 (the modelview capture)

## Context

The GH #67 viewmodel probe (`ViewmodelObserve=1`) landed in wake 43/44 as an
off-by-default, observation-only probe (`ViewmodelProbe [#N]: inside=… outside=…
sample=… pushes=… translates=…` + the sampled `GL_MODELVIEW`). Its `wake 43/44`
records both end the same way: **not validated live**, because the only way to
flip the flag was to edit the **game install's** `riddick_vr.ini`, and this loop
must never write the install.

ADR-0471 (wake 70) removed the *hook binary* half of that wall with the read-only
shadow install (the loader resolves `opengl32.dll` from the executable's
directory, so the shadow runs the repo build). It did **not** remove the *config*
half: the hook resolves `riddick_vr.ini` beside its own module
(`vr_config.cpp` — `GetModuleFileNameA(hSelf)` → `"%s\\riddick_vr.ini"`, then
CWD), and the shadow mirrors the **install's** ini as a real copy. So a run could
exercise current source but not a non-default flag. Every remaining item on the
deploy-gated ladder (GH #67 step 3, #68 req 5, #66/#63 suppression, #76 step 1)
needs exactly that: a live run with one opt-in key flipped, without a deploy.

## Decision

Let `--shadow-install` runs set hook-config keys in the **shadow's** program-dir
`riddick_vr.ini`, and refuse to do so anywhere else.

- `shadow_install.merge_ini(text, overrides, section="VR")` is a pure,
  offline-testable merge that mirrors the reader the hook uses
  (`GetPrivateProfileStringA`/`GetPrivateProfileIntA`): **section-scoped** (a
  same-named key in another section is left alone), **case-insensitive**, inserts
  a missing key directly after the section header, appends a missing section,
  preserves every other line (including comments) and the line-ending style, and
  is **idempotent**.
- `shadow_install.apply_ini_overrides(shadow_root, overrides)` writes it to
  `<shadow>\System\Win32_x86\riddick_vr.ini` — a **real copy** in the shadow,
  never a junction, so nothing is written back to the install.
- `shadow_install.build_shadow(..., ini_overrides=)` applies them as part of the
  build, and `shadow_manifest` records them (`ini_overrides`), so the run says
  which flags it exercised.
- `tools/xrsim.py --ini KEY=VALUE` (repeatable) forwards them. Used **without**
  `--shadow-install` it is refused (exit 2) rather than writing the game install.
  The run's report gains an `ini_overrides` key (the manifest carries the same).

## Consequences

- A deploy-gated hook flag is reachable by a live run: `--shadow-install <root>
  --ini ViewmodelObserve=1 --require-fresh-hook` runs the **repo build** with the
  probe on and proves both facts (hook identity + config) in the report.
- The override set is explicit and recorded, so "this run used
  `ViewmodelObserve=1`" is attributable instead of inferred from a hand edit.
- The merge is pure, so the section/case/idempotence properties are unit-tested
  without a game; a malformed token is rejected at parse time.

## Honest limits

- The keys are written into the **shadow**, so a run's config is whatever the
  shadow was built with plus the overrides — not the install's current ini. That
  is the point (it makes the flag attributable), but it means a shadow run does
  **not** validate an ini the user edited in the install.
- `merge_ini` rewrites only the lines it targets; it re-encodes the rest as UTF-8.
  The shipped ini is ASCII, so bytes are preserved in practice, but a non-UTF-8
  comment byte would be replaced (the hook's profile reader does not care).
- This is harness/config plumbing: it changes no rendering. Whether the probe
  actually fires is a property of the game, measured by the live run — the tool
  only makes that measurement reachable.

## Validation

- Offline: `tools/test_shadow_install.py` (+5 cases: `merge_ini` replace/insert/
  idempotence, missing-section append, malformed-token rejection,
  `apply_ini_overrides` source-safety, `build_shadow` applies overrides);
  `tools/test_xrsim.py` (+2: `--ini` parses and is refused without
  `--shadow-install`; overrides reach the shadow/report);
  `tools/xrsim.py --selftest` (+1: `ini_override_parse_and_merge`); the GH #67
  source invariant in `tools/verify_reported_fixes.py` now requires the live path.
- No C++ changed, so no `build-all.ps1`/CTest.
- Live: see the GH #67 step-3 run recorded in `tools/autoloop/STATE.md`
  (`--shadow-install … --ini ViewmodelObserve=1 --require-fresh-hook`).
