# ADR 0445: make the native hand texture sample, show cues as a tint, stop fading while boxing

## Status
**Accepted, unverified on a headset.** Code-only change to the hand renderer. It
needs an in-headset check for each of the three issues below (GH #61, #64, #46).

## Context
Three reports about how the VR hands look.

1. **GH #61, white hand.** The baked DXT1 diffuse is skin-coloured at every arm
   UV. A unit test over both arm meshes (with and without a V flip) finds zero
   near-white texels and a mean red of 153-174. With the texture bound the vertex
   tint is white, so a white hand means the texture does not sample at runtime.
   The renderer shares the game's GL context, and the engine can leave state that
   changes sampling: other texture units or targets enabled, a non-identity
   texture matrix, odd pixel-unpack state or a bound unpack buffer, an ARB
   program, or it can reuse or overwrite the texture id we generated.
2. **GH #64, stray blocks.** The native mesh is placed from its own bounds. The
   IK upper-arm tube and the hand-local cue boxes (receiver glow, cartridge, heal,
   grab) are authored for the procedural hand, so on the native mesh they float.
3. **GH #46, faded while boxing.** `RenderHands` fades the hands to 0.25 when both
   grips are above 0.70 or the trigger is above 0.60. A boxer guard closes both
   grips, so the hands faded exactly while the player was boxing.

## Decision
- `RenderHands` disables every other texture unit and target (1D, 2D, 3D, cube
  map, rectangle), texture coordinate generation and ARB programs, and pushes an
  identity texture matrix for the pass. `glPushAttrib` with `GL_ENABLE_BIT` and
  `GL_TEXTURE_BIT` restores the state, and the active unit is restored by hand.
- The texture upload forces known unpack state (`glPushClientAttrib`), unbinds any
  pixel-unpack buffer, and sets base and max mip level to 0. GL errors are drained
  before and logged after.
- Once per host frame the texture is checked: `glIsTexture`, level 0 size and min
  filter feed `CheckNativeTextureIntegrity` (pure, unit-tested). A missing or
  resized object is re-uploaded into a **fresh** id; the old id is never deleted
  because the engine may own it now. A changed min filter only gets its sampling
  parameters set again.
- The first draw logs one line, `NativeHand GL state: ...`, before any state
  change. If the hand still reads white, that line is the evidence to ask for.
- `NativeHandTexture` is on in `riddick_vr.ini`.
- With native hands the IK upper-arm tube is skipped unless `NativeHandUpperArm=1`.
  The cue boxes stay procedural-only; the native hand shows the same cues as a
  skin tint (`ComputeNativeHandCueTint`, `hand_visuals.h`).
- `ComputeHandFade` returns 1.0 in a boxer guard (both grips above 0.70, no trigger
  above 0.60, both hands inside the shared guard envelope) and when
  `HandFadeWhenWeapon=0`. Native hands were already drawn opaque; the fade affects
  the procedural hand and arm tube.

## Consequences
- If the white hand was caused by engine GL state or a clobbered id, it is fixed.
  If it was caused by something else (for example a shader the engine keeps bound
  outside the ARB path), the `NativeHand GL state:` line shows it.
- The per-frame check adds a few GL queries and one texture bind per host frame.
- Unit tests cover the texture integrity decision, UV coverage of the real
  texture, the cue tint and the fade. The GL calls themselves cannot be tested
  headlessly and need a Windows build and a headset.
