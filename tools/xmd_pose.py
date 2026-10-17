#!/usr/bin/env python3
"""Reference-pose vs bind-pose analysis for ``MOS DATAFILE2.0`` ``.xmd`` models.

The export/bake pipeline (``tools/xmd_export.py``, ``tools/xmd_bake.py``) already
turns a ``.xmd`` into geometry + skeleton + resolved skin weights (GH #55). The
remaining blocker for a **native skinned draw path** (GH #46) is the pose/base
pairing: does a cluster's stored ``VERTEXFRAMES`` geometry live in the ``NODES``
bind pose, and if not, how does it relate to the bind pose and to its sibling
clusters?

This tool answers that read-only and deterministically:

  * ``bind_table``      - the ``NODES`` bind skeleton as a renderer-facing table
                          (bind translation, parent, children, rotation/movement
                          slot, model-space local offset, derived bone axis).
  * ``cluster_pose``    - per cluster: geometry AABB, the AABB of the bind
                          positions of the bones the cluster uses, the
                          weighted-centroid bind offset per bone, and summary
                          offsets (RMS/max) that quantify how far the stored
                          geometry sits from the bind pose. Also carries the
                          ``MISCELLANEOUS`` reference box and whether it encloses
                          the stored geometry (ADR-0425).
  * ``cluster_relation``- two clusters' shared bones: how far their stored
                          per-bone centroids are from each other and whether
                          their geometry AABBs overlap.
  * ``analyze``         - the whole model (skeleton + clusters + relations).

The offset of a bone's *weighted geometry centroid* from its *bind translation*
is a coarse but scale-free way to compare a cluster against the bind pose; when
two clusters agree on that offset for every shared bone they are stored in the
same reference pose, and when they disagree they are not. This is deliberately a
**diagnostic**: it reports the pairing, it does not silently assume one.

Strictly read-only: the game install is never written and no input is
synthesised. See ``docs/adr/0424-*.md`` for the finding this produced on the
canonical Riddick models.

Usage::

    python tools/xmd_pose.py --model <file.xmd> --json
    python tools/xmd_pose.py --model <file.xmd> --bind-table
    python tools/xmd_pose.py --model <file.xmd> --cluster 1 --cluster 6 --offsets
    python tools/xmd_pose.py --model <file.xmd> --relation 1 6
    python tools/xmd_pose.py --inventory <game>\\Content\\Models --check
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mos_xmd import GeometryError, Xmd, XmdError  # noqa: E402
import xmd_export  # noqa: E402

DEFAULT_GAME_DIR = r"C:\Program Files (x86)\Starbreeze Studios\Riddick EFBB"
MODELS_SUBDIR = os.path.join("Content", "Models")

# Fallback bone axis when a node has neither children nor a parent direction.
UP_AXIS = (0.0, 0.0, 1.0)


# --------------------------------------------------------------------------- #
# small vector helpers (dependency-free)
# --------------------------------------------------------------------------- #
def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _len(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _dist(a, b):
    return _len(_sub(a, b))


def _unit(v):
    n = _len(v)
    if n <= 1e-12:
        return UP_AXIS
    return (v[0] / n, v[1] / n, v[2] / n)


def _mean(points):
    n = len(points)
    if not n:
        return (0.0, 0.0, 0.0)
    return (sum(p[0] for p in points) / n,
            sum(p[1] for p in points) / n,
            sum(p[2] for p in points) / n)


def _aabb(points):
    if not points:
        return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
    mn = tuple(min(p[i] for p in points) for i in range(3))
    mx = tuple(max(p[i] for p in points) for i in range(3))
    return mn, mx


def _inside(inner_mn, inner_mx, outer_mn, outer_mx, tol=1e-3):
    return all(inner_mn[i] >= outer_mn[i] - tol
               and inner_mx[i] <= outer_mx[i] + tol for i in range(3))


def _aabb_overlap_fraction(inner_mn, inner_mx, outer_mn, outer_mx):
    """Fraction of the inner AABB's volume that overlaps the outer AABB (0..1)."""
    volume = 1.0
    overlap = 1.0
    for i in range(3):
        span = inner_mx[i] - inner_mn[i]
        if span <= 1e-9:
            continue
        volume *= span
        lo = max(inner_mn[i], outer_mn[i])
        hi = min(inner_mx[i], outer_mx[i])
        overlap *= max(0.0, hi - lo)
    if volume <= 1e-9:
        return 0.0
    return overlap / volume


# --------------------------------------------------------------------------- #
# skinning geometry helpers
# --------------------------------------------------------------------------- #
def bone_centroids(mesh):
    """Weighted centroid + total influence weight per resolved skeleton node.

    Uses the ``matrix_bones`` palette (ADR-0423) so the keys are real skeleton
    node indices. Returns ``{node: {"centroid", "weight", "vertex_count"}}``
    for every node that receives a positive total weight."""
    acc: dict[int, list] = {}
    if mesh.skin is None:
        return {}
    for vi in range(mesh.vertex_count):
        inf = mesh.skin.resolved_influences_of(vi)
        if not inf:
            continue
        p = mesh.positions[vi]
        for node, w in inf:
            if w <= 0.0:
                continue
            a = acc.setdefault(node, [0.0, 0.0, 0.0, 0.0, 0])
            a[0] += p[0] * w
            a[1] += p[1] * w
            a[2] += p[2] * w
            a[3] += w
            a[4] += 1
    out = {}
    for node, (sx, sy, sz, w, n) in acc.items():
        if w <= 0.0:
            continue
        out[node] = {"centroid": (sx / w, sy / w, sz / w),
                     "weight": w, "vertex_count": n}
    return out


def bind_table(skeleton):
    """The ``NODES`` bind skeleton as a renderer-facing table (used nodes)."""
    table = []
    for n in skeleton.used_nodes():
        children = skeleton.children_of(n.index)
        child_pts = [skeleton.nodes[c].translation for c in children
                     if 0 <= c < len(skeleton.nodes)]
        if child_pts:
            axis = _unit(_sub(_mean(child_pts), n.translation))
        elif 0 <= n.parent < len(skeleton.nodes) and n.parent != n.index:
            axis = _unit(_sub(n.translation, skeleton.nodes[n.parent].translation))
        else:
            axis = UP_AXIS
        local = (_sub(n.translation, skeleton.nodes[n.parent].translation)
                 if 0 <= n.parent < len(skeleton.nodes) else (0.0, 0.0, 0.0))
        table.append({
            "index": n.index,
            "parent": n.parent,
            "children": children,
            "rotation_slot": n.rotation_slot,
            "movement_slot": n.movement_slot,
            "translation": list(n.translation),
            "local_offset": list(local),
            "bind_axis": list(axis),
            "is_leaf": not children,
        })
    return table


def cluster_pose(x, cluster, table=None):
    """Quantify cluster ``cluster``'s stored geometry against the ``NODES`` bind."""
    mesh = xmd_export.decode_cluster(x, cluster)
    sk = x.skeleton()
    tmap = ({t["index"]: t for t in table} if table is not None
            else {t["index"]: t for t in bind_table(sk)})
    cents = bone_centroids(mesh)

    offsets = []
    for node in sorted(cents):
        if node not in tmap:
            continue
        bind = tmap[node]["translation"]
        c = cents[node]["centroid"]
        off = _sub(c, bind)
        offsets.append({
            "node": node,
            "centroid": list(c),
            "bind": list(bind),
            "offset": list(off),
            "distance": _len(off),
            "weight": cents[node]["weight"],
        })

    bind_pts = [tmap[n]["translation"] for n in sorted(cents) if n in tmap]
    bind_mn, bind_mx = _aabb(bind_pts)
    geom_mn, geom_mx = mesh.bounds_min, mesh.bounds_max

    if offsets:
        mean_off = _mean([tuple(o["offset"]) for o in offsets])
        off_rms = math.sqrt(sum(o["distance"] ** 2 for o in offsets)
                            / len(offsets))
        max_off = max(o["distance"] for o in offsets)
        spread = math.sqrt(sum(_len(_sub(tuple(o["offset"]), mean_off)) ** 2
                               for o in offsets) / len(offsets))
    else:
        mean_off = (0.0, 0.0, 0.0)
        off_rms = max_off = spread = 0.0

    return {
        "cluster": cluster,
        "vertices": mesh.vertex_count,
        "triangles": mesh.tri_count,
        "skinned": mesh.skin is not None,
        "bone_space": mesh.bone_space,
        "bounds_min": list(geom_mn),
        "bounds_max": list(geom_mx),
        "bones": sorted(cents),
        "bind_bounds_min": list(bind_mn),
        "bind_bounds_max": list(bind_mx),
        "geometry_inside_bind_bounds": _inside(geom_mn, geom_mx, bind_mn, bind_mx),
        "bind_overlap": _aabb_overlap_fraction(geom_mn, geom_mx, bind_mn, bind_mx),
        "bind_offset_rms": off_rms,
        "bind_offset_max": max_off,
        "bind_offset_mean": list(mean_off),
        "bind_offset_spread": spread,
        "offsets": offsets,
    }


def _shared_offsets(pa, pb):
    a = {o["node"]: o for o in pa["offsets"]}
    b = {o["node"]: o for o in pb["offsets"]}
    shared = sorted(set(a) & set(b))
    return shared, a, b


def cluster_relation(pa, pb):
    """Compare two clusters' stored per-bone centroids (shared bones only)."""
    shared, a, b = _shared_offsets(pa, pb)
    if not shared:
        return {"a": pa["cluster"], "b": pb["cluster"], "shared": 0,
                "centroid_delta_rms": 0.0, "centroid_delta_max": 0.0,
                "offset_delta_rms": 0.0, "offset_delta_max": 0.0,
                "bounds_overlap": False}
    centroid_deltas, offset_deltas = [], []
    for node in shared:
        centroid_deltas.append(
            _dist(tuple(a[node]["centroid"]), tuple(b[node]["centroid"])))
        offset_deltas.append(
            _dist(tuple(a[node]["offset"]), tuple(b[node]["offset"])))
    n = len(shared)
    return {
        "a": pa["cluster"],
        "b": pb["cluster"],
        "shared": n,
        "centroid_delta_rms": math.sqrt(sum(d * d for d in centroid_deltas) / n),
        "centroid_delta_max": max(centroid_deltas),
        "offset_delta_rms": math.sqrt(sum(d * d for d in offset_deltas) / n),
        "offset_delta_max": max(offset_deltas),
        "bounds_overlap": not (pa["bounds_max"][0] < pb["bounds_min"][0]
                               or pb["bounds_max"][0] < pa["bounds_min"][0]
                               or pa["bounds_max"][1] < pb["bounds_min"][1]
                               or pb["bounds_max"][1] < pa["bounds_min"][1]
                               or pa["bounds_max"][2] < pb["bounds_min"][2]
                               or pb["bounds_max"][2] < pa["bounds_min"][2]),
    }


def _compact(report, want_offsets, want_bind_table):
    """A committable JSON view: drop the bulky per-bone arrays by default."""
    out = dict(report)
    out["clusters"] = []
    for p in report["clusters"]:
        q = {k: v for k, v in p.items() if k != "offsets"}
        if want_offsets:
            q["offsets"] = p["offsets"]
        out["clusters"].append(q)
    if not want_bind_table:
        out.pop("bind_table", None)
    return out


def analyze(x, clusters=None):
    """Full reference-pose report for one ``.xmd``."""
    sk = x.skeleton()
    table = bind_table(sk) if sk else []
    infos = []
    try:
        infos = x.cluster_infos()
    except GeometryError:
        infos = []
    nclusters = len(xmd_export.vertex_frame_nodes(x))
    if clusters is None:
        clusters = list(range(nclusters))
    poses = [cluster_pose(x, c, table) for c in clusters]
    for p in poses:
        ci = p["cluster"]
        info = infos[ci] if ci < len(infos) else None
        p["surface_index"] = info.surface_index if info else None
        if info is not None:
            p["reference_box_min"] = list(info.box_min)
            p["reference_box_max"] = list(info.box_max)
            p["reference_box_encloses_geometry"] = info.encloses(
                p["bounds_min"], p["bounds_max"], tol=1e-3)
    relations = []
    for i in range(len(poses)):
        for j in range(i + 1, len(poses)):
            relations.append(cluster_relation(poses[i], poses[j]))
    return {
        "source": os.path.abspath(x.path),
        "coordinate_system": xmd_export.COORDINATE_SYSTEM,
        "skeleton": {
            "node_count": len(sk.nodes) if sk else 0,
            "used_nodes": len(sk.used_nodes()) if sk else 0,
            "bind_table_entries": len(table),
        },
        "bind_table": table,
        "clusters": poses,
        "relations": relations,
    }


# --------------------------------------------------------------------------- #
# corpus check
# --------------------------------------------------------------------------- #
def _iter_models(root):
    for dirpath, _dirs, files in os.walk(root):
        for name in sorted(files):
            if name.lower().endswith((".xmd", ".xmg")):
                yield os.path.join(dirpath, name)


def check(root):
    """Structural invariants over every model under ``root``.

    Returns ``(problems, totals)``; an empty ``problems`` means pass."""
    problems: list[str] = []
    totals = {"models": 0, "skinned_clusters": 0, "clusters": 0}
    for path in _iter_models(root):
        try:
            x = Xmd(path)
            # A per-file skeleton problem must be *reported*, not abort the
            # whole corpus walk (GH #84): skeleton() returns None for a static
            # prop / BOXTREE NODES, but still raises on a genuine 34-byte count
            # mismatch that --check is meant to surface.
            sk = x.skeleton()
        except (XmdError, GeometryError) as e:
            problems.append(f"{path}: {e}")
            continue
        if sk is None:
            continue
        totals["models"] += 1
        table = bind_table(sk)
        idx = {t["index"] for t in table}
        try:
            infos = x.cluster_infos()
        except GeometryError:
            infos = []
        for c in range(len(xmd_export.vertex_frame_nodes(x))):
            try:
                p = cluster_pose(x, c, table)
            except (XmdError, GeometryError) as e:
                problems.append(f"{path} cluster {c}: {e}")
                continue
            totals["clusters"] += 1
            if p["skinned"]:
                totals["skinned_clusters"] += 1
            if c < len(infos) and not infos[c].encloses(
                    p["bounds_min"], p["bounds_max"], tol=1e-3):
                problems.append(
                    f"{path} cluster {c}: reference box "
                    f"{list(infos[c].box_min)}..{list(infos[c].box_max)} does "
                    f"not enclose geometry {p['bounds_min']}.."
                    f"{p['bounds_max']}")
            for o in p["offsets"]:
                if o["node"] not in idx:
                    problems.append(
                        f"{path} cluster {c}: bone {o['node']} not a used node")
                for key in ("centroid", "bind", "offset"):
                    if any(not math.isfinite(v) for v in o[key]):
                        problems.append(
                            f"{path} cluster {c}: non-finite {key} on node "
                            f"{o['node']}")
            if p["skinned"] and not p["offsets"]:
                problems.append(f"{path} cluster {c}: skinned but no weights")
    return problems, totals


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _print_pose(p):
    print(f"cluster {p['cluster']:2d}: verts={p['vertices']:5d} "
          f"space={p['bone_space']:7s} "
          f"geom_z={p['bounds_min'][2]:7.2f}..{p['bounds_max'][2]:7.2f} "
          f"bind_z={p['bind_bounds_min'][2]:7.2f}..{p['bind_bounds_max'][2]:7.2f} "
          f"bind_overlap={p['bind_overlap']:5.2f} "
          f"off_rms={p['bind_offset_rms']:7.3f} "
          f"refbox={p.get('reference_box_encloses_geometry')}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME_DIR,
                    help="game install root (default: %(default)s)")
    ap.add_argument("--model", metavar="FILE", help="analyze one .xmd")
    ap.add_argument("--cluster", type=int, action="append", metavar="N",
                    help="restrict to cluster N (repeatable; with --model)")
    ap.add_argument("--relation", type=int, nargs=2, metavar=("A", "B"),
                    help="print the relation between clusters A and B")
    ap.add_argument("--bind-table", action="store_true",
                    help="print/include the NODES bind table")
    ap.add_argument("--offsets", action="store_true",
                    help="include per-bone bind offsets in JSON output")
    ap.add_argument("--inventory", metavar="DIR",
                    help="run --check over every model under DIR")
    ap.add_argument("--check", action="store_true",
                    help="validate structural invariants (with --inventory)")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    args = ap.parse_args(argv)

    try:
        if args.inventory:
            root = args.inventory
            if not os.path.isdir(root):
                print(f"ERROR: not a directory: {root}", file=sys.stderr)
                return 1
            problems, totals = check(root)
            if args.json:
                print(json.dumps({"problems": problems, "totals": totals},
                                 indent=2))
            else:
                print(f"== xmd_pose --check {root}")
                print(f"   models={totals['models']} clusters={totals['clusters']} "
                      f"skinned={totals['skinned_clusters']}")
                print(f"   problems: {len(problems)}")
                for p in problems[:50]:
                    print(f"     FAIL {p}")
            return 1 if problems else 0

        if not args.model:
            ap.error("--model or --inventory is required")

        x = Xmd(args.model)
        if args.bind_table and not args.json and not args.cluster:
            for t in bind_table(x.skeleton()):
                print(f"node {t['index']:3d} parent={t['parent']:3d} "
                      f"rot={t['rotation_slot']:4d} mov={t['movement_slot']:4d} "
                      f"t=({t['translation'][0]:8.3f},{t['translation'][1]:8.3f},"
                      f"{t['translation'][2]:8.3f}) "
                      f"axis=({t['bind_axis'][0]:6.3f},{t['bind_axis'][1]:6.3f},"
                      f"{t['bind_axis'][2]:6.3f})")
            return 0

        report = analyze(x, args.cluster)
        if args.relation:
            a, b = args.relation
            rel = next((r for r in report["relations"]
                        if r["a"] == a and r["b"] == b
                        or r["a"] == b and r["b"] == a), None)
            if rel is None:
                print(f"ERROR: clusters {a}/{b} not both analyzed",
                      file=sys.stderr)
                return 1
            print(json.dumps(rel, indent=2))
            return 0

        if args.json:
            out = _compact(report, want_offsets=args.offsets,
                           want_bind_table=args.bind_table)
            print(json.dumps(out, indent=2))
            return 0

        print(f"== {os.path.basename(args.model)}  "
              f"used_nodes={report['skeleton']['used_nodes']}")
        for p in report["clusters"]:
            _print_pose(p)
        for r in report["relations"]:
            print(f"   relation c{r['a']} <-> c{r['b']}: shared={r['shared']} "
                  f"centroid_delta_rms={r['centroid_delta_rms']:7.3f} "
                  f"offset_delta_rms={r['offset_delta_rms']:7.3f} "
                  f"bounds_overlap={r['bounds_overlap']}")
        return 0
    except (XmdError, GeometryError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
