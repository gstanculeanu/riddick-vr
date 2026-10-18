# ADR-0477: `TRIANGLES` is not unique — select the mesh chunk, not the `BOXTREE` leaf

- Status: Accepted (scheduled autoloop wake 76, GH #86)
- Date: 2026-10-18
- Related: GH #86, GH #85 (`VERTEXFRAMES` 0x205), GH #84 / ADR-0474 (the same
  collision for `NODES`), ADR-0415 (2004 `VERTEXFRAMES`/`TRIANGLES`), ADR-0414
  (container loader), `tools/mos_xmd.py`, `docs/rea/mos-xmd-format.md`

## Context

With GH #85 landed, the documented corpus checks completed on the 2009
*Assault on Dark Athena* install (`F:`) and reported **41 problems, all of one
kind**: `TRIANGLES size <N> != 4 + <C>*6`, with the declared `<C>` a multiple of
65536 (GH #86). The failing payloads began, e.g. for `Misc\Cigarette.xmd`
cluster 3, `00 00 05 00 07 00 00 00 06 00 05 00 …` — which reads as a plausible
`u16` index stream but not as a `u32 count` + `u16` list, so the issue proposed a
new 2009 `TRIANGLES` variant.

It is not a variant. It is the **same name collision as GH #84**, one level down.

### Measured (read-only; installs untouched)

`Xmd` walk of both installs, parent of every `TRIANGLES` node derived from the
container's child links:

| install | `.xmd` under `Content\Models` | `TRIANGLES` parents | mesh `TRIANGLES` | non-mesh payloads |
|---|---|---|---|---|
| 2004 `C:` | 631 | `CLUSTER` 4662 | 4662 (all decode `u32 count` + `u16[3·count]`) | 0 |
| 2009 `F:` | 1285 | `VERTEXBUFFER` 8645, `CLUSTER` 160, **`BOXTREE` 1411** | 8805 (all decode `u32 count` + `u16[3·count]`) | 1411 |

Every one of the 1411 `BOXTREE` `TRIANGLES` payloads fits the *same* alternative
shape exactly (0 remainders): a sequence of runs, each
`u16 tag; u16 count; u16[count]` — a per-leaf triangle set (`Σ(4 + 2·count)`
bytes, `count` 1-7), not a mesh index list. Its `BOXTREE` sibling set is
`{DIRECTORYHEADER, INVRADIUS, NODES, TRIANGLES}` (1411 nodes; the `NODES` is the
12-byte BV record set of GH #84, the `TRIANGLES` is the BV leaf geometry).

**Mechanism (decisive):** the engine's mesh reader resolves the cluster's
indices with `bVar2 = CDataFile::GetNext(param_1, "TRIANGLES")` — a *sibling* of
the cluster data-file node (`MXR_CTriangleMeshCore_Read.txt:380`, the same
`GetNext` lookup that finds `VERTEXFRAMES`/`TVERTEXFRAMES`/`BONEDEFORM`). A
`BOXTREE` **child** can therefore never be a cluster chunk.

**Guard:** selecting the mesh `TRIANGLES` (see Decision) yields exactly one per
`VERTEXFRAMES` on **every** model of both installs (4662/4662 and 8805/8805,
zero mismatches) and every selected node decodes with `decode_triangles`.

### The pairing was silently wrong, not merely red

`xmd_export.decode_cluster` / `Xmd.cluster_meshes` paired cluster *i* with the
*i*-th `TRIANGLES` in traversal order. Because the `BOXTREE` leaves are
**interleaved** (not appended), every cluster after the first leaf was paired
with the *previous* cluster's triangle list:

| install | clusters | models with a shifted pairing | clusters mis-paired |
|---|---|---|---|
| 2009 `F:` | 8805 | 289 / 1285 | **1938** |

Only 41 of those 1938 raised an error (the ones that landed on a leaf payload);
the other ~1897 decoded the *wrong* index list silently (e.g. `Cigarette.xmd`:
`vf#3` paired with `tri#4`, which belongs to `vf#3` only after the leaf at
position 3 is removed). This is a data-correctness bug in the decode path a
native-hand mesh bake (GH #55) consumes.

## Decision

`TRIANGLES` selection is centralized on `Xmd.triangle_nodes()`:

- it returns the mesh `TRIANGLES` **in traversal order**;
- a node is excluded only when its parent is a `BOXTREE` that owns **no**
  `VERTEXFRAMES` (the conclusive BV-leaf case) — so a container whose parent
  links are unknown (a flat synthetic container, e.g. the existing tests) keeps
  its chunks;
- `Xmd.triangle_node()` returns the first mesh chunk (`None` when there is
  none, e.g. a `BOXTREE`-only prop).

`triangle_indices()` (default node), `cluster_meshes()`, `geometry_summary()`
and `xmd_export.decode_cluster` / the new `xmd_export.triangle_nodes` all use it
instead of `chunk("TRIANGLES")` / an unfiltered traversal list. This mirrors
ADR-0474's `skeleton_node()` for the identical `NODES`/`BOXTREE` collision.

## Consequences

- `xmd_pose/xmd_fk --inventory --check` on the 2009 target install is **green**:
  `F:` **0 problems / 4643 clusters** (was 41 / 4602 — the 41 fixed clusters now
  decode and are counted), `xmd_fk` worst bind-pose drift 2.8e-14.
- The **2004** install is unchanged: `C:` 268 models / 3454 clusters / 0
  problems, `xmd_fk` 0 problems.
- Cluster→triangle pairing is correct for **1938** previously mis-paired 2009
  clusters, i.e. the 2009 geometry a native draw path reads is now the cluster's
  own.
- A prop whose only `TRIANGLES` is a `BOXTREE` leaf now reports no mesh
  triangles (`triangle_indices() == (0, ())`) instead of a size error.
- No 2009 `TRIANGLES` "variant" needs a reader: the 2004 shape (`u32 count` +
  `u16` triplets) covers every mesh chunk on both installs.

## Validation

- `python tools/test_mos_xmd.py` — new `test_triangle_nodes_skips_boxtree_leaf`
  (a `BOXTREE` leaf ahead of two clusters: `chunk("TRIANGLES")` still returns
  the trap, `decode_triangles` on it still raises, while `triangle_nodes()` skips
  it and both clusters pair and decode) and
  `test_boxtree_only_triangles_means_no_mesh`.
- `tools/test_xmd_export.py` — new `test_decode_cluster_skips_boxtree_triangles`
  (`decode_cluster(x, 1)` returns `(2, 1, 0)`, not the leaf/shifted list);
  `tools/test_xmd_pose.py` — new `test_check_pairs_boxtree_triangles` (corpus
  `--check` over such a model is 0 problems / 2 clusters).
- Read-only corpus, both installs, before/after measured in one session by
  `git stash`-ing only the two source files: `F:` pose 41/4602 → **0/4643**,
  `F:` fk 41/4602/2,763,282 verts → **0/4643/2,778,422**; `C:` 0/3454 unchanged.
- No C++ changed, so no `build-all.ps1`/CTest (documented). No live/sim run:
  nothing player-visible changed.
