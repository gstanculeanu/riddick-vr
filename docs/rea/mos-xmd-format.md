# Starbreeze `MOS DATAFILE2.0` (`.xmd`) format — REA notes

Read-only reverse-engineering of the Riddick model container, so the real
character/hand meshes and skeletons can be reused for VR (GH #55, ADR-0413).
Nothing here writes to the game install.

Tooling: **REA 6.5.0 + Ghidra 12.1.4** (`GHIDRA_INSTALL_DIR` set). Full
Ghidra pseudocode excerpts live in `docs/rea/reference/`.

## Artifacts analysed
| Module | Build | SHA-256 | Role |
|---|---|---|---|
| `MCCDyn.dll` | AoDA (F:) | `fa4c995cb066626e1fb3d85adfbc965efea4a92e724a72c6b7ae135a1de58aee` | generic `CDataFile` container read/write |
| `MXR.dll` | EFBB (C:) | `06424f55f5a0f85eb09b5fbd69491c7f6a88de808c26c513e8dac180f3c2052d` | model/skeleton/mesh (`CTriangleMeshCore`) |
| `MXR.dll` | AoDA (F:) | 4,596,976 B | same chunk names present |

Only `MCCDyn.dll` contains the literal `MOS DATAFILE2.0` (`0x100e808c`) and
`MOS DATAFILE1.0` (`0x100e80b4`). Only `MXR.dll` contains the geometry chunk
names `VERTEXFRAMES`, `TVERTEXFRAMES`, `BONEDEFORM`, `SKELETON`, `NODEINDICES`.

## Container (`CDataFile`)
- Writer: `CDataFile::Create` — AoDA `MCCDyn.dll` xref `0x10082a74`.
- Reader: `CDataFile::ReadTree` — AoDA `MCCDyn.dll` xref `0x10082f15`
  (`?ReadTree@CDataFile@@AAEXPAVCCFile@@@Z`). Pseudocode:
  `docs/rea/reference/MCCDyn_CDataFile_ReadTree.txt`.
- Reads a **0x30-byte header**, matches `MOS DATAFILE2.0` (v2) / `1.0` (v1),
  seeks back to the node-table start, and reads **N × 0x30-byte node records**
  (N from header+0x28, or counted recursively when 0).
- Header: `+0x18` node-table start (`0x30`); `+0x20` data-region start;
  `+0x28` u32 declared node count (0 ⇒ count recursively).
- Node record (0x30), validated by `tools/mos_xmd.py`:
  | off | meaning |
  |---|---|
  | `+0x00` | name[16], NUL-padded |
  | `+0x10` | u32[2] flags/reserved (usually 0) |
  | `+0x18` | u32 node-link A (offset) |
  | `+0x1C` | u32 node-link B (offset) |
  | `+0x20` | u32 data offset (absolute) |
  | `+0x24` | u32 data size |
  | `+0x28` | u32 element count / scalar |
  | `+0x2C` | u32 flags (e.g. `TVERTEXFRAMES` = `0x201`) |
- `ReadTree` converts A and B to node indices via `(value - nodeTableStart)/0x30`
  and links them. One is the first child, the other the next sibling; the
  decompiler names vs. the parser labels differ, but `tools/mos_xmd.py`
  (`+0x18`=next_sibling, `+0x1C`=first_child) reproduces the correct
  per-cluster nesting. **Exact role confirmation is a small pending task.**
- Chunk payloads tile the data region contiguously — verified for every
  `TRIANGLES` (declared count == node `count`).
- `CDataFile::GetNext(df, "NAME")` selects a sibling chunk by name; that is how
  the mesh reader finds `VERTEXFRAMES`/`TVERTEXFRAMES`/`BONEDEFORM`.

## Geometry (`CTriangleMeshCore`) — in `MXR.dll`
- Reader entry: `CTriangleMeshCore::Read` ≈ `FUN_10080f50` (error strings
  `"Corrupt model. (1|2)"`), referenced from `0x10081222`. Pseudocode:
  `docs/rea/reference/MXR_CTriangleMeshCore_Read.txt`.
- It calls, in order:
  - `GetNext("VERTEXFRAMES")` → `FUN_1007fba0` (sizes the vertex-frame list)
  - `GetNext("TVERTEXFRAMES")` → `FUN_1007fce0`
  - `GetNext("BONEDEFORM")` → `FUN_1007d260`; the chosen data offset is stored
    at mesh `+0x94`.
- Chunk-name xrefs: `BONEDEFORM`@`0x10149c88` → `0x100817f6` (reader) /
  `0x1007daaf` (writer); `VERTEXFRAMES`@`0x10149ca4` → `0x10081222`;
  `TVERTEXFRAMES`@`0x10149c94` → `0x10081437`.
- `FUN_1007d260` (`MXR_ReadBoneDeform.txt`) — BONEDEFORM layout (observed):
  1. seek to mesh+0x94; read an int (`FUN_10006620`) = **bone-record count**;
  2. resize the list at mesh+0x14, then `CCFile::Read` a raw array of
     `count × stride` bytes;
  3. read a second int = **bone-index-list count**; resize mesh+0x1c;
  4. `FUN_1007c150(file, 0x200, …)` reads that secondary array;
  5. per primary record: `u16 index = *(u16*)(rec+2)`, and byte `rec+1` = number
     of bone influences; the secondary list holds 8-byte entries whose first
     `u16` is a 1-based bone index.
  → i.e. per-vertex skin influences. Exact stride/packing to be finalised.
   **Decoded (ADR-0417/ADR-0423):** the completed layout is
   `u32 n; n*{u8 flags; u8 count; u16 start}; u32 m; m*{u8 bone; u8 wq};
   s32 matrix_map_id; u32 b; b*u8 matrix_bones`, with `influence_start` a
   running prefix sum and `weight = wq/255`. The `bone` byte is a **palette
   index into `matrix_bones`** when that palette is non-empty, and a direct
   skeleton node index only when it is empty (ADR-0423).
   **Field identity (ADR-0426):** this trailing block is the engine's
   **`BONEMATRIXMAP`** — `CTriangleMeshCore::Read` stores it at mesh `+0x24`
   and its signed id at `+0x28`; the legacy path (version ≤ `0x100`) reads it as
   a literal `GetNext("BONEMATRIXMAP")` chunk alongside `BONEVERTEXINFO`/
   `BONEINFLUENCE`. `FUN_1007c150` (`MXR_ReadBoneInfluence.txt`) stores the pool
   bytes **verbatim** (no palette lookup), so palette→node resolution happens in
   the runtime skinning consumer; `FUN_10081b10` (`MXR_CreateMatrixIW.txt`) packs
   ≤4 indices/vertex into mesh `+0x30` with 3 weights/vertex at `+0x34`. The
   `matrix_map_id` is a **per-cluster id**, not a shared palette key (ADR-0426).
   **Runtime shape (ADR-0429):** the draw-side skin provider reads `mesh+0x24` as
   `{u32 n; u8 palette[n]}` — `FUN_1007eda0(mesh)` = `n`, `FUN_1007fa90(mesh)` =
   the palette bytes (`+4`) — so the runtime palette is exactly this file block.
- `VERTEXFRAMES` positions are byte-packed (not plain float3 arrays); the
  reader sizes the list then fills it in `CTriangleMeshCore::Read`. The precise
  packing (likely per-frame deltas) is the remaining decode target.
  **Resolved for the two shipped versions (ADR-0415 + ADR-0476):** frame 0x204
  (2004) is `header(52) + u16 pos[u16x3, 6n] + s8 nrm[3n]`; frame 0x205 (the
  2009 *Assault on Dark Athena* models) keeps that header/position/normal layout
  and appends **two more `s8` unit-vector arrays** (`3n` each) → `52 + 15n`.
  Positions dequantize with `min + q*(max-min)/65535`; the normals/basis
  re-normalize `s8/127`. Both are decoded by `tools/mos_xmd.py`
  (`decode_vertex_frame`), and the two versions are told apart by whether the
  chunk's first `u32` is itself a frame version (0x205) or a `frame_count` word
  (0x1, the 2004 prefix).

## Reproduce
```
$env:GHIDRA_INSTALL_DIR="C:\Users\GigelCastel\Tools\ghidra_12.1.4_PUBLIC"
rea search "<game>\System\Win32_x86\MXR.dll" "BONEDEFORM" --format json
rea xrefs  "<game>\System\Win32_x86\MXR.dll" --address 0x10149c88 --format json
rea decompile "<game>\System\Win32_x86\MXR.dll" --procedure 0x10080f50 --format json
rea decompile "<game>\System\Win32_x86\MCCDyn.dll" --procedure 0x10082f15 --format json
```
Container-level inventory without a decompiler: `python tools/mos_xmd.py --inventory <Content/Models>`.

## `.xsa` animation sets (`COMPRESSEDANIMATIONSET2`) — in `MXR.dll`
`.xsa` clips are the same `MOS DATAFILE2.0` container; their first child node is
`COMPRESSEDANIMATIONSET2`. Reader `FUN_10076d20` (`.\XRAnimCompressed.cpp`,
vtable `0x101442b8 +0x20`), reached from the dispatcher `FUN_1001d7c0`. It is the
offline source of per-bone **rotations** (ADR-0422). Layout:

```
QUATERNIONS        count * 4 x i16     -> /32767  (CXR_Anim_Quatint16, unit)
MOVES              count * 3 x f32     (translation, model units)
TRACKINDICES       count * u16         (per-key index into TRACKS)
QUATERNIONINDICES  count * u16         (per-key-slot index pool)
KEYFRAMETIMES      count * f32         (keyframe times)
TRACKS             count * { u16 start; u16 flags }   (CXR_Anim_CompressedTrack)
DATAKEYS           variable            (opaque)
SEQUENCES          variable, version 0x103
```

`SEQUENCES` records are read by `CXR_Anim_Sequence::Read` (`FUN_100774f0`,
`.\XRAnimCompressed.cpp`), whose eight tail words (u16,u16,u8,u8,u16,u16,u16,u16
at object `+0x20..+0x2e`) give each clip a **key window**
`[track_start, track_start + rotation_count + movement_count)` into
`TRACKINDICES` and a **time window** `[time_start, time_start + time_count)`
into `KEYFRAMETIMES`; over all 313 shipped sets the non-empty windows tile their
pools exactly. Skeleton-side binding is by **slot**: `CXR_Skeleton::Read`
(`FUN_100219d0`) finalises through `FUN_1001ff60`, which caps rotation slots at
`0x80` (128) and movement slots at `0x20` (32) from each `.xmd` `NODES` record's
`rotation_slot`/`movement_slot` (ADR-0418, 34-byte records).

**Clip slot -> bone binding** (runtime sampler `FUN_100776b0`, reached from
`FUN_10077f20` — the sequence object's `+0x24` method): a clip's key window is a
**slot -> track** table.
Its first `rotation_count` entries are indexed by rotation slot, the next
`movement_count` by movement slot. For slot `j`,
`track = TRACKINDICES[window_start + j]` and `(start, flags) = TRACKS[track]`.
`flags & 0x8000` means one key, else the track owns exactly `sequence.time_count`
keys, living in `QUATERNIONINDICES[start .. start + key_count)`. A rotation
slot's values always dereference `QUATERNIONS`; a movement slot's dereference
`MOVES` **unless** `flags & 0x0001` is set, in which case they dereference
`QUATERNIONS` (81 shipped movement blocks, e.g. `Johns.xsa` track 895 with values
up to 11229 against `MOVES.count == 7170`, prove the override; all fit
`QUATERNIONS`, none exceeds it). `flags & 0x4000` (unused) yields the
identity/zero key; `KEYFRAMETIMES` is indexed directly by the clip's time window.

Decoded read-only by `tools/xsa_anim.py` (`--inventory <Content> --check` →
313/313 ok; `--sequences` prints the per-clip windows; `--clip NAME` prints the
slot -> track -> pool table). `.xag` files are animation **graphs**
(states/actions/layers) and contain no `COMPRESSEDANIMATIONSET2`. See
`docs/adr/0422-xsa-compressed-animation-set.md`.

## Cluster header (`CLUSTER`/`MISCELLANEOUS`)
Every `CLUSTER` node begins with a 44-byte `MISCELLANEOUS` child that carries the
cluster's surface id and a **reference bounding box** (ADR-0425):

| off | type | field | shipped value |
|---|---|---|---|
| `+0x00` | u32 | `version` | `0x102` on every cluster |
| `+0x04` | u32 | `flags` | `0` exactly when a UV set is present (`{0:2099,1:839,3:1724}`) |
| `+0x08` | u32 | `surface_index` | `< SURFACES.count`, covers every surface |
| `+0x0C` | u32 | `reserved` | `0` |
| `+0x10` | f32[3] | `box_min` | reference lower corner |
| `+0x1C` | f32[3] | `box_max` | reference upper corner |
| `+0x28` | f32 | `trailer` | `0.0` |

The `version` word is the only byte `CTriangleMeshCore::Read` inspects (it drives
the model-version branches). The six floats are **exactly the paired
`VERTEXFRAMES` frame's quantization range** (`bounds_min`/`bounds_max`, ADR-0415)
on all 4662 shipped clusters, so the header mirrors the cluster's reference /
dequantization box; it encloses the decoded geometry but is a **bound, not** the
per-cluster reference transform (GH #59 stays open). Decoded by
`tools/mos_xmd.py` (`ClusterInfo.box_min/box_max/encloses`; `--check` enforces
the box == frame-bounds equality).

## Cluster reference pose vs `NODES` bind pose
A cluster's stored `VERTEXFRAMES` geometry is **not** in general the `NODES`
bind pose: the bind skeleton is one shared array, but each cluster may be stored
in its own reference pose. On `Riddick_B05.xmd` the legs cluster (`0/3`) and the
shadow-body cluster (`6`) agree per bone to **< 1 unit** (weighted geometry
centroid vs `NODES.translation`) while the two arm clusters (`1/2/4/5`) differ
from the body by **~26–29 units** and their geometry AABB (Z `44.2 .. 52.5`) is
disjoint from the body (Z `0 .. 36.3`) — the arms are stored in a raised pose and
sit above the body's head. `tools/xmd_pose.py` reports this per cluster
(`bone_centroids`, `bind_table`, `cluster_pose`, `cluster_relation`, `check`).
Consequence: a **skinned** native draw needs a per-cluster bind/reference
transform, not one global `NODES` bind. `docs/adr/0424-xmd-cluster-reference-pose-vs-bind.md`.

## Next steps (for the loop/next agent)
1. Finish the `VERTEXFRAMES` packing and `BONEDEFORM` stride by reading the
   remainder of `CTriangleMeshCore::Read` + `FUN_1007d260` and validating
   against `tools/mos_xmd.py` counts (e.g. Riddick_B03: 74 bones, 2925 skin
   records, 520 tris in cluster 1).
2. Locate hand/forearm bones: the skeleton is `SKELETON`→`NODES` (34 B/node).
   Bone **names** are not stored as strings; recover the hash/name scheme from
   the `.xsa/.xag/.xrg` animation groups or the MXR skeleton reader.
3. Emit an OBJ (positions + triangles + UVs) for `Riddick_B05`
   (`RIDDICK_ARMSCAR_*`) and `Pa2_Hand_01`, then a skinned draw path in
   `VRHandRenderer` behind a config flag (fallback: current procedural hands).
   **Done:** OBJ/JSON export = ADR-0419; baked native C++ asset + cluster→surface
   mapping = ADR-0420 (`tools/xmd_export.py`, `tools/xmd_bake.py`,
   `src/hook/native_hand_mesh.h`). Remaining: the `VRHandRenderer` draw path
   itself (rigid first, then skinned once bone rotations are known — they must
   come from the engine or the `.xsa`/`.xag` groups, **not** `KEYS`, which is a
   metadata map; see ADR-0421). A **skinned** path additionally needs a
   per-cluster reference transform, because the shipped clusters are not all in
   the `NODES` bind pose (ADR-0424, `tools/xmd_pose.py`). The `MISCELLANEOUS`
   header is **not** that transform (it is a reference bound — ADR-0425), and the
   `BONEMATRIXMAP` palette/id is **not** it either (it is a bone-index palette
   plus a per-cluster id — ADR-0426), so the remaining candidate is a runtime
   per-cluster matrix in the model/skin class (built from the live skeleton).
   **Resolved (ADR-0428/ADR-0429):** that matrix is not stored anywhere on the
   `.xmd` draw path — the provider just selects a matrix out of a **live** array
   owned by the model instance; a correct skinned draw needs runtime access
   (GH #34) or an offline `bind_inverse × .xsa` pose reconstruction.

## Draw-side skinning (ADR-0428) — the per-cluster transform does not exist here
`CXR_Model_TriangleMesh::Cluster_TransformBones_V_N_TgU_TgV` =
**`FUN_100254e0(mesh, provider)`** in MXR.dll (named by its warning string at VA
`0x10144a1d`; emitter `FUN_10025ced`, four call sites `0x10025595`..`0x100255ad`).
It reads only the mesh's geometry (`FUN_1007ee20`/`1007f100`/`100825a0`/`10082750`),
its per-vertex influence records (`FUN_1007f9f0`) and its influence pool
(`FUN_1007fa40`, `{u16 bone; u16 pad; f32 weight}`), and fetches **one 3×4 matrix
per bone** from the second argument via **`FUN_10022f90(provider, bone)`**. The
vertex is then `Σ w_k·M_k` (a plain/weight-blended matrix sum): no
bind/reference/inverse matrix is read from the mesh at all. So the hunt for a
"per-cluster reference transform" in the `.xmd` is **closed (negative)**; the
reference lives inside the matrix *provider*. Pseudocode:
`docs/rea/reference/MXR_ClusterTransformBones.txt`.

## The skin matrix provider (ADR-0429) - a palette selector, not a bind-inverse
The provider consumed by `FUN_100254e0` is built per skinned cluster by
**`FUN_100238a0(mesh, matrices, countCap, desc)`** (`0x100238a0`, called from
`FUN_1002dcc0`) as a transient **`0x10`-byte** object stored at `desc+0x0C`
(`desc` = global `0x1016a7d0`, so `desc+0x0C` = `DAT_1016a7dc`):

- `+0x00 float* matrices` = **`param_2`** — a **live** per-bone 3x4 array;
- `+0x04 u8* palette` = **`FUN_1007fa90(mesh)`** = `(mesh+0x24) + 4`, i.e. the
  file's **`BONEMATRIXMAP`/`matrix_bones`** bytes (`NULL` when the mesh has none);
- `+0x08 u32 count` = **`FUN_1007eda0(mesh)`** = `*(u32*)(mesh+0x24)` (the palette
  length; no-palette branch: `min(*(mesh+0x2c), countCap)`).

So `mesh+0x24` is `{u32 n; u8 palette[n]}`, and the provider reads **no** bind/
reference/inverse matrix — it only selects `matrices[palette[bone]]`. The matrices
themselves come from the **model instance** (in `FUN_1002dcc0`:
`local_84 = *(int*)(*(int*)(*(int*)(param_1 + 0x94) + 0x14) + 0x18)`), not from
the mesh. Pseudocode/field chain: `docs/rea/reference/MXR_SkinMatrixProvider.txt`.

**Corrected (ADR-0430):** the base is `param_1[0x25]` (`param_1 + 0x94`),
overridden by `*(int*)(param_6 + 0x3c)` when non-null — **not** `param_1[0x2c]`
(`+0xb0`) as ADR-0429 first recorded (the decompiler local was assigned twice).
The container is shaped like the engine's `TList_Vector<TMatrix43<float,float>>`
(`{+0x04 count; +0x18 data}`, `0x30`-byte elements).

**Answered (ADR-0440):** the builder of that live per-bone array is the
`CXR_SkeletonInstance` **forward kinematics** chain
`FUN_10021650` (virtual, `XRSkeleton.cpp`) → `FUN_10020b50` (evaluate the local
pose) → `FUN_1001f340` (tree walk). It composes
**`world[node] = world[parent] · local[node]`** (`FUN_1001db60`), i.e. **raw
world** matrices — **no bind-inverse** — writing world to `*(poseDesc+0x30)+node*
0x30` and local to `*(poseDesc+0x34)+node*0x30`. Pseudocode:
`docs/rea/reference/MXR_SkeletonInstance_WorldMatrices.txt`.

**Answered (ADR-0441):** `FUN_1001de10` (the descriptor's array (re)sizer, called
by `FUN_10021650`) **caches the FK arrays' data pointers**:
`poseDesc+0x30 = *(*(poseDesc+0x14)+0x18)` (world) and
`poseDesc+0x34 = *(*(poseDesc+0x0c)+0x18)` (local). So the list the draw reads at
`poseDesc+0x14` **does alias `+0x30`** — the skinning consumes the FK output
bit-identically. The skinning's per-bone fetch `FUN_10022f90(provider, bone)` is a
raw `0x30`-byte palette-indexed fetch (no math), and `FUN_100254e0` applies it as a
plain affine `v·R + T` with **no bind subtraction**. Finally, `FUN_1001f340`
overwrites the local translation to `bindpos − R·bindpos + t` (rotate about the
bone's bind position); at bind every world → identity. **The "world" matrices are
therefore bind-relative deltas (identity at bind), applied directly to the
model-space file geometry** — there is *neither* a load-time vertex rebase *nor* a
bind-inverse. The ADR-0440 (a)/(b) dichotomy was false. Pseudocode:
`docs/rea/reference/MXR_SkeletonInstance_WorldMatrices.txt` (wake-34 addendum);
ADR-0441.

**Consequence for the offline skinned native hand (#46/#58/#60):** FK from `NODES`
+ a `.xsa` pose (the engine's own parent-walk), producing per-bone bind→animated
deltas, applied to the model-space `.xmd` geometry — **no per-cluster bind
transform exists to find** (confirming the negative in ADR-0424/0428/0429). Because
the delta is identity at bind, the file mesh *is* the rest-pose model, so the
current rigid draw (ADR-0436) is a valid baseline.

**Implemented offline (ADR-0442):** `tools/xmd_fk.py` evaluates exactly this
recipe — `local = v ↦ R·(v − bindpos) + bindpos + t`, `world = parent · local`,
per-vertex influence weighted sum — and poses a cluster from a shipped `.xsa`
clip (`rotation_slot`/`movement_slot` → the clip's window slots, ADR-0422),
exporting a posed OBJ. Its `--check` confirms ADR-0441 over the shipped corpus:
268 models / 3454 clusters / 915508 vertices all reproduce the file geometry at
bind with worst drift **1.42e-14** (0 problems) — i.e. the file geometry *is* the
bind pose everywhere, with no bind-inverse and no load-time rebase.

**Pose source pinned (ADR-0443, GH #58):** the first-person arms are driven by
the `FPS*.xsa` clips in `Content/Anim/Main` (`FPSAssault/FPSPistol/FPSMini/
FPSGrenade/FPSMelee`), whose rotation windows cover `Riddick_B05`'s `NODES`
rotation slots `0..69` with no mapping notes — i.e. they key the *same* Riddick
skeleton the baked hand mesh is skinned to (no separate first-person arm model
ships). `tools/xmd_bake.py --anim/--sequence/--pose-key` now applies that recipe
at bake time and emits a **posed** variant of each skinned cluster into
`native_hand_mesh_data.h`; the draw path selects it under `[VR] NativeHandPose`
(default off). Canonical pose: `FPSMelee.xsa::Mel_ShaUnEquip` last key (hands
returned to rest, the most neutral shipped first-person frame).

## Related container: the `.xtc` texture packs (ADR-0431, GH #61)
The shipped textures are *not* loose files; they live in
`Content/{GUI,Models,Levels}.xtc` (plus
`ExtraContent/Offline01/Textures/Offline01.xtc`). A `.xtc` is the **same
`CDataFile` tree** as `.xmd` (identical `MOS DATAFILE2.0` magic), written by the
engine's `VirtualXTC` texture container. The class strings are in `MSystem.dll`:
`CTextureContainer_VirtualXTC2` / `CTextureContainer_VirtualXTC`,
`?AddFromXTC@CTextureContainer_Plain@@...`, `XTXCompile`,
`".\Raster\MTextureContainerXTC2.cpp"`, and the diagnostics
`"Invalid XTC. (IMAGELIST was not a directory.)"` / `"Invalid XTC. (No IMAGELIST)"`.

The three game packs share one layout, parsed by `tools/mos_xtc.py`:

1. **Root** = the usual 0x30-byte node record whose `+0x18` sibling field is the
   **node-table offset near EOF** and whose `+0x20` offset field is `0` (the root
   has no payload).
2. **Two contiguous nodes** at that offset: `TEXTURES` (payload = the packed
   texture-data blob, starting at file offset `0x30`) and `IMAGEDIRECTORY5`
   (payload = the image directory, the last region before the node table). The
   two payloads tile `[0x30, node_start)` exactly.

   | pack | size | node_start | TEXTURES blob | directory |
   |---|---|---|---|---|
   | `GUI.xtc` | 0x112C9E6 | 0x112C986 | 0x30 + 0x1127322 | 0x1127352 + 0x5634 |
   | `Models.xtc` | 0x28DEFF6B | 0x28DEFF0B | 0x30 + 0x28DDD11B | 0x28DDD14B + 0x12DC0 |
   | `Levels.xtc` | 0x5AE8DCB1 | 0x5AE8DC51 | 0x30 + 0x5AE40437 | 0x5AE40467 + 0x4D7EA |

3. **`IMAGEDIRECTORY5`** is a flat list of framed records (no nested tree):
   `u32 marker=0xFFFFFFFF; u32 0; u32 name_len; char name[name_len]; 0x20 pad to
   4; u32 desc[14+]`. 246 (GUI) / 901 (Models) / 3797 (Levels) records, which
   tile the directory payload exactly.

   `desc` (little-endian). Pinned, and verified across all **4944** records:

   | idx | meaning |
   |---|---|
   | 1 | mip level count == `log2(max(w,h)) + 1` (4944/4944) |
   | 2 | `0x300` constant |
   | 3 | `0x5000` colour / `0x0` linear (normal map) |
   | 4 | base mip byte count, OR `0x10` when block-compressed |
   | 5,6 | width, height (powers of two) |
   | 9 | absolute file offset of the packed block (strictly increasing, inside `TEXTURES`) |
   | 10 | `0x102` colour / `0x4300102` linear |

   E.g. `Riddick_ArmScar_D` = 512x1024, 11 mips, compressed, base mip `0x40000`
   bytes (DXT1-sized), block at `0x16CDEABE` (+350072 B). `riddick_armscar_N` =
   512x1024, uncompressed (`0x200000` = 512*1024*4 B).

4. Each packed block is a **mip chain**: a `u32` mip-offset table (`mip_count`
   absolute file offsets, where `mip_count = (offset[0] - block_offset) / 4`),
   then one `[header + pixel data]` record per mip. The header is `0x2C` bytes
   for compressed images and `0x1C` for raw ones; the invariant
   `offset[i] + header_len + mip_bytes == offset[i+1]` holds on 887/901 (Models),
   244/246 (GUI) and 3787/3797 (Levels) images.

   The per-mip header carries width (+0x0C), height (+0x10) and `mip_bytes`
   (+0x20 compressed / +0x08 raw), and — for compressed mips — a **codec field
   at +0x1C**: `0` = **DXT1/BC1** (`mip_bytes == w*h/2`), `2` = **DXT3/BC2**,
   `4` = **DXT5/BC3** (both `mip_bytes == w*h`). A `mip_bytes == 0` mip is empty.
   Decoding as standard S3TC reproduces the shipped art (BC1 on
   `Riddick_ArmScar_D`/`Riddick_B03_D`, BC3 on `Pa2_inmate_H04_D`, BC2 on
   `SKYMOUNT_0` — all verified visually). `tools/mos_xtc.py --decode NAME --out
   FILE.png` writes a mip to an RGBA PNG (`--mips NAME` prints the chain). The
   raw/linear images are 32-bit but their channel order is not yet decoded; the
   engine reader is `CTextureContainer_VirtualXTC2::ReadTexture` /
   `GetTextureMipMap` / `CImage::ReadHeader` (ADR-0432).

5. `tools/xtc_bake.py` bakes one **DXT1** mip of an image into a compile-in C++
   asset (`src/hook/native_hand_texture_data.h`), storing the shipped BC1 blocks
   verbatim plus decoded reference pixels/checksum; `src/hook/bc1.h` decodes them
   at runtime and `VRHandRenderer::DrawNativeHand` binds the result (ADR-0433,
   GH #61). Canonical bake: `Riddick_ArmScar_D` mip 1 (256x512). For an
   inventory of a single image: `xtc_bake.py --image NAME --mip N --json`.

Caveats: the node `count` field (`+0x28`) is 2-14 higher than the framed-record
count (e.g. Models says 914, frames 901) - the framing tiles exactly, so the
framed records are authoritative. `Offline01.xtc` uses a *different*, nested
variant (`SOURCE`/`IMAGELIST`/`IMAGEDIRECTORY4`/`IMAGE`/`NAME`/`PROPERTIES`/
`MIPMAP`), out of scope here.

