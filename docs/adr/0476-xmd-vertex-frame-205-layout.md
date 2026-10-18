# ADR-0476: Decode the 2009 `VERTEXFRAMES` version `0x205` (positions + normal + two extra unit-vector arrays)

- Status: Accepted (scheduled autoloop wake 75, GH #85)
- Date: 2026-10-18
- Related: GH #85, GH #84 / ADR-0474 (skeleton `NODES` selection), ADR-0415
  (2004 `VERTEXFRAMES` 0x204 / `TVERTEXFRAMES` geometry), GH #55 (native hand
  mesh decode), `tools/mos_xmd.py`

## Context

ADR-0474 made the corpus walk *decidable* and surfaced that the 2009 *Assault on
Dark Athena* install (`F:`) decoded **zero** `VERTEXFRAMES` clusters: every one of
its 4566 reported problems was

```
VERTEXFRAMES version 0x<...> not decodable (only 0x202-0x204)
```

because every 2009 frame starts with version word **`0x205`** (8645/8805 nodes;
the remaining 160 are empty) where the 2004 install uses a leading `u32`
`frame_count == 0x1` followed by version **`0x204`** (4662/4662). GH #85 asked to
recover the `0x205` layout from the 2009 loader and re-grade `--check` on `F:`.

The 2009 engine ships the same reader module (`System/Win32_x86/MXR.dll`) as the
2004 build, so the field *names* carry over from ADR-0415; only the per-vertex
record changed. Rather than decompile the 2009 `MXR.dll` (allowed, but slow), the
layout was recovered **from the shipped assets themselves**, which is sufficient
because the geometry is self-validating (a wrong decode produces incoherent
meshes — see the decisive guards below).

## Decision

Add a `0x205` branch to `decode_vertex_frame` (`tools/mos_xmd.py`) with the
following layout, which shares the 2004 52-byte frame header byte-for-byte:

```
u32  version          = 0x205          (+0)
u32  header20                          (+4)
f32  bounds_min[3]                     (+8)     (== the cluster/MISCELLANEOUS box)
f32  bounds_max[3]                     (+20)
f32  aux[3]                            (+32)
u32  header30                          (+44)
u32  vertex_count  n                   (+48)
u16  positions[n][3]   (6n bytes)      (+52)    dequantized min + q*(max-min)/65535
s8   normals[n][3]     (3n bytes)      (+52+6n) re-normalized s8/127
s8   tangent[n][3]     (3n bytes)      (+52+9n) re-normalized s8/127
s8   bitangent[n][3]   (3n bytes)      (+52+12n) re-normalized s8/127
```

so a frame is exactly `52 + 15n` bytes. Unlike the 2004 payload there is **no
leading `frame_count` word**: the first `u32` *is* the version. `Xmd.vertex_frames`
already distinguishes the two by "skip a leading word only if it is not itself a
known frame version", so adding `0x205` to `VERTEX_FRAME_VERSIONS` is the entire
framing change. The two extra arrays are exposed as `VertexFrame.extra_basis`
(empty for `0x204`).

## Evidence (read-only; installs untouched)

### Size law

Over the first eight `0x205` clusters of `Characters/Abbot_rcap/abbot_rcap.xmd`
and `Characters/Riddick/Riddick_B03.xmd`, `data_size == 52 + 15·n` holds **exactly**
for `n =` the triangle-index maximum + 1 (e.g. `14062 = 52 + 15·934`,
`292 = 52 + 15·16`, `1987 = 52 + 15·129`, `772 = 52 + 15·48`). Solving two nodes
(`14062, n=934` and `292, n=16`) gives the unique `(header, stride) = (52, 15)`,
confirmed by the other six.

### Positions are the first `6n` bytes (decisive)

For a decode to be correct, triangle edges must join nearby vertices. Using the
median triangle-edge length over the cluster's bounding-box diagonal as the
metric, the correct layout scores like the known-good 2004 loader output, and
every wrong placement scores ~10× worse:

| decode | `abbot` vf[0] | `Riddick_B03` vf[0] |
|---|---|---|
| 2004 ground truth (loader, 0x204) | 0.045 | 0.045 |
| 0x205 positions as `u16[3]` @ stride 6 (this ADR) | **0.037** | **0.021** |
| 0x205 positions as `u16[3]` @ stride 15, any offset 0-11 | 0.37-0.40 | 0.29-0.34 |
| 0x205 positions as `u8`/`i8[3]` @ stride 15 | 0.35-0.40 | — |
| 0x205 positions as `u16[3]` @ stride 9 | 0.34 | 0.29 |

### The tail `9n` bytes are three `s8` unit-vector arrays (decisive)

After the positions, `9n` bytes remain. Reading them as `s8[3]` triplets gives
**100.0 %** unit-magnitude vectors (934/934 and 1236/1236) at stride 3 — i.e.
three per-vertex vector arrays. Correlating each array with the *geometric* face
normal (positions + `TRIANGLES`) identifies the first as the shading normal and
the other two as orthogonal:

| array | mean &#124;dot&#124; with face normal (`abbot` / `Riddick`) |
|---|---|
| block 0 (`+52+6n`) | **0.951 / 0.833** |
| block 1 (`+52+9n`) | 0.476 / 0.483 |
| block 2 (`+52+12n`) | 0.478 / 0.501 |
| interleaved `s8[3]` per vertex (rejected) | 0.487-0.488 / 0.502-0.510 |

The two ~0.49 blocks are unit vectors orthogonal to the normal → tangent and
bitangent (the exact handedness is not needed for decode and is not claimed).

## Consequences

- `xmd_pose --check` on `F:` (2009) went from **4566 problems / 77 clusters /
  59 skinned** to **41 problems / 4602 clusters / 4384 skinned**; the 4525 fixed
  problems were all the `VERTEXFRAMES` kind. `xmd_fk --check` on `F:` reports the
  same 41.
- The 2009 geometry is now decoded (positions, normals, tangent/bitangent), which
  is the prerequisite for the native-hand mesh path (GH #55) on the *target*
  install.
- `C:` (2004) is unchanged and stays green: `xmd_pose`/`xmd_fk --check` = 268
  models / 3454 clusters / **0** problems (identical to ADR-0474).
- The remaining `F:` red is a **new, distinct** format gap: 41 clusters in 19
  files (mostly `Weapons/*`, `Misc/*`) whose `TRIANGLES` payload is *not*
  `u32 count` + `u16[3]` — e.g. `Misc/Cigarette.xmd` cluster 3 has
  `size 188` and first bytes `00 00 05 00 07 00 00 00 06 00 05 00` (a plausible
  index stream with no leading count), where the working clusters of the same
  file begin `40 00 00 00 00 00 01 00 …` (`u32 count = 64`). Filed as **GH #86**;
  it was previously hidden behind the `VERTEXFRAMES` abort and is **not** caused
  by this change.

## Validation

- `python tools/test_mos_xmd.py` — new `test_geometry_roundtrip_205` (a synthetic
  `0x205` frame decodes `52+15n`, positions dequantize, normals re-normalize,
  two `extra_basis` arrays of unit vectors), `test_geometry_204_has_no_extra_basis`
  (no regression), and `test_geometry_205_truncated_basis_is_rejected` (a frame
  that omits a basis array no longer tiles the chunk and is reported). Full
  `tools/test_*.py` PASS.
- Read-only corpus (no install write): 2004 `C:` → 0 problems; 2009 `F:` →
  41 problems, all `TRIANGLES` (GH #86).
- No C++ changed, so no `build-all.ps1`/CTest (documented).
