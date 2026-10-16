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



