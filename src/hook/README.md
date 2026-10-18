# 32-bit Hook Component (`opengl32.dll`)

## Responsibility
The Hook component is a 32-bit (x86) dynamic link library compiled as `opengl32.dll` (or an injected payload DLL). It loads directly into `DarkAthena.exe` within the game's address space.

## Key Duties
- Export standard WGL entry points (`wglCreateContext`, `wglDeleteContext`, `wglMakeCurrent`, `wglSwapBuffers`, `wglGetProcAddress`).
- Forward all unhooked OpenGL calls to the real system `C:\Windows\SysWOW64\opengl32.dll`.
- Intercept viewport setup (`glViewport`, `glScissor`) and matrix uploads (`glUniformMatrix4fv`).
- Manage the double-pass stereo render loop:
  1. Left eye render target setup & matrix injection.
  2. Right eye render target setup & matrix injection.
- Pass rendered texture handles and receive tracking data from the Bridge component.

## Native first-person hand mesh (GH #46)
- `native_hand_mesh.h` + generated `native_hand_mesh_data.h`: the real Riddick
  arm/hand geometry (baked from the shipped `.xmd` models by `tools/xmd_bake.py`)
  as a compile-in, dependency-free asset. Header-only; unit-tested by
  `native_hand_mesh_asset` (`tests/native_hand_mesh_tests.cpp`).
- `VRHandRenderer` still draws the **procedural** overlay by default; the baked
  asset is consumed by the native draw path behind the INI flag
  `[VR] NativeHands` (see ADR-0420/0427). Do not claim the procedural hands are
  the game's real model. The native path is a provisional, rigid, headset-tuning
  draw — the procedural overlay remains the fallback.
- `native_hand_pose.h` maps the asset's model-space AABB into the hand-local
  frame (unit-tested by `native_hand_pose_mapping`,
  `tests/native_hand_pose_tests.cpp`). GH #64 adds `RotateNativeHandPlacement`
  for an INI-tunable hand-local orientation correction
  (`NativeHandYaw/Pitch/Roll`, degrees).
- The arm's long axis is **baked** into the asset (`NativeHandMesh::arm_axis`,
  from the mesh's file geometry) and passed to
  `ComputeNativeHandPlacementAlongAxis`; it is not re-derived from the AABB at
  draw time, which flipped for the posed right arm and mis-placed it. See
  ADR-0444.

## Native hand draw quality (GH #64)
- `DrawNativeHand` renders the baked mesh **two-sided** (backface culling
  disabled + `GL_LIGHT_MODEL_TWO_SIDE`) so no faces vanish and the shipped
  per-vertex normals drive real smooth `GL_LIGHTING` shading. The old hand-rolled
  two-sided abs-lambert (which flattened the silhouette into a card) is gone.
- The hand-local interaction boxes (reload/cartridge/heal/grab glow) are drawn
  only with the **procedural** hand; on the native path they floated as stray
  blocks. Re-anchoring them to the native frame is future work. See ADR-0436.

## Native hand pose source (GH #58)
- `tools/xmd_bake.py` bakes an optional **posed** variant of each skinned
  cluster (`native_hand_mesh_data.h`, `NativeHandMesh::posed_vertices`) from a
  shipped first-person `.xsa` clip. The pose is the ADR-0441 recipe evaluated
  offline by `tools/xmd_fk.py`: forward kinematics over the `NODES` skeleton
  (bind-relative deltas) plus the influence-weighted skin blend. Canonical clip:
  `FPSMelee.xsa::Mel_ShaUnEquip` last key ("hands returned to rest"), pinned by
  `KEYFRAMETIMES` index; re-bake any other clip with
  `--anim/--sequence/--pose-key`. See ADR-0443.
- `DrawNativeHand` selects the posed arrays + posed AABB when
  `[VR] NativeHandPose` is set and a pose exists, otherwise the file (un-posed)
  geometry. The file geometry is **not** the `NODES` bind pose for the arm
  clusters (a raised reference pose, ADR-0424; the `xmd_fk --check` identity is a
  property of the recipe, ADR-0444). The pose is an evidence-backed but
  **unvalidated** VR fit — headset tuning remains.

## Native hand diffuse texture (GH #61)
- `native_hand_texture.h` + generated `native_hand_texture_data.h`: the real
  Riddick skin/scar diffuse (`Riddick_ArmScar_D`) baked from the shipped
  `Content/Models.xtc` by `tools/xtc_bake.py`, stored as the engine's own
  **DXT1/BC1** blocks (no re-encode).
- `bc1.h` decodes those blocks to RGBA8 (`VRHandRenderer::DrawNativeHand`
  uploads a fixed-function `GL_TEXTURE_2D` and modulates it under the existing
  lit colour); unit-tested by `native_hand_texture_asset`
  (`tests/native_hand_texture_tests.cpp`), which cross-checks the C++ decoder
  against reference pixels + checksum produced by the independent Python decoder
  in `tools/mos_xtc.py`.
- Gated by `[VR] NativeHands` (mesh) and `[VR] NativeHandTexture`; the flat
  skin tone is retained as the fallback. Only active when `NativeHands` is set.
  The shipped INI keeps `NativeHandTexture=1`: the bake is **verified skin-toned,
  never white** (whole-surface mean luma ~139, zero near-white texels; the arm
  meshes' own UVs sample varied skin across the atlas). The commit that disabled
  it (52c2101) attributed the washed-out look to a white bake, but the real
  cause was the 0.25 fade translucency fixed in the same commit — the invariant
  is now pinned by `native_hand_skin_mapping`
  (`tests/native_hand_skin_tests.cpp`) and `tools/test_xtc_bake.py`.

## Front-of-HMD block guard (GH #62)
- `combat_guard.h`: header-only, dependency-free `GuardEnvelope` (`GuardHandInWindow`
  / `InGuardEnvelope`) in head-local metres (`+Y` up, `-Z` forward). Block is a
  hand **pose** held in front of the face; a low rest hold never blocks.
- Shared by the guard-pose detector **and** the strike gate in
  `camera_hook.cpp` (`GetGuardEnvelope()`), so a fast forward/hook swing thrown
  from a raised guard is no longer masked by `!inGuardVolume`. Tunable via the
  INI (`GuardYMin/GuardYMax/GuardZMin/GuardZMax/GuardMaxHandSpan`); unit-tested
  by `combat_guard_envelope` (`tests/combat_guard_tests.cpp`). See ADR-0434.

## Swing-driven strike thresholds (GH #65)
- `combat_strike.h`: header-only, dependency-free `StrikeThresholds` +
  `ClassifyStrike` (forward jab / hook / uppercut / dual cross-slash and the
  combined `strike` gate) over per-hand velocity (m/s) and head-local position
  (`+Y` up, `-Z` forward).
- The classifier is shared with `camera_hook.cpp` (`GetStrikeThresholds()`) and
  driven by the INI (`StrikeForwardVz/StrikeForwardMaxZ/StrikeHookVx/StrikeHookVz/
  StrikeUppercutVy/StrikeUppercutMaxY/StrikeUppercutVz/StrikeCrossVx`), so strike
  sensitivity can be tuned on a headset without a rebuild. Raising the hands into
  guard (`Vy>0`, `Vz~0`) matches no strike condition (ADR-0403). Unit-tested by
  `combat_strike_thresholds` (`tests/combat_strike_tests.cpp`). See ADR-0437.

## Engine viewmodel suppression (GH #63 / #66, approach 2)
- Approach 1 (forcing the RPG item's `noviewmodel` bit) was falsified in
  ADR-0438: the flag is an engine output and no shipped module reads it.
- Approach 2 suppresses the engine's own first-person arms/hands **at the draw
  level**. `RndrGL.dll` is the only shipped module that imports OpenGL, and its
  3D mesh path is `glDrawElements` (no `glDrawArrays` anywhere); see
  `docs/rea/reference/RndrGL_GL_Path.txt`.
- `InstallRndrGLHooks` (`dllmain.cpp`) IAT-hooks `OPENGL32.dll!glDrawElements`
  and `OPENGL32.dll!glCallList` and forwards to the saved original (the proxy's
  own forwarder) unless the suppressor says to skip the draw.
- `viewmodel_suppressor.h`: header-only, dependency-free window state machine
  (`ViewmodelSuppressor` + `ViewmodelWindowOpener`). A full colour clear and the
  swap are frame boundaries; a mid-frame **depth-only** clear opens the window
  and the next projection load (or swap) closes it. Unit-tested by
  `viewmodel_suppressor_window` (`tests/viewmodel_suppressor_tests.cpp`).
- **Off by default** (`SuppressEngineViewmodel=0`; `ViewmodelDepthClearWindow`
  selects the opener). The signature is PROVISIONAL and is confirmed via the
  per-frame `Viewmodel suppression [#N]: last frame draws=.. suppressed=..` log
  line on a headset before it is trusted. See ADR-0439.
- **`NearPlaneDrop` now fires: the engine's projection is z-flipped, and the
  near extraction was the gap (ADR-0487 → ADR-0488).** The engine loads its
  projection through `glLoadMatrixf` (never `glFrustum`), and its matrix carries
  `m[11] == +1` with `m[10] = +(f+n)/(f-n)` — the opposite z orientation to the
  classic GL form — so the classic `m[14]/(m[10]-1)` returned **-far** and
  rejected every load (the pass trace read `P?`, `worldNear_` was never set).
  `projection_near.h` (`ExtractProjectionNear`, pure, unit-tested by
  `projection_near`) implements both orientations, keyed on the sign of `m[11]`.
  Measured live (wake 87): the world pass is `P4.0` (`m10=1.00391 m11=+1
  m14=-8.01566` → near 4.0 / far 2048), a second pass is `P1.8`, and the sampled
  near-plane-drop pass is `P0.1`. With `ViewmodelWindowOpener=2` the window now
  opens (measured: 1322 in-window draws at one interval, `viewmodel_pass_identified`
  PASS) — the opener's premise is expressed on the engine, and the previous
  "never opens" reading is retracted. The raw matrix and both extractions are
  logged (`ViewmodelProbe proj [#N]: … classic=.. near=..`) so the convention
  stays auditable; XRSIM grades `projection_near_extracted` (`FAIL` if the
  extraction still rejects the engine's matrix).
- **ADR-0489 — the window's contents are sampled per interval, and the strict
  gate keeps the last one.** `viewmodel_probe_stats` now retains **every**
  `ViewmodelProbe submission fp:` sample (the per-label slot still holds the
  last), because a run samples one in-window draw per 300-swap interval and the
  intervals differ: on the wake-87 level the depth-only window sampled a
  **14688-count volumetric** draw (extent 0.80 × 0.88 × 0.30) in **1 of 18**
  intervals and the near-plane-drop window the same 14688-count mesh in **1 of
  3** — while both runs' *last* interval was the 2520-count / 12-count plane.
  The strict `viewmodel_candidate_is_volumetric` (ADR-0484) therefore still
  FAILs on both (`planar/screen-space`), and a separate graded check
  `viewmodel_volumetric_candidate_sampled` reports the observation
  (`N/M sampled interval(s) had a volumetric in-window draw`). **Because both
  openers sample the same 14688-count mesh, that check is not evidence that the
  window isolates the arms** — the biggest in-window draw is still the wrong
  candidate; only the *sampling* bug is fixed here, not the identification.

- **GH #67 step 2 — observation-only identification.** The same depth-only-clear
  boundary also drives a non-suppressing probe: `ViewmodelObserve=1` opens the
  window and records, per sampled frame, how many draws fell **inside/outside**
  it and the `GL_MODELVIEW` in effect at the **first in-window draw** (the
  first-person pass loads one viewmodel-relative matrix and draws its submeshes),
  logged as `ViewmodelProbe [#N]: inside=.. outside=.. sample=.. pushes=..
  translates=..` plus a `ViewmodelProbe matrix: [..]` line every 300 presented
  frames. It **never** skips a draw (the suppression decision is unchanged and
  stays last). The transform is the input for re-expressing the pass in
  head/controller space (GH #67) and for the duplicate-hands suppression
  (GH #63). XRSIM consumes it (`viewmodel_probe` report key; the
  `viewmodel_pass_identified` check **skips**, not fails, when the probe was not
  run). See ADR-0450.
- **GH #67 step 1 — the composed modelview.** `gl_matrix_tracker.h` mirrors the
  fixed-function `GL_MODELVIEW` stack (`LoadMatrixf`/`LoadIdentity`/
  `Translatef`/`Push`/`Pop`), because RndrGL composes object transforms from
  `glLoadMatrixf` + `glPushMatrix`/`glTranslatef`/`glPopMatrix` and never imports
  `glMultMatrixf`/`glRotatef` (`docs/rea/reference/RndrGL_GL_Path.txt` §3). Without
  it `GetLastViewMatrix()` reported only the last *loaded* base, so a viewmodel pass
  that pushed a sub-transform was captured wrong. `InstallRndrGLHooks` IAT-hooks
  `OPENGL32.dll!glLoadIdentity/glPushMatrix/glPopMatrix/glTranslatef` and forwards
  verbatim (never suppresses); `OpenGLProxy::NoteModelview*` feeds the tracker and
  re-publishes `s_lastView`. Unit-tested by `gl_matrix_tracker`
  (`tests/gl_matrix_tracker_tests.cpp`). See ADR-0451.
- **ADR-0481 — measured: that stack is inert on the live title, and so is the ARB
  program path.** A live run logs exactly one `glLoadMatrixf [MODELVIEW #1]`
  (identity) and zero `glPushMatrix`/`glTranslatef`; the projection loads are
  per-view (only two distinct `(m0, m5)` pairs across 404 samples), and the ARB
  vertex-program entry points the module references
  (`glProgramStringARB`/`glBindProgramARB`/`glProgramEnvParameter4fvARB`/`…4fARB`,
  resolved via `wglGetProcAddress` -- the proxy exports no `Program*` symbol) are
  never called (`ViewmodelProbe program [#N]: strings=0 binds=0 env=0`).
  `vertex_program_observe.h` (+ `Hook_glProgramStringARB`/`Hook_glBindProgramARB`/
  `Hook_glProgramEnvParameter4fvARB`/`Hook_glProgramEnvParameter4fARB` in the
  `wglGetProcAddress` hook) does that measurement; it is inert unless
  `ViewmodelObserve=1`. **Consequence for GH #67:** the object/view transform is
  not in GL state at all -- it is applied engine-side to the submitted vertices --
  so a drive path has to hook the vertex submission (or REA the engine's own
  matrix code), not a GL matrix. Unit-tested by `vertex_program_observe`
  (`tests/vertex_program_tracker_tests.cpp`).
- **ADR-0482 — the submitted-vertex capture.** Since the transform is applied
  engine-side to the geometry, the observable surface is the draw itself:
  `vertex_submission_observe.h` records the client `glVertexPointer` array
  (`InstallRndrGLHooks` IAT-hooks `OPENGL32.dll!glVertexPointer`) and samples, for
  the **largest** `glDrawElements` inside the first-person window (ADR-0483; the
  first in-window draw was measured to be a flat ±0.5 screen quad, ADR-0482) and
  the first outside it, up to 24 vertices (first + axis-aligned bounds) plus the
  draw's index `count`, while counting `glDrawElements` vs `glCallList` by side.
  Logged as `ViewmodelProbe submission [#N]: arrayIn=.. … inSample=..` plus a
  `ViewmodelProbe submission fp:` (in-window, the viewmodel candidate) and
  `ViewmodelProbe submission world:` (the control) geometry line every 300
  presented frames; the live run reads a 2520-vertex, camera-relative `fp` mesh
  (`count=2520`, bbox ±0.45, identical every frame) against a 6-vertex world
  quad (`count=6`). `listIn > 0` with `arrayIn == 0` means the first-person pass
  is display-list geometry and no draw-time sample can see it (the capture would
  have to move into list compilation). Only client `GL_FLOAT` arrays are decoded;
  anything else logs `valid=0` with its type/stride rather than a guessed number.
  Inert unless `ViewmodelObserve=1`; XRSIM parses it into
  `viewmodel_probe.submission`. Unit-tested by `vertex_submission_observe`
  (`tests/vertex_submission_observe_tests.cpp`). See ADR-0482/0483.
- **ADR-0484 — the largest in-window draw is a *plane*, so it is not the arms.**
  The bounds of that same `fp` sample are `min=(-0.450,-0.450,-0.050)
  max=(0.450,0.450,0.000)`: 0.90 × 0.90 × 0.05, symmetric about the origin, i.e.
  `dz/dx = 0.056`. An arm mesh is volumetric; a flat, origin-centred mesh on a
  39.5°-varying head pose is a **screen-space element**, and the run's dumped
  "gameplay" frames are the game's **start screen** (GH #71) — the harness's
  `gameplay` branch is "full-bleed + a recent perspective projection", which the
  full-bleed 3-D start page satisfies. So `count = 2520` identifies a real draw
  but *not* the viewmodel. XRSIM derives the sample's axis extents and grades
  `viewmodel_candidate_is_volumetric` (FAIL for a planar sample); the hook, its
  log line and its default are unchanged.
- **ADR-0490 — the window-independent near-field candidate; the near-field
  hypothesis is falsified on the live level.** ADR-0489's "Next" #1 was a
  discriminator that does not depend on the window, so the observer now also
  keeps the interval's largest decodable array draw whose sampled bounds are
  entirely within `kNearFieldMaxAbs = 16.0` engine units of the origin, chosen
  regardless of the window and logged as `ViewmodelProbe submission near: …
  inside=..` (the trailing flag says whether the window happened to be open at
  that draw); the interval line gains a `nearSample=0|1` marker that identifies a
  #0490+ hook (a pre-#0490 log is SKIPPED, not failed). Measured live (two runs,
  wake 88): the near-field candidate exists only on the title/transition pages
  (intervals `#1200`/`#1500`/`#1800`, best `count=44064`, bounds ±0.45,
  volumetric, **0/3 inside the window**), and the live level (16 consecutive
  intervals, ~2 M array draws) submits **no** draw within ±16 units at all —
  this engine submits the level's geometry in view space (magnitudes ≳ 100), so
  a near-field/object-space test cannot separate the viewmodel from the world.
  XRSIM grades the *claim* (`viewmodel_near_field_candidate_identifies_pass`):
  PASS only when a near-field candidate was found in at least as many intervals
  as the window had in-window draws **and** the best one is volumetric **and**
  its logged bounds are really near-field (the harness mirrors the constant);
  the measured 3/18 and 3/19 runs FAIL. The remaining route to GH #67 step 1 is
  the engine-side identification of the viewmodel pass, not another geometric
  heuristic.
- **ADR-0491 — the census now covers *every* submission mechanism the shipped module
  binds, including the one every previous correction missed: immediate mode.** The
  eight corrections above sampled only `glDrawElements`/`glCallList`, but the live
  2009 `RndrGL.dll` **imports the immediate-mode path** (`glBegin`/`glEnd`/
  `glVertex3f`/`glVertex3fv`/`glVertex2f`/`glVertex2i` among its 96 static
  `OPENGL32.dll` imports; measured read-only this wake with
  `python tools/gl_vocabulary_scan.py --imports`), and the reference explicitly left
  "**do the first-person arms turn out to use immediate mode?**" as an open
  follow-up. A mesh submitted with `glBegin(..) { glVertex3f(..) } glEnd()` emits
  **no** draw call, so it produced no sample in any prior correction — no array or
  list reading could ever have been the arms.
  `immediate_mode_observe.h` (pure; unit-tested by `immediate_mode_observe`) counts,
  per 300-swap interval and split by the first-person window at each `glBegin`:
  blocks (`beganIn/Out`, `endIn/Out`), 3-D vs 2-D vertices (`v3In/Out`, `v2In/Out`;
  2-D is kept separate so "the window is screen-space HUD" is distinguishable) and
  the window's **largest** block with exact bounds (one `glBegin`..`glEnd` batch is
  one submitted mesh — the immediate-mode analogue of ADR-0483). `InstallRndrGLHooks`
  IAT-hooks the six entry points and forwards each verbatim (never suppresses;
  observation-only, off unless `ViewmodelObserve=1`). `OnSwapBuffers` logs
  `ViewmodelProbe immediate [#N]: …` and the candidate in the existing
  `ViewmodelProbe submission fpimm:` form, so XRSIM grades it with the *same*
  planar-vs-volumetric shape predicate as `fp`/`world`/`near`.
  `viewmodel_in_window_immediate_geometry` PASSes when the window held 3-D
  immediate-mode vertices, **FAILs** when the census ran and it held none, and
  SKIPs on a pre-#0491 hook; finding immediate geometry in the window is removing a
  mechanism blind spot, **not** identifying the arms. See ADR-0491.
- **ADR-0492 — engine-side identification: the viewmodel is an item *property*, not
  a GL pass.** With every GL route measured dead (ADR-0484/0487/0490/0491), the
  live (2009) `Content\GameClasses_Win32_x86.dll` was scanned read-only and the
  engine's own viewmodel mechanism is an **item property** `VIEWMODEL` plus an
  attach configuration `ATTACHPOINT`/`ATTACHROTTRACK`, declared next to the
  `CRPG_Object_Inventory` class name (`.rdata` VAs `0x1075a144`/`0x1075a150`/
  `0x1075a15c`). Each name has exactly **one** code xref, all in one neighbourhood
  (`0x10265a92`, `0x10265b7a`, `0x10265b9d`): `FUN_10265ab0` looks up
  `ATTACHROTTRACK`/`ATTACHPOINT` by name and reads a **bool** (vtable slot `0x1b8`)
  into the object at `+0x362`/`+0x35a`; `FUN_10265a70` pushes `VIEWMODEL` into a
  name-keyed call. The 2009 RPG item-flag table (VA `0x1082d728`) reproduces
  ADR-0435's bits with **`noviewmodel` = bit 1**. Consequence: the arms reach GL as
  an ordinary `glDrawElements` of an already-transformed, item-owned model (the
  transform is engine-side, ADR-0481/0482), so **no** GL signature can isolate
  them — the next step is the `VIEWMODEL` consumer / the engine's own FP attach
  state, not another threshold. Recorded (with the provider-timeout limit on the
  9.0 MB module) in `docs/rea/reference/GameClasses2009_ViewmodelProperties.txt`.
  See ADR-0492.
- **ADR-0493 — the 2009 `GameClasses` module *does* decompile.** ADR-0492's "the
  9.0 MB module exceeds the provider's analysis budget" is corrected: 330 s is
  just the **default** Ghidra startup deadline (`REA_GHIDRA_STARTUP_TIMEOUT_MS`);
  raised to 1500 s, two `rea decompile` requests returned **exit 0** (382 s / 380 s
  wall). `FUN_10265a70` decompiles to
  `{ FUN_10260940(); CStr::CStr("VIEWMODEL"); FUN_1026d790(); }`, and the callee
  `0x100162e0` is **`CStr::CStr(const char*)`** — *not* the "name-keyed helper"
  ADR-0492 §3b read from the raw bytes — so the `VIEWMODEL` literal is passed *by
  name* into `FUN_1026d790`. `--snapshot` was measured **not** to be a cross-call
  analysis cache for the CLI Ghidra provider (the second call re-imported and
  re-analysed; `import_mode = "ephemeral-source-immutable"`), so the
  "persist a snapshot" route does not reduce cost. Read-vs-set at the site is
  still open (its callees/callers are not decompiled). Recorded in
  `docs/rea/reference/GameClasses2009_ViewmodelDecompiled.txt`. See ADR-0493.
- **ADR-0494 — the `VIEWMODEL` property is the *collectible* class's model-load
  override, and it is a **read**, not a set.** ADR-0493's two open questions are
  answered read-only on the same 2009 module. `FUN_1026d790` is the named-child
  copy helper (`name = arg->vtable[0x18]()`; `CRegistry::FindChild(registry, name)`;
  the found child's value → destination `vtable[0x68]`), and the item model loader
  `FUN_10260940` uses the *same* helper for `MODEL`/`MODEL0..MODEL3`/`MODEL_DUMMY`/
  `PHYSMODEL`/`ATTACHMODEL`/`SOUND_*`/`ANIM_*`/`ANIMGRAPH2`/`EFFECTLIST`/`PICKUPMSG`
  — so the direction is *definition → object* and the `VIEWMODEL` site is a **read**
  of the item definition. `rea xrefs 0x10265a70` returns exactly one incoming
  reference, the `.rdata` slot `0x10759ed4` (vtable slot **8** of `0x10759eb4`),
  whose `-4` RTTI (`0x107ab9d4` → TypeDescriptor `0x1082d888`) names the class
  **`CRPG_Object_Collectible`** — *not* `CRPG_Object_Inventory` (the name-string
  proximity ADR-0492 relied on is string-pooling order; the `.data` class registry
  shows Inventory as a sibling of Collectible off `CRPG_Object`). Slot 8 is the
  model-load virtual, implemented by `FUN_10260940` in
  `CRPG_Object_Item`/`CreatureAttack`/`Telephone_Receiver` and **overridden** by
  `FUN_10265a70` in `CRPG_Object_Collectible`; **no weapon-class vtable**
  (`Gun`/`Melee`/`Ulaks`/`AngelusChain`) holds either function, so "the player's
  first-person arms are the `VIEWMODEL` property" is **not** supported. Recorded in
  `docs/rea/reference/GameClasses2009_ViewmodelConsumer.txt`. See ADR-0494.

## Display branch / full-bleed vs cinema (GH #43 / #44 / #72)
- The hook publishes one branch bit, `cinemaMode`, from `OnSwapBuffers`:
  `!Has3DThisFrame() || menuCursor`. GH #44 keeps real gameplay full-bleed even
  when the engine bakes letterbox/pillarbox bars (they are cropped from the blit
  using the `BarScan` active rect).
- **GH #72 requirement 1.** A *static 2D* startup page (legal/attribution, ESRB)
  loads a perspective projection, so it was published as `FULL-BLEED gameplay`
  even though `BarScan` measures it at ~0.31-0.36 of the window (gameplay is
  1.00) — the XRSIM dump gate and the per-frame stereo skip then graded the
  frozen splash. `display_branch.h` (pure, unit-tested by
  `display_branch_cold_start_latch`) adds a *cold-start* latch: while
  `DisplayBranchColdStartLatch=1`, a fill `<= 0.75` is cinema until the first
  full-bleed fill `>= 0.90` is ever measured; after that the demotion can never
  fire again, so GH #44 is unaffected. **On by default since the 2026-10-18 live
  A/B (ADR-0478):** a live XRSIM run demoted the letterboxed startup page
  (`fill=0.40`) to cinema and still reached full-bleed gameplay
  (`fill=1.00 fullBleed=1`); no full-bleed page appeared before gameplay. Set
  `DisplayBranchColdStartLatch=0` for the legacy bit. The `DisplayMode` line now
  carries `latch=.. fill=.. fullBleed=..`, and the latch logs its own demotion
  once (`cold-start latch demoted ...`). See ADR-0459 / ADR-0478.



