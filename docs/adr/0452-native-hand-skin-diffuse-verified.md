# ADR-0452: The native-hand skin diffuse is verified skin-toned (refuting the "decodes white" premise)

- Status: accepted
- Date: 2026-10-16
- Issues: GH #61 (native-hand texture), GH #64 (flat/2D native hands)
- Relates: ADR-0431/0432 (`.xtc` container + S3TC codec), ADR-0433 (bind the real
  diffuse), ADR-0420/0427 (`Riddick_B05` arm surfaces)

## Context

GH #61 landed the real `Riddick_ArmScar_D` skin/scar diffuse on the native hand
mesh: `tools/xtc_bake.py` bakes the shipped **DXT1** blocks verbatim into
`src/hook/native_hand_texture_data.h`, `src/hook/bc1.h` decodes them, and
`VRHandRenderer::DrawNativeHand` uploads one `GL_TEXTURE_2D` and modulates it
under the lit colour.

An interactive session (commit `52c2101`, "native hands opaque + drop broken
white texture bake") then set `[VR] NativeHandTexture=0`, stating that *"the
baked NativeHandTexture decoded white"*, and kept the flat skin-tone fallback.
The same commit's first (correct) finding was that the native hands inherited the
overlay's **0.25 cull-away fade** — i.e. they were *translucent* and washed out
against the bright scene. It forced `f = 1.0f` (opaque) and, separately,
disabled the texture on the "white bake" claim.

A standing-prompt wake (wake 45 → 46) was asked to "fix or keep the skin-tone
fallback" and, before touching anything, to check the claim. It is false.

## Decision

Treat the "baked diffuse decodes white" premise as **refuted**, keep the real
diffuse **enabled in the shipped config** (`riddick_vr.ini`
`NativeHandTexture=1`, matching the compiled default and
`tools/riddick_vr.ini.example`), and pin the refutation with a regression test so
the feature is not reverted on the same false premise.

The earlier washed-out/white look is attributed to the **translucency** that the
same commit fixed (a 0.25-alpha overlay blended over a bright frame), not to the
asset.

## Evidence (reproducible, read-only, offline)

`tools/xtc_bake.py` decodes the shipped `Content/Models.xtc`
(`Riddick_ArmScar_D`, mip 1, 256×512, DXT1) with the independent Python decoder
in `tools/mos_xtc.py`:

- **Whole surface:** mean RGB ≈ `(164.0, 130.6, 114.7)` → mean luma ≈ **139**;
  **0 of 131072** texels are near-white (all channels > 200). A 685 MB pack
  mip that "decodes white" cannot produce zero near-white texels with a warm
  mid-tone mean.
- **The mesh's own UVs** (baked in `native_hand_mesh_data.h`) span nearly the
  whole atlas — `Riddick_LeftArm` `u[0.004,0.994] v[0.006,0.995]`,
  `Riddick_RightArm` `u[0.006,0.994] v[0.007,0.998]` — i.e. non-degenerate.
  Sampling the decoded surface at every vertex (nearest, both V-flip variants):
  mean luma **127–147**, per-vertex stddev **42–55**, **0 near-white** samples.
  Example texels: `(202,165,144)`, `(180,145,128)`, `(207,169,150)`.
- The baked asset is unchanged since it was first committed (`280e058`,
  checksum `0x05308AE2`), so the asset the session saw is exactly the asset
  probed above.

## Consequences

- The renderer already binds `Riddick_ArmScar_D` with `GL_MODULATE` and emits
  the mesh's `uv` per vertex, so enabling the flag is the whole feature; the
  flat skin tone stays the fallback when the flag is off or the GL entry points
  are unavailable.
- New CTest `native_hand_skin_mapping` (`tests/native_hand_skin_tests.cpp`)
  decodes the in-tree asset with `bc1.h` and asserts: whole-surface mean luma in
  a skin band, near-white fraction < 2%, real tonal spread, arm UVs span the
  atlas, and the UV-sampled luminance is skin-toned (no near-white) with
  stddev > 15 — for both V-flip variants.
- `tools/test_xtc_bake.py` mirrors the same invariant on the committed headers
  (CI lane, no game install); `tools/verify_reported_fixes.py` asserts the
  shipped INI keeps `NativeHandTexture=1`.
- **Not headset-validated.** `NativeHandTextureVFlip` remains provisional. One
  in-headset confirm (texture on the arm, no UV seam) closes #61; the offline
  probe already removes the "white bake" as a reason to keep it off.

## Alternatives considered

- **Keep the flat skin tone** (the status quo) — rejected: its only justification
  was a refuted premise, and it forfeits the #61 deliverable.
- **Re-bake the texture / pick a different mip or surface** — rejected: the
  current bake is provably skin-toned and the arm UVs map cleanly to it; there
  is nothing to fix in the bake.
