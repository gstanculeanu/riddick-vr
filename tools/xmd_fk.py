#!/usr/bin/env python3
"""Offline forward kinematics + skinning for ``MOS DATAFILE2.0`` ``.xmd`` models.

Ten wakes of read-only RE (ADR-0423..0430, ADR-0440, ADR-0441) ended the hunt for
a per-cluster bind/reference transform: there is none. ``MXR.dll`` builds the live
skin matrices by **forward kinematics** over the ``NODES`` skeleton and feeds them
*straight* to the vertex blender. ADR-0441 recovered the exact algebra, so the
pose can be reproduced *offline* -- no running game, no runtime read (GH #34):

  * a bone's **local** map is the affine
    ``v |-> R * (v - bindpos) + bindpos + t`` where ``bindpos`` is the bone's
    ``NODES`` model-space translation and ``R``/``t`` are the ``.xsa`` rotation /
    translation *deltas from bind* (identity / zero at bind), because the engine
    overwrites the local translation with ``bindpos - R*bindpos + t``
    (``FUN_1001f340``);
  * **world** = parent-world . local (3x4 concat, ``FUN_1001db60``);
  * a vertex is the influence **weighted sum** of its bones' world maps
    (``FUN_100254e0``); a normal uses the rotation part only;
  * the world matrices are **bind-relative deltas applied directly to the
    model-space file geometry**: no bind-inverse and no load-time vertex rebase
    sit between the FK output and the blend. At the bind pose every world map is
    the identity, so the blend equals ``position * sum(weights)`` and
    :func:`check` finds zero drift over the whole shipped corpus. (Caveat,
    ADR-0444: that identity is a *property of the recipe*, not a test that the
    stored geometry is the ``NODES`` bind pose -- see the note on :func:`check`.)

This tool turns that recipe into a deterministic, dependency-free evaluator:

  * ``bind``      - FK at the bind pose (every map identity) and the blend/weight
                    diagnostics; it reproduces the file geometry by construction
                    and reports the identity drift (it does not test whether the
                    geometry is the bind pose -- ADR-0424/0444).
  * ``pose``      - FK from a shipped ``.xsa`` clip (``tools/xsa_anim.py``),
                    sampled at a time; the clip's ``rotation``/``movement`` window
                    slots are indexed by each node's ``rotation_slot`` /
                    ``movement_slot`` (pinned in ADR-0422).
  * ``skin``      - the per-vertex weighted blend, exported as OBJ so the posed
                    arm/hand can be inspected offline (GH #46/#58).

Strictly read-only: the game install is never written and no input is
synthesised. This is the offline half of the native skinned hand; the renderer
consumer is GH #46 (ADR-0427/0436) and stays rigid until a validated pose source
is chosen.

Usage::

    python tools/xmd_fk.py --model <file.xmd> --bind --json
    python tools/xmd_fk.py --model <file.xmd> --cluster 1 --joints
    python tools/xmd_fk.py --model <file.xmd> --cluster 1 \
        --anim <file.xsa> --sequence use_pad_rid --time 0.20 --obj out.obj
    python tools/xmd_fk.py --inventory <game>\\Content\\Models --check
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
import xsa_anim  # noqa: E402

DEFAULT_GAME_DIR = r"C:\Program Files (x86)\Starbreeze Studios\Riddick EFBB"
MODELS_SUBDIR = os.path.join("Content", "Models")
SLOT_NONE = 0xFFFF
IDENTITY_QUAT = (0.0, 0.0, 0.0, 1.0)
ZERO_MOVE = (0.0, 0.0, 0.0)

# model-space geometry is 32-bit floats off disk; the bind round-trip is exact
# up to a few ulps of the float64 evaluation here.
BIND_EPS = 1e-4


# --------------------------------------------------------------------------- #
# 3x4 affine math (dependency-free; row-major 3x3 rotation + 3 translation)
# --------------------------------------------------------------------------- #
IDENTITY_MAT34 = ((1.0, 0.0, 0.0,
                   0.0, 1.0, 0.0,
                   0.0, 0.0, 1.0), (0.0, 0.0, 0.0))


def identity_mat34():
    return IDENTITY_MAT34


def quat_to_mat3(q):
    """A unit quaternion ``(x, y, z, w)`` as a row-major 3x3 rotation matrix."""
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    if n <= 1e-12:
        return (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    x, y, z, w = x / n, y / n, z / n, w / n
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return (1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy),
            2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx),
            2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy))


def mat3_mul(a, b):
    """Row-major ``a . b``."""
    return tuple(
        a[3 * r + 0] * b[0 + c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c]
        for r in range(3) for c in range(3))


def mat3_apply(m, v):
    return (m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
            m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2])


def mat3_is_orthonormal(m, tol=1e-5):
    for r in range(3):
        for c in range(r, 3):
            dot = sum(m[3 * k + r] * m[3 * k + c] for k in range(3))
            want = 1.0 if r == c else 0.0
            if abs(dot - want) > tol:
                return False
    return True


def mat34_apply(m, v):
    """Apply a ``(R, T)`` affine to a point: ``R . v + T``."""
    r, t = m
    p = mat3_apply(r, v)
    return (p[0] + t[0], p[1] + t[1], p[2] + t[2])


def mat34_apply_dir(m, v):
    """Apply the rotation part only (for normals / directions)."""
    return mat3_apply(m[0], v)


def mat34_compose(a, b):
    """``a . b``: apply ``b`` then ``a`` (the engine's 3x4 concat)."""
    ra, ta = a
    rb, tb = b
    r = mat3_mul(ra, rb)
    p = mat3_apply(ra, tb)
    return (r, (p[0] + ta[0], p[1] + ta[1], p[2] + ta[2]))


def local_from_bind(bindpos, quat=IDENTITY_QUAT, move=ZERO_MOVE):
    """The engine's bone-local map ``v |-> R*(v - bindpos) + bindpos + t``.

    ``quat``/``move`` are the ``.xsa`` deltas from bind, so the default
    (identity rotation, zero translation) yields the identity map at *any*
    ``bindpos`` -- the bind-relative-delta property of ADR-0441.
    """
    r = quat_to_mat3(quat)
    rb = mat3_apply(r, bindpos)
    t = (bindpos[0] + move[0] - rb[0],
         bindpos[1] + move[1] - rb[1],
         bindpos[2] + move[2] - rb[2])
    return (r, t)


# --------------------------------------------------------------------------- #
# skeleton forward kinematics
# --------------------------------------------------------------------------- #
def world_matrices(skeleton, locals_by_node=None):
    """Per-node world maps ``world[n] = world[parent] . local[n]``.

    ``skeleton`` is a :class:`mos_xmd.Skeleton`; ``locals_by_node`` maps a node
    index to a ``(quat, move)`` delta pair (missing nodes are bind/identity).
    Returns a list indexed by node index (``None`` for skeletal padding nodes).
    """
    nodes = list(skeleton.nodes)
    deltas = locals_by_node or {}
    out: list = [None] * len(nodes)
    busy: set[int] = set()

    def resolve(i):
        if out[i] is not None:
            return out[i]
        if i in busy:
            raise GeometryError(f"skeleton cycle at node {i}")
        busy.add(i)
        node = nodes[i]
        q, mv = deltas.get(i, (IDENTITY_QUAT, ZERO_MOVE))
        local = local_from_bind(node.translation, q, mv)
        parent = node.parent
        if parent != i and 0 <= parent < len(nodes):
            local = mat34_compose(resolve(parent), local)
        out[i] = local
        busy.discard(i)
        return local

    for i in range(len(nodes)):
        resolve(i)
    return out


def bind_worlds(skeleton):
    """The bind pose: every world map identity (ADR-0441)."""
    return world_matrices(skeleton, {})


# --------------------------------------------------------------------------- #
# skinning
# --------------------------------------------------------------------------- #
def skin_positions(positions, skin, worlds):
    """Weighted-sum blend of ``positions`` by ``skin``'s world maps.

    Unskinned clusters are returned unchanged. Weights are used as stored (the
    engine sums the influence maps directly, ADR-0441); :func:`skin_report`
    reports any vertex whose weights do not sum to 1.
    """
    if skin is None:
        return list(positions)
    out = []
    for vi, p in enumerate(positions):
        inf = skin.resolved_influences_of(vi)
        if not inf:
            out.append(tuple(p))
            continue
        sx = sy = sz = 0.0
        for node, w in inf:
            m = worlds[node]
            q = mat34_apply(m, p)
            sx += w * q[0]
            sy += w * q[1]
            sz += w * q[2]
        out.append((sx, sy, sz))
    return out


def skin_normals(normals, skin, worlds):
    """Rotation-only blend of ``normals`` (re-normalised; skipped when absent)."""
    if skin is None or not normals:
        return list(normals) if normals else []
    out = []
    for vi, n in enumerate(normals):
        inf = skin.resolved_influences_of(vi)
        if not inf:
            out.append(tuple(n))
            continue
        sx = sy = sz = 0.0
        for node, w in inf:
            d = mat34_apply_dir(worlds[node], n)
            sx += w * d[0]
            sy += w * d[1]
            sz += w * d[2]
        ln = math.sqrt(sx * sx + sy * sy + sz * sz)
        if ln <= 1e-12:
            out.append(tuple(n))
        else:
            out.append((sx / ln, sy / ln, sz / ln))
    return out


def skin_report(positions, skin, worlds):
    """Diagnostics for one cluster: identity drift, weight sums, displacement.

    ``pose_drift_max`` isolates the *pose* from the weights: at the bind pose
    every world map is the identity, so ``blend == position * sum(weights)`` and
    the residual is zero even for content whose influence weights do not sum to
    one. ``identity_drift_max`` is the plain ``|blend - position|`` and is only
    zero when the weights sum to one *and* the pose is the bind pose.
    """
    blended = skin_positions(positions, skin, worlds)
    identity_drift = 0.0
    pose_drift = 0.0
    disp = 0.0
    bad_weights = 0
    max_weight_error = 0.0
    for vi, (a, b) in enumerate(zip(positions, blended)):
        identity_drift = max(identity_drift, max(abs(a[i] - b[i]) for i in range(3)))
        disp = max(disp, math.dist(a, b))
        sw = 1.0
        if skin is not None:
            inf = skin.resolved_influences_of(vi)
            if inf:
                sw = sum(w for _n, w in inf)
        pose_drift = max(pose_drift, max(abs(a[i] * sw - b[i]) for i in range(3)))
        max_weight_error = max(max_weight_error, abs(sw - 1.0))
        if abs(sw - 1.0) > 1e-3:
            bad_weights += 1
    return {
        "vertices": len(positions),
        "skinned": skin is not None,
        "identity_drift_max": identity_drift,
        "pose_drift_max": pose_drift,
        "displacement_max": disp,
        "weight_error_max": max_weight_error,
        "weight_error_vertices": bad_weights,
    }


# --------------------------------------------------------------------------- #
# pose from a shipped .xsa clip
# --------------------------------------------------------------------------- #
def pose_from_sequence(skeleton, seq, sampler, t=0.0):
    """Bone ``(quat, move)`` deltas for ``seq`` at time ``t``.

    ``sampler`` is a :class:`xsa_anim.AnimSet` (anything exposing
    ``sample_rotation(seq, slot, t)`` / ``sample_move(seq, slot, t)``). Each used
    node listens on its ``rotation_slot`` in the clip's rotation window and its
    ``movement_slot`` in the movement window (ADR-0422). Returns
    ``(deltas_by_node, notes)``.
    """
    notes: list[str] = []
    rot_count = int(getattr(seq, "rotation_count", 0) or 0)
    mov_count = int(getattr(seq, "movement_count", 0) or 0)
    deltas: dict[int, tuple] = {}
    unmapped_rot = unmapped_mov = quat_mov = 0
    for node in skeleton.used_nodes():
        quat = IDENTITY_QUAT
        move = ZERO_MOVE
        rs = node.rotation_slot
        if rs != SLOT_NONE and 0 <= rs < rot_count:
            quat = sampler.sample_rotation(seq, rs, t)
        elif rs != SLOT_NONE:
            unmapped_rot += 1
        ms = node.movement_slot
        if ms != SLOT_NONE and 0 <= ms < mov_count:
            try:
                move = sampler.sample_move(seq, ms, t)
            except ValueError:
                # a movement-window slot whose track routes to QUATERNIONS
                # (TRACK_FLAG_MOVEMENT); the engine's consumer is not pinned, so
                # the delta stays zero and the case is reported (ADR-0442).
                quat_mov += 1
        elif ms != SLOT_NONE:
            unmapped_mov += 1
        deltas[node.index] = (tuple(quat), tuple(move))
    if unmapped_rot:
        notes.append(f"{unmapped_rot} nodes have a rotation slot past the clip's "
                     f"rotation window ({rot_count})")
    if unmapped_mov:
        notes.append(f"{unmapped_mov} nodes have a movement slot past the clip's "
                     f"movement window ({mov_count})")
    if quat_mov:
        notes.append(f"{quat_mov} movement slots carry QUATERNIONS keys "
                     f"(consumer not pinned; treated as zero translation)")
    return deltas, notes


def sample_sequence(skeleton, anim, sequence, t=0.0):
    """Resolve a clip by name (or index/object) and return its pose deltas."""
    if isinstance(sequence, int):
        seq = anim.sequences[sequence]
    elif isinstance(sequence, str):
        seq = anim.sequence_by_name(sequence)
        if seq is None:
            raise XmdError(f"sequence {sequence!r} not found in {anim.path}")
    else:
        seq = sequence
    deltas, notes = pose_from_sequence(skeleton, seq, anim, t)
    return seq, deltas, notes


# --------------------------------------------------------------------------- #
# per-cluster analysis
# --------------------------------------------------------------------------- #
def _bounds(points):
    if not points:
        return [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]
    mn = [min(p[i] for p in points) for i in range(3)]
    mx = [max(p[i] for p in points) for i in range(3)]
    return mn, mx


def cluster_analysis(x, cluster, worlds):
    """Pose one cluster: geometry, blended positions and diagnostics."""
    mesh = xmd_export.decode_cluster(x, cluster)
    blended = skin_positions(mesh.positions, mesh.skin, worlds)
    normals = skin_normals(mesh.normals, mesh.skin, worlds)
    mn, mx = _bounds(blended)
    rep = skin_report(mesh.positions, mesh.skin, worlds)
    rep.update({
        "cluster": cluster,
        "triangles": mesh.tri_count,
        "bones": mesh.bones_used(),
        "bone_space": mesh.bone_space,
        "bounds_min": [round(v, 6) for v in mn],
        "bounds_max": [round(v, 6) for v in mx],
    })
    return rep, mesh, blended, normals


def analyze(x, clusters=None, deltas=None, source=None):
    """Full FK/skin report for one ``.xmd`` at the posed ``deltas``."""
    sk = x.skeleton()
    if sk is None:
        raise GeometryError("model has no NODES skeleton")
    worlds = world_matrices(sk, deltas)
    nclusters = len(xmd_export.vertex_frame_nodes(x))
    if clusters is None:
        clusters = list(range(nclusters))
    out = []
    for c in clusters:
        rep, _mesh, _b, _n = cluster_analysis(x, c, worlds)
        out.append(rep)
    return {
        "source": os.path.abspath(x.path),
        "coordinate_system": xmd_export.COORDINATE_SYSTEM,
        "pose": source or "bind",
        "skeleton": {
            "nodes": len(sk.nodes),
            "used_nodes": len(sk.used_nodes()),
            "max_rotation_slot": sk.max_rotation_slot(),
            "max_movement_slot": sk.max_movement_slot(),
        },
        "clusters": out,
    }


def joints(skeleton, deltas=None):
    """Per-node bind translation + posed world origin (a joint readout)."""
    worlds = world_matrices(skeleton, deltas)
    rows = []
    for n in skeleton.used_nodes():
        origin = mat34_apply(worlds[n.index], n.translation)
        rows.append({
            "node": n.index,
            "parent": n.parent,
            "rotation_slot": n.rotation_slot,
            "movement_slot": n.movement_slot,
            "bind": [round(v, 5) for v in n.translation],
            "posed": [round(v, 5) for v in origin],
            "is_bone": bool(skeleton.children_of(n.index)),
        })
    return rows


# --------------------------------------------------------------------------- #
# corpus check: the bind pose is the file geometry (ADR-0441)
# --------------------------------------------------------------------------- #
def _iter_models(root):
    for dirpath, _dirs, files in os.walk(root):
        for name in sorted(files):
            if name.lower().endswith((".xmd", ".xmg")):
                yield os.path.join(dirpath, name)


def check(root):
    """Verify the bind FK is the identity on every skinned cluster under ``root``.

    This makes ADR-0441's algebra falsifiable over the shipped corpus: with every
    bone delta at identity, ``world == local == identity`` and the blend collapses
    to ``position * sum(weights)``, so a non-zero ``pose_drift`` would mean the FK
    or the weighting is wrong.

    Caveat (ADR-0444): a *zero* drift does **not** prove the stored geometry is
    the ``NODES`` bind pose. The identity holds for any input geometry, so this
    check cannot detect a cluster stored in a different reference pose (ADR-0424
    measured exactly that for the Riddick arms). It tests the recipe, not the
    content."""
    problems: list[str] = []
    totals = {"models": 0, "clusters": 0, "skinned_clusters": 0,
              "vertices": 0, "worst_pose_drift": 0.0, "worst_weight_error": 0.0}
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
        worlds = bind_worlds(sk)
        for c in range(len(xmd_export.vertex_frame_nodes(x))):
            try:
                rep, _mesh, _b, _n = cluster_analysis(x, c, worlds)
            except (XmdError, GeometryError) as e:
                problems.append(f"{path} cluster {c}: {e}")
                continue
            totals["clusters"] += 1
            totals["vertices"] += rep["vertices"]
            if rep["skinned"]:
                totals["skinned_clusters"] += 1
            totals["worst_pose_drift"] = max(totals["worst_pose_drift"],
                                             rep["pose_drift_max"])
            totals["worst_weight_error"] = max(totals["worst_weight_error"],
                                               rep["weight_error_max"])
            if rep["pose_drift_max"] > BIND_EPS:
                problems.append(
                    f"{path} cluster {c}: bind FK drifts "
                    f"{rep['pose_drift_max']:.3g} from the file geometry "
                    f"(not the bind pose)")
    return problems, totals


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _load_deltas(x, args):
    sk = x.skeleton()
    if not args.anim:
        return None, "bind", []
    anim = xsa_anim.AnimSet(args.anim)
    if not anim.has_anim:
        raise XmdError(f"{args.anim} is not an animation set")
    seq, deltas, notes = sample_sequence(sk, anim, args.sequence, args.time)
    label = f"{os.path.basename(args.anim)}:{seq.name}@{args.time:g}s"
    return deltas, label, notes


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME_DIR,
                    help="game install root (default: %(default)s)")
    ap.add_argument("--model", metavar="FILE", help="analyze one .xmd")
    ap.add_argument("--cluster", type=int, action="append", metavar="N",
                    help="restrict to cluster N (repeatable; with --model)")
    ap.add_argument("--anim", metavar="FILE",
                    help="pose from this .xsa instead of the bind pose")
    ap.add_argument("--sequence", metavar="NAME",
                    help="clip name (or index) inside --anim")
    ap.add_argument("--time", type=float, default=0.0,
                    help="clip time in seconds (default: %(default)s)")
    ap.add_argument("--joints", action="store_true",
                    help="print the per-node bind/posed joint table")
    ap.add_argument("--obj", metavar="FILE",
                    help="export the posed cluster(s) as Wavefront OBJ")
    ap.add_argument("--inventory", metavar="DIR",
                    help="run --check over every model under DIR")
    ap.add_argument("--check", action="store_true",
                    help="validate invariants (with --inventory or --model)")
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
                print(f"== xmd_fk --check {root}")
                print(f"   models={totals['models']} clusters={totals['clusters']} "
                      f"skinned={totals['skinned_clusters']} "
                      f"vertices={totals['vertices']}")
                print(f"   worst bind pose drift="
                      f"{totals['worst_pose_drift']:.3g} "
                      f"worst weight error={totals['worst_weight_error']:.3g}")
                print(f"   problems: {len(problems)}")
                for p in problems[:50]:
                    print(f"     FAIL {p}")
            return 1 if problems else 0

        if not args.model:
            ap.error("--model or --inventory is required")
        if args.anim and not args.sequence:
            ap.error("--anim requires --sequence")

        x = Xmd(args.model)

        deltas, label, notes = _load_deltas(x, args)

        if args.check:
            sk = x.skeleton()
            if sk is None:
                print("ERROR: no skeleton", file=sys.stderr)
                return 1
            problems = []
            worlds = world_matrices(sk, deltas)
            nclusters = len(xmd_export.vertex_frame_nodes(x))
            for c in (args.cluster or range(nclusters)):
                rep, _m, _b, _n = cluster_analysis(x, c, worlds)
                if rep["pose_drift_max"] > BIND_EPS and not args.anim:
                    problems.append(f"cluster {c}: bind drift "
                                    f"{rep['pose_drift_max']:.3g}")
            if args.json:
                print(json.dumps({"pose": label, "problems": problems}, indent=2))
            else:
                print(f"== xmd_fk --check {os.path.basename(args.model)} "
                      f"[{label}]")
                print(f"   problems: {len(problems)}")
                for p in problems:
                    print(f"     FAIL {p}")
            return 1 if problems else 0

        if args.joints:
            for row in joints(x.skeleton(), deltas):
                print(f"node {row['node']:3d} parent={row['parent']:3d} "
                      f"rot={row['rotation_slot']:5d} mov={row['movement_slot']:5d} "
                      f"bind=({row['bind'][0]:8.3f},{row['bind'][1]:8.3f},"
                      f"{row['bind'][2]:8.3f}) "
                      f"posed=({row['posed'][0]:8.3f},{row['posed'][1]:8.3f},"
                      f"{row['posed'][2]:8.3f})")
            return 0

        report = analyze(x, args.cluster, deltas, label)

        if args.obj:
            sk = x.skeleton()
            if sk is None:
                print("ERROR: no skeleton", file=sys.stderr)
                return 1
            worlds = world_matrices(sk, deltas)
            chunks = []
            for rep_c in (args.cluster or range(len(report["clusters"]))):
                rep, mesh, blended, normals = cluster_analysis(x, rep_c, worlds)
                mn, mx = _bounds(blended)
                posed = xmd_export.ClusterMesh(
                    mesh.index, mesh.frames, blended, normals, mesh.uvs,
                    mesh.triangles, mesh.skin, tuple(mn), tuple(mx))
                chunks.append(xmd_export.mesh_obj(
                    posed, f"{os.path.splitext(os.path.basename(args.model))[0]}"
                           f"_c{rep_c}_{label}"))
            with open(args.obj, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(chunks))
            print(f"wrote {args.obj} ({len(chunks)} cluster(s), pose {label})")

        if args.json:
            out = dict(report)
            if notes:
                out["notes"] = notes
            print(json.dumps(out, indent=2))
        elif not args.obj:
            print(f"== {os.path.basename(args.model)}  pose={label}  "
                  f"nodes={report['skeleton']['used_nodes']}")
            for c in report["clusters"]:
                print(f"   cluster {c['cluster']:2d}: verts={c['vertices']:5d} "
                      f"tris={c['triangles']:5d} space={c['bone_space']:7s} "
                      f"bones={len(c['bones']):3d} "
                      f"drift={c['pose_drift_max']:9.3g} "
                      f"disp={c['displacement_max']:9.3g}")
            for n in notes:
                print(f"   note: {n}")
        return 0
    except (XmdError, GeometryError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
