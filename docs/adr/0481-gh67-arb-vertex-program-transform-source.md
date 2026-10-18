# ADR-0481 — GH #67: the engine's transforms are ARB vertex-program parameters, not a GL_MODELVIEW

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (this — step 1, corrected), GH #63/#66 (the same pass
  identification), GH #68 (iteration vehicle), ADR-0450 (the observation probe),
  ADR-0451 (the modelview mirror), ADR-0413/0414 (the 2004-vs-2009 target
  mismatch), ADR-0471/0480 (live-capable, no-deploy shadow runs)

## Context

GH #67's plan ("hook the FP viewmodel draw + matrix and re-project into
head/controller space") assumed the engine composes its object/view transforms
in the fixed-function `GL_MODELVIEW` stack. That assumption came from
`docs/rea/reference/RndrGL_GL_Path.txt` section 3, which was produced from the
**2004** install (`C:\Program Files (x86)\Starbreeze Studios\Riddick EFBB`) and
found `glLoadMatrixf` + `glPushMatrix`/`glTranslatef`/`glPopMatrix` and **no**
`glMultMatrixf`/`glRotatef`. Wake 43/44 built `gl_matrix_tracker.h` (a software
mirror of that stack) and wake 79 ran the probe live for the first time: the
sampled matrix was the **identity** on every frame with `pushes=0
translates=0`, and wake 79's "next step" was to intercept the calls it had not
yet hooked (`glMultMatrixf`, `glLoadMatrixd`) on the theory that the engine
composed the modelview with them.

That theory was testable against the shipped binary, and it is false.

## Evidence (read-only, no install write)

1. **Vocabulary of the live target's renderer.** `tools/gl_vocabulary_scan.py
   --game-dir "F:\The Chronicles of Riddick - Assault on Dark Athena"` (939248
   bytes, 05-Mar-12) reports the 2009 `RndrGL.dll` — the only module that
   imports OpenGL — references:
   - `fixed_function_matrix` present: `glLoadMatrixf`, `glTranslatef`,
     `glPushMatrix`, `glPopMatrix`, `glLoadIdentity`, `glMatrixMode`,
     `glFrustum`, `glOrtho`; **absent: `glMultMatrixf`, `glMultMatrixd`,
     `glLoadMatrixd`, `glRotatef`, `glRotated`, `glScalef`, `glDepthRange`**.
     So the calls wake 79 proposed to intercept are not imported at all, and the
     fixed-function modelview cannot be composed with them.
   - `program` present: `glProgramStringARB`, `glBindProgramARB`,
     `glGenProgramsARB`, `glDeleteProgramsARB`, `glProgramEnvParameter4fvARB`,
     `glProgramEnvParameter4fARB`, `glGetProgramivARB`, `glGetProgramStringARB`
     (ARB_vertex_program; no `glProgramLocalParameter4fvARB`, no
     `glLoadProgramNV`/`glTrackMatrixNV`).
   - `attributes` present: `glVertexAttribPointer`/`…ARB`,
     `glEnableVertexAttribArrayARB`, `glVertexAttrib4fARB`,
     `glVertexPointer`/`glNormalPointer`/`glColorPointer`,
     `glEnableClientState`/`glDisableClientState`; **absent:
     `glTexCoordPointer`** — i.e. client arrays plus *generic* attributes, the
     shape of an ARB-vertex-program renderer.
   - **The static import table (the authoritative list).** `python
     tools/gl_vocabulary_scan.py --game-dir "<install>" --imports` parses the
     PE import directory: `RndrGL.dll` binds exactly **96** OpenGL 1.1 entry
     points from `OPENGL32.dll` — the whole matrix API it has is
     `glLoadMatrixf`, `glTranslatef`, `glPushMatrix`, `glPopMatrix`,
     `glLoadIdentity`, `glMatrixMode`, `glFrustum`, `glOrtho` (no
     `glMultMatrixf*`/`glLoadMatrixd`/`glRotatef`/`glScalef`, exactly as the byte
     scan says) — plus a heavy **legacy immediate-mode** vocabulary
     (`glBegin`/`glEnd`, `glVertex3f`/`glVertex3fv`/`glVertex2f`, `glNormal3fv`,
     `glTexCoord2f`/`2fv`/`3f`/`3fv`/`4fv`, `glColor3f`/`4f`/`4ub`,
     `glMaterialfv`, `glLight*`, `glFog*`, `glClipPlane`, `glTexGen*`,
     `glDrawPixels`, `glNewList`/`glEndList`/`glCallList`) and **no** ARB entry
     point at all — so every ARB name in the byte scan is a
     `wglGetProcAddress` literal, and the proxy's `wglGetProcAddress` hook is the
     only place they can be intercepted.
2. **The live log agrees.** The wake-79 run's hook log
   (`%TEMP%\riddickvr-shadow-4616\System\Win32_x86\riddick_vr.log`, 8790 frames,
   `RIDDICKVR_SIM=1`, `--ini ViewmodelObserve=1`) contains exactly **one**
   `glLoadMatrixf [MODELVIEW #1]: Pos(0.00, 0.00, 0.00)` for the whole run, 405
   `glLoadMatrixf [3D VR PROJECTION #…]` lines, and **zero**
   `glFrustum called` lines; the probe reports `pushes=0 translates=0` on every
   sampled frame. All six RndrGL IAT hooks (including `glPushMatrix`/
   `glTranslatef`) log `Successfully IAT-hooked … in module 07BE0000`, so the
   zero is a real zero, not a failed hook.
3. **The proxy exports no `Program*` symbol** (`src/hook/opengl32.def`), so the
   engine cannot statically import the ARB entry points from `OPENGL32.dll`; it
   resolves them through `wglGetProcAddress`, which `RndrGL` does import and
   which is served by `OpenGLProxy::Hook_wglGetProcAddress`. That is the
   interception point.

4. **The ARB path is instrumented, and it is silent too.** With the ARB hook of
   this ADR in place, **three** live runs (45 s each, per-run read-only shadow,
   `--require-fresh-hook`, no deploy) all log the same thing. The last one runs
   the final build (`dist\opengl32.dll` sha256 `c71a36e9cf48…`, byte-identical in
   the repo and the shadow): 30 gameplay captures, XRSIM `PASS`, 17 step-2 probe
   lines (matrix = identity) and 24 sampled
   `ViewmodelProbe program [#N]: strings=0 binds=0 env=0 lastProg=0 strLen=0
   strHash=00000000 matrix=0 base=0` lines — and **both** spellings of the env
   setter (`glProgramEnvParameter4fvARB` *and* `glProgramEnvParameter4fARB`, the
   two the module references) are intercepted, so the zero is a statement about
   the engine, not about one name. The run's report carries the measurement
   (`viewmodel_probe.program = {"strings": 0, "binds": 0, "env_params": 0,
   "matrix": null}`). Every run also logs exactly one
   `glLoadMatrixf [MODELVIEW #1]: Pos(0.00, 0.00, 0.00)`.
5. **Neither GL matrix can be the object transform.** The projection is reloaded
   ~14x per frame (121k loads / 8790 frames), but across 404 sampled loads the
   live log shows only **two** distinct `(m0, m5)` pairs (0.63/-1.33 and
   0.58/-1.22) — values that are plain focal lengths. A per-object `P*V*M`
   upload would vary those elements with the object's rotation; a per-view/pass
   upload does not. So the projection carries the view, not the objects, and with
   an identity modelview and immediate-mode + client-array draws the object
   transform must be in the *submitted vertex data* (the engine's own CPU-side
   transform), not in any GL state the hook can observe.

**Conclusion.** The transform that GH #67 must re-express is not in the GL
matrix stack (falsified: the engine never composes one), and it is not in the
ARB vertex-program parameter path either (measured silent). The engine renders
fixed-function geometry with an identity modelview, so the per-object/world
transform is applied to the vertex data on the engine side before submission.
The fix direction changes accordingly: capture the transform where the engine
builds its vertices (or REA the engine's own matrix/skinning code — the
`CreateMatrixIW`/`TransformBones` family already mapped in wakes 20/21), not at
a GL entry point.

## Decision

Instrument the ARB vertex-program path — the last remaining GL-state candidate —
so the question "where is the transform?" is answered by measurement rather than
by the next guess. Keep the fixed-function mirror as a (now provably inert)
unit-tested component rather than deleting it.

- `src/hook/vertex_program_observe.h` — a header-only, dependency-free
  `VertexProgramObserver` fed with `glProgramStringARB` (count + length + FNV-1a
  string identity), `glBindProgramARB` (count + last bound id) and the env
  setter (count + a 4x4 assembled from four *consecutive* indices). It is inert
  until `Configure(true)`, so observation can never change a run that did not ask
  for it.
- `OpenGLProxy::Hook_wglGetProcAddress` returns the new hook stubs for
  `glProgramStringARB` / `glBindProgramARB` / `glProgramEnvParameter4fvARB` /
  `glProgramEnvParameter4fARB` (saving the real pointer the engine resolved), so
  the engine's own calls land in the observer. **Both** spellings of the env
  setter are covered because the module references both; each hook forwards
  verbatim and nothing suppresses or delays a call.
- The observer is switched by the same `ViewmodelObserve=1` key (configured in
  `Hook_glClear` next to the suppressor) and reported on the same sampled frames
  as the step-2 probe:
  `ViewmodelProbe program [#N]: strings=… binds=… env=… lastProg=… strLen=…
  strHash=… matrix=… base=…` plus the assembled matrix. `tools/xrsim.py` parses
  it (`viewmodel_probe_stats` → `program_lines`/`program`) so a run's report
  carries the measurement.
- `docs/rea/reference/RndrGL_GL_Path.txt` section 3 gains the corrected 2009
  measurement; `tools/gl_vocabulary_scan.py` (+ test) makes both halves (byte
  scan and static import table) reproducible.

## Consequences

- GH #67's capture target is now known to be **outside GL state**: the drive path
  cannot be "read the modelview at the FP draw" (the modelview is identity) nor
  "read the program env parameter the FP pass uses" (none is called in gameplay).
  The transform has to be captured where the engine builds it — in its own
  CPU-side matrix/skinning code (`CreateMatrixIW`/`TransformBones`, wakes 20/21)
  or at the vertex submission (`glVertexPointer`/`glVertex3fv`/`glVertex*`).
  That is a change of approach, not a tweak.
- The step-2 probe stays useful as *pass* identification (the depth-only-clear
  window still counts draws, and the draw path is visible); only its matrix half
  is known-dead on this engine.
- The same read-only scans run against any install, so "2004 and 2009 share this
  vocabulary" is checkable rather than remembered.
- Cost: four more `wglGetProcAddress`-served hooks on the render thread, each a
  counter and (for the env parameter) a 16-float copy; no GL state is touched.

## Honest limits

- "Silent" is measured through the proxy's `wglGetProcAddress`: it proves no ARB
  program call reached the engine's GL dispatch that way. A module that bypassed
  the proxy entirely could in principle differ — but the same run's
  `glLoadMatrixf`/`glClear`/`glDrawElements` interception (406 + draws per frame)
  shows `RndrGL` binds to the proxy, so this is the engine's dispatch.
- Whether the engine *requested* the ARB pointers at all is not logged; a
  request log would distinguish "vestigial names" from "resolved but unused".
  Either way the calls are not made, which is the property that matters here.
- The 16-float assembly records four consecutive env vectors **in index order**
  and claims no row/column convention. It is currently unused (no env params were
  ever seen), so it is exercised only by its unit tests.
- The byte scan is a substring match; `cstr` (name + NUL) is the authoritative
  count and `--imports` is authoritative for static imports. `VOCABULARY` avoids
  names that are prefixes of one another for this reason.
- `glGenProgramsARB`/`glDeleteProgramsARB`/`glGetProgramStringARB` are not
  intercepted; a program is identified only by length + FNV-1a hash of its
  string (capped at 4096 bytes), not by its GL id.
- The fixed-function mirror is kept, not deleted: it is the correct semantics if
  an engine ever does use the stack, it is unit-tested, and it is cheap. On the
  live 2009 target it is inert — which is itself the finding.
- Nothing player-visible changes in this unit (no rendering path is altered); the
  value is that the next #67 step now targets the right data.

## Validation

- Offline: `tools/test_gl_vocabulary_scan.py` (pure `scan_names`,
  `group_presence`, the synthetic-PE import parse, CLI + error paths);
  `tools/test_xrsim.py` +1 (`viewmodel_probe_program_lines_are_parsed`, incl. the
  additive/legacy case); CTest `vertex_program_observe`
  (`tests/vertex_program_tracker_tests.cpp`, 9 cases); the GH #67 source
  invariant in `tools/verify_reported_fixes.py` now requires the ARB path.
  `build-all.ps1` Release (no deploy), CTest 23/23 x64 + 25/25 x86, all 12
  `tools/test_*.py`, `scenario_tests --check`, `xrsim --selftest`,
  `verify_test_artifacts`, `verify_hook_init`, `validate_addresses`,
  `verify_reported_fixes` PASS.
- Live: three runs recorded in `tools/autoloop/STATE.md`
  (`--shadow-install … --ini ViewmodelObserve=1 --require-fresh-hook`), the last
  on the final build (`dist\opengl32.dll` sha256 `c71a36e9cf48…`,
  `deployed_hook_matches_build` PASS): XRSIM `PASS`, 30 gameplay captures, and
  on its hook log `viewmodel_probe_stats` = 17 step-2 lines (matrix = identity) +
  24 program lines (`strings=0 binds=0 env=0`), surfaced in the report as
  `viewmodel_probe.program`.

