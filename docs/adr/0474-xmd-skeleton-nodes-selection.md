# ADR-0474: `NODES` is not unique — select the skeleton by `SKELETON` parent / `NODEINDICES` sibling

- Status: Accepted (scheduled autoloop wake 73, GH #84)
- Date: 2026-10-18
- Related: GH #84, GH #85, GH #82 (test temp isolation), ADR-0418 (SKELETON
  `NODES`/`NODEINDICES` layout), ADR-0414 (container loader), ADR-0471 (XRSIM
  read-only shadow install)

## Context

`Xmd.skeleton()` resolved its payload with `self.chunk("NODES")`, and `chunk()`
(`tools/mos_xmd.py`) returns the **first** node named `NODES` in node-table order.
On the shipped 2009 *Assault on Dark Athena* models most static props and many
characters carry a `NODES` chunk under a **`BOXTREE`** bounding-volume node
(12 bytes per record) ahead of the `SKELETON` array (34 bytes per record), so the
first match is frequently **not** the skeleton. `decode_skeleton` then raised
`GeometryError: NODES size 228 != 19 * 34`, and because `xmd_pose.check`
(`tools/xmd_pose.py`) and `xmd_fk.check` (`tools/xmd_fk.py`) called `x.skeleton()`
*outside* their `try/except`, a single such model aborted the whole corpus walk.

### Measured (read-only; installs untouched)

Two installs, both surveyed with `Xmd`:

| install | `.xmd/.xmg` under `Content\Models` | `NODES` whose parent is `SKELETON` | `NODES` with neither a `SKELETON` parent nor a `NODEINDICES` sibling |
|---|---|---|---|
| 2004 `C:\Program Files (x86)\Starbreeze Studios\Riddick EFBB` | 631 | 873 (all 34 B, sibling `NODEINDICES`) | 0 |
| 2009 `F:\The Chronicles of Riddick - Assault on Dark Athena` | 1285 | 1504 (all 34 B, sibling `NODEINDICES`) | 1411 (all `BOXTREE` children, 12 B, sibling `TRIANGLES`) |

**Guard:** across both installs there are **zero** `NODES` that lack *both* a
`SKELETON` parent *and* a `NODEINDICES` sibling yet still tile 34-byte records, so
a tiling fallback can never out-rank a real skeleton on shipped assets.

### Correction to the GH #84 / ADR-0473 record

GH #84's survey ("1285 files", the `CoordSys.xmd` example) measured the **2009**
install on `F:`, not the 2004 install: the 2004 `C:` install has 631 models and
every `NODES` there is a `SKELETON` child, so its
`xmd_pose/xmd_fk --inventory --check` is **green** (0 problems, 268 models). The
quantity "1285" that ADR-0473 quotes came from a `%TEMP%` XRSIM shadow install
whose source root was the 2009 `F:` install.

## Decision

`skeleton()` selects the *skeleton* `NODES`, not the first one:

1. a `NODES` whose parent node is `SKELETON` wins (rank 3);
2. else a `NODES` whose **next sibling** is `NODEINDICES` (rank 2) — this covers the
   synthetic bare-root-`NODES` containers in the tests;
3. else a `NODES` whose payload tiles `NODE_ENTRY_LEN` (34) records with the chunk
   header count agreeing (rank 1);
4. otherwise **no** candidate qualifies and `skeleton()` returns `None`.

The strongest signal wins; the highest-ranked candidate in node-table order breaks
ties. `NODEINDICES` is likewise taken from the chosen `NODES`' sibling
(`_skeleton_indices_node`), falling back to the first `NODEINDICES`.

A **genuine** 34-byte count mismatch still raises: a `NODES` with a `NODEINDICES`
sibling is selected (rank 2) and `decode_skeleton` raises
`NODES size 34 != 3 * 34` as before (ADR-0418,
`test_skeleton_detects_size_mismatch`).

The two corpus checks also move `x.skeleton()` **inside** their `try/except`, so a
single genuinely-malformed file is *reported* as a problem instead of aborting the
walk. The new helper `Xmd.skeleton_node()` exposes the selection;
`geometry_summary()` and `validate()` use it for `skeleton_node_count` / bone-index
checks instead of `chunk("NODES")`.

## Consequences

- The corpus walk is **decidable**: `xmd_pose/xmd_fk --inventory --check` completes
  on the 2009 install instead of dying on the first `BOXTREE` prop.
- `C:` (2004) stays green: 268 models / 3454 clusters / **0** problems.
- `F:` (2009) is now red for a **different, real** reason: **4566** problems, all
  `VERTEXFRAMES version 0x<...> not decodable (only 0x202-0x204)`, across 479 files.
  2009 `VERTEXFRAMES` payloads start with version **`0x205`** (8645/8805 nodes) with
  no leading frame count, where 2004 uses `0x1` + version `0x204` (4662/4662). Filed
  as **GH #85**; the `skeleton()` fix did **not** cause it (it reproduces through
  `x.vertex_frames(...)` directly).
- A static prop now decodes to `skeleton() is None` (and `skeleton_node_count == 0`)
  rather than raising, which is what `cluster_pose` / `bind_table` callers assume.

## Validation

- `python tools/test_mos_xmd.py` — incl. the new
  `test_boxtree_nodes_is_not_a_skeleton` (a 12-byte `BOXTREE` `NODES` → `None`) and
  `test_skeleton_selects_skel_nodes_over_boxtree` (a `BOXTREE` `NODES` ahead of a
  real `SKELETON` pair); `tools/test_xmd_pose.py` — incl.
  `test_check_skips_boxtree_prop` and `test_check_reports_skeleton_size_mismatch`
  (still reported, no abort); `tools/test_xmd_fk.py` — incl.
  `test_check_skips_boxtree_prop`. Full `tools/test_*.py` 11/11 PASS.
- Read-only corpus (no install write): 2004 `C:` → 0 problems; 2009 `F:` → walk
  completes, 4566 reported problems (GH #85).
- No C++ changed, so no `build-all.ps1`/CTest (documented).
