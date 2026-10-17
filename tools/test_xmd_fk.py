#!/usr/bin/env python3
"""Self-contained test for ``tools/xmd_fk.py``.

Builds synthetic ``MOS DATAFILE2.0`` containers (no game install, no input
synthesis) and asserts the offline forward-kinematics / skinning recipe of
ADR-0441 is faithful:

  * the bind pose is the identity on every bone (so the blend reproduces the file
    geometry by construction; this does *not* test that the stored geometry is
    the ``NODES`` bind pose -- ADR-0424/0444);
  * a bone-local map rotates about the bone's own ``NODES`` translation and adds
    the ``.xsa`` translation displacement directly;
  * ``world = parent . local`` composes down the tree;
  * skinning is the influence weighted sum (rotation-only for normals);
  * a clip's ``rotation``/``movement`` window slots map to a node's
    ``rotation_slot`` / ``movement_slot`` (ADR-0422), with the QUATERNIONS-pool
    movement case reported rather than silent.

Run headlessly:  python tools/test_xmd_fk.py
Exit code 0 = pass, 1 = fail.
"""
from __future__ import annotations

import contextlib
import inspect
import io
import math
import os
import struct
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import mos_xmd  # noqa: E402
import xmd_export  # noqa: E402
import xmd_fk  # noqa: E402
import test_mos_xmd as tm  # noqa: E402
import test_xmd_export as te  # noqa: E402


def _approx(a, b, tol=1e-9):
    return abs(a - b) <= tol


def _vec_approx(a, b, tol=1e-6):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


# --------------------------------------------------------------------------- #
# synthetic skeleton with real bind translations (NODES + NODEINDICES only)
# --------------------------------------------------------------------------- #
# node: (translation, parent, child_start, child_count, rotation_slot, movement_slot)
_TREE = [
    ((0.0, 0.0, 0.0), 0, 0, 1, 0, 0),
    ((0.0, 0.0, 10.0), 0, 1, 1, 1, 1),
    ((0.0, 5.0, 20.0), 1, 2, 1, 2, mos_xmd.NODE_SLOT_NONE),
    ((3.0, 5.0, 20.0), 2, 3, 0, 3, mos_xmd.NODE_SLOT_NONE),
]


def _tree_container() -> bytes:
    recs = [dict(translation=t, parent=p, child_start=cs, child_count=cc,
                 rotation_slot=rs, movement_slot=ms,
                 uc=0, scale=(1.0, 1.0))
            for (t, p, cs, cc, rs, ms) in _TREE]
    nodes_payload = tm._nodes_payload(recs)
    indices = tm._node_indices([1, 2, 3])
    stride = mos_xmd.NODE_RECORD_LEN
    nodes = [
        ("NODES", stride * 2, 0, len(nodes_payload), len(recs), 0),
        ("NODEINDICES", 0, 0, len(indices), len(indices) // 2, 0),
    ]
    return tm.build_container(nodes, [nodes_payload, indices])


def _load_tree(tmp_dir):
    path = tm._write_tmp(_tree_container())
    return mos_xmd.Xmd(path), path


def _flat_nodes_translated(n):
    """A flat skeleton (node 0 root, ``n-1`` children) with bind translations."""
    recs = [dict(rotation_slot=0, movement_slot=0, child_start=0,
                 child_count=n - 1, parent=0, translation=(0.0, 0.0, 0.0))]
    for i in range(1, n):
        recs.append(dict(rotation_slot=i, movement_slot=mos_xmd.NODE_SLOT_NONE,
                         parent=0,
                         translation=(float(i), float(-i), 10.0 * i)))
    return tm._nodes_payload(recs)


def _translated_cluster(bones=8) -> bytes:
    """A skinned single cluster whose bones bind at non-zero translations.

    The influence weights sum to exactly 1 for every vertex, so the bind pose
    must reproduce the file geometry *bit for bit* -- ADR-0441's claim."""
    verts = [(0, 0, 0), (65535, 0, 0), (0, 65535, 0), (0, 0, 65535)]
    normals = [(127, 0, 0), (0, 127, 0), (0, 0, -127), (0, 0, 0)]
    vf = tm._vf204_frame(verts, normals)
    tri = tm._triangles([0, 1, 2, 0, 2, 3])
    tv = tm._tv200_frame([(0, 0), (65535, 0), (0, 65535), (65535, 65535)])
    vrecs = [(0, 1, 0), (0, 2, 1), (0, 1, 3), (0, 1, 4)]
    infl = [(0, 255), (1, 255), (2, 0), (3, 255), (5, 255)]
    bd = tm._bonedeform(vrecs, infl, matrix_map_id=-1)
    nodes_payload = _flat_nodes_translated(bones)
    indices = tm._node_indices(range(1, bones))
    stride = mos_xmd.NODE_RECORD_LEN
    nodes = [
        ("VERTEXFRAMES", stride * 2, 0, len(vf), 1, 0),
        ("TVERTEXFRAMES", stride * 3, 0, len(tv), 1, 0x201),
        ("BONEDEFORM", stride * 4, 0, len(bd), 0, 0),
        ("TRIANGLES", stride * 5, 0, len(tri), 2, 0),
        ("NODES", stride * 6, 0, len(nodes_payload), bones, 0),
        ("NODEINDICES", 0, 0, len(indices), len(indices) // 2, 0),
    ]
    return tm.build_container(nodes, [vf, tv, bd, tri, nodes_payload, indices])


def _bindpos(sk, i):
    return sk.nodes[i].translation


# --------------------------------------------------------------------------- #
# tests
# --------------------------------------------------------------------------- #
def test_quat_and_mat3():
    assert xmd_fk.quat_to_mat3((0.0, 0.0, 0.0, 1.0)) == (1.0, 0.0, 0.0, 0.0, 1.0,
                                                          0.0, 0.0, 0.0, 1.0)
    # 90 degrees about +Z: (1,0,0) -> (0,1,0).
    h = math.sqrt(0.5)
    rz = xmd_fk.quat_to_mat3((0.0, 0.0, h, h))
    assert _vec_approx(xmd_fk.mat3_apply(rz, (1.0, 0.0, 0.0)), (0.0, 1.0, 0.0))
    assert xmd_fk.mat3_is_orthonormal(rz)
    # A non-unit quaternion is normalised, not rejected.
    assert _vec_approx(xmd_fk.mat3_apply(
        xmd_fk.quat_to_mat3((0.0, 0.0, 3.0, 3.0)), (1.0, 0.0, 0.0)),
        (0.0, 1.0, 0.0))
    # Degenerate quaternion -> identity (no NaN).
    assert xmd_fk.quat_to_mat3((0.0, 0.0, 0.0, 0.0))[0] == 1.0
    assert xmd_fk.mat3_mul(rz, xmd_fk.quat_to_mat3((0.0, 0.0, 0.0, 1.0))) == rz


def test_local_from_bind():
    bp = (1.0, -2.0, 30.0)
    # identity / zero at bind for any bind position
    r, t = xmd_fk.local_from_bind(bp)
    assert r == (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    assert _vec_approx(t, (0.0, 0.0, 0.0))
    # 180 degrees about +Z about the bone's own bind position
    m = xmd_fk.local_from_bind(bp, (0.0, 0.0, 1.0, 0.0))
    assert _vec_approx(xmd_fk.mat34_apply(m, bp), bp)
    assert _vec_approx(xmd_fk.mat34_apply(m, (bp[0] + 1.0, bp[1], bp[2])),
                       (bp[0] - 1.0, bp[1], bp[2]))
    # a translation delta is added directly (no rotation)
    m = xmd_fk.local_from_bind(bp, move=(4.0, 0.0, -1.0))
    assert _vec_approx(xmd_fk.mat34_apply(m, bp),
                       (bp[0] + 4.0, bp[1], bp[2] - 1.0))
    assert _vec_approx(m[1], (4.0, 0.0, -1.0))


def test_world_composition(tmp_dir):
    x, path = _load_tree(tmp_dir)
    try:
        sk = x.skeleton()
        # A pure translation delta on the root moves every descendant by it,
        # because the child world composes parent . local.
        d = (0.0, 0.0, 7.0)
        worlds = xmd_fk.world_matrices(sk, {0: (xmd_fk.IDENTITY_QUAT, d)})
        for i in (1, 2, 3):
            got = xmd_fk.mat34_apply(worlds[i], _bindpos(sk, i))
            want = tuple(_bindpos(sk, i)[k] + d[k] for k in range(3))
            assert _vec_approx(got, want, 1e-6), (i, got, want)
        # Translation deltas accumulate down the chain: node 2's own delta adds
        # on top of the root's.
        worlds = xmd_fk.world_matrices(
            sk, {0: (xmd_fk.IDENTITY_QUAT, (0.0, 0.0, 7.0)),
                 2: (xmd_fk.IDENTITY_QUAT, (1.0, 0.0, 0.0))})
        got = xmd_fk.mat34_apply(worlds[3], _bindpos(sk, 3))
        want = (_bindpos(sk, 3)[0] + 1.0, _bindpos(sk, 3)[1],
                _bindpos(sk, 3)[2] + 7.0)
        assert _vec_approx(got, want, 1e-6), (got, want)
        # A rotation on the root rotates a descendant about the root's position.
        worlds = xmd_fk.world_matrices(
            sk, {0: ((0.0, 0.0, 1.0, 0.0), (0.0, 0.0, 0.0))})
        got = xmd_fk.mat34_apply(worlds[1], _bindpos(sk, 1))
        # node 1 binds at (0,0,10); 180 about +Z keeps x,y and z, so it is fixed
        # only if it lies on the axis -- here it does (x=y=0).
        assert _vec_approx(got, (0.0, 0.0, 10.0), 1e-6), got
        # A node with no delta present resolves to the identity map.
        assert worlds[2] is not None
    finally:
        os.remove(path)


def test_bind_is_identity_end_to_end(tmp_dir):
    path = tm._write_tmp(_translated_cluster())
    try:
        x = mos_xmd.Xmd(path)
        sk = x.skeleton()
        # the bind FK is the identity on every bone, whatever its translation
        worlds = xmd_fk.bind_worlds(sk)
        for n in sk.used_nodes():
            assert worlds[n.index] == xmd_fk.IDENTITY_MAT34, n.index
        mesh = xmd_export.decode_cluster(x, 0)
        blended = xmd_fk.skin_positions(mesh.positions, mesh.skin, worlds)
        for a, b in zip(mesh.positions, blended):
            assert _vec_approx(a, b, 1e-12), (a, b)
        rep = xmd_fk.skin_report(mesh.positions, mesh.skin, worlds)
        assert rep["identity_drift_max"] < 1e-12, rep
        assert rep["weight_error_vertices"] == 0, rep
        assert rep["skinned"] is True
        # the bounds are the file bounds at bind
        mn, mx = xmd_export.decode_cluster(x, 0).bounds_min, \
            xmd_export.decode_cluster(x, 0).bounds_max
        bmn = [min(p[i] for p in blended) for i in range(3)]
        bmx = [max(p[i] for p in blended) for i in range(3)]
        assert _vec_approx(tuple(bmn), mn, 1e-9)
        assert _vec_approx(tuple(bmx), mx, 1e-9)
    finally:
        os.remove(path)


def test_bind_check_is_tautological(tmp_dir):
    """GH #64/ADR-0444: the bind FK check cannot test whether the geometry is
    the bind pose. At bind every world map is the identity, so the blend equals
    ``position * sum(weights)`` for *any* input geometry and ``pose_drift`` is
    identically zero -- a cluster whose geometry is NOT at the ``NODES`` bind
    pose still "passes"."""
    path = tm._write_tmp(_translated_cluster())
    try:
        x = mos_xmd.Xmd(path)
        sk = x.skeleton()
        worlds = xmd_fk.bind_worlds(sk)
        for n in sk.used_nodes():
            assert worlds[n.index] == xmd_fk.IDENTITY_MAT34, n.index
        mesh = xmd_export.decode_cluster(x, 0)
        # Shift the geometry far away from the bones' bind positions: this is a
        # cluster stored in a *different* reference pose (as ADR-0424 measured
        # for the Riddick arm clusters).
        shifted = [(p[0] + 37.0, p[1] - 12.0, p[2] + 9.0) for p in mesh.positions]
        # The shift is genuinely non-bind: every vertex is far from the nearest
        # bone's bind translation.
        binds = [sk.nodes[n.index].translation for n in sk.used_nodes()]
        nearest = min(
            min(math.dist(v, b) for b in binds) for v in shifted)
        assert nearest > 5.0, nearest
        # ... yet the bind FK "check" still reports zero pose drift, because at
        # bind the blend equals ``position * sum(weights)`` for any geometry.
        rep = xmd_fk.skin_report(shifted, mesh.skin, worlds)
        assert rep["pose_drift_max"] < 1e-12, rep
        # So ``check()``'s criterion would not flag this non-bind geometry.
        assert not (rep["pose_drift_max"] > xmd_fk.BIND_EPS), rep
    finally:
        os.remove(path)


def test_pose_rotates_about_bind(tmp_dir):
    path = tm._write_tmp(_translated_cluster())
    try:
        x = mos_xmd.Xmd(path)
        sk = x.skeleton()
        mesh = xmd_export.decode_cluster(x, 0)
        # 90 deg about +Z on the root: every child inherits it, and the root
        # binds at the origin, so the whole mesh maps by that rotation.
        h = math.sqrt(0.5)
        worlds = xmd_fk.world_matrices(sk, {0: ((0.0, 0.0, h, h),
                                               (0.0, 0.0, 0.0))})
        blended = xmd_fk.skin_positions(mesh.positions, mesh.skin, worlds)
        for a, b in zip(mesh.positions, blended):
            want = (-a[1], a[0], a[2])
            assert _vec_approx(b, want, 1e-9), (a, b, want)
    finally:
        os.remove(path)


def test_skin_weighted_sum(tmp_dir):
    path = tm._write_tmp(te._single_cluster())
    try:
        x = mos_xmd.Xmd(path)
        sk = x.skeleton()
        mesh = xmd_export.decode_cluster(x, 0)
        # A translation delta on the root shifts every vertex by it, because
        # every bone is a descendant of node 0 in this flat skeleton.
        d = (2.0, -1.0, 0.5)
        worlds = xmd_fk.world_matrices(sk, {0: (xmd_fk.IDENTITY_QUAT, d)})
        blended = xmd_fk.skin_positions(mesh.positions, mesh.skin, worlds)
        for vi, a in enumerate(mesh.positions):
            sw = sum(w for _n, w in mesh.skin.resolved_influences_of(vi))
            want = tuple(sw * (a[k] + d[k]) for k in range(3))
            assert _vec_approx(blended[vi], want, 1e-9), (a, blended[vi], want)
        # This fixture has two vertices whose influence weights do NOT sum to 1
        # (q 128 -> 0.502), so the blend is the weighted SUM, not an average:
        # report flags them rather than silently renormalising.
        rep = xmd_fk.skin_report(mesh.positions, mesh.skin, worlds)
        assert rep["weight_error_vertices"] == 2, rep
        # A vertex blended 0.5/0.5 between +x and -x maps to 0.
        A = (xmd_fk.IDENTITY_MAT34[0], (1.0, 0.0, 0.0))
        B = (xmd_fk.IDENTITY_MAT34[0], (-1.0, 0.0, 0.0))
        blended = xmd_fk.skin_positions([(0.0, 0.0, 0.0)], _HalfSkin(), [A, B])
        assert _vec_approx(blended[0], (0.0, 0.0, 0.0), 1e-12)
        # Normals use the rotation part only (translation ignored).
        n = xmd_fk.skin_normals([(0.0, 0.0, 1.0)], _HalfSkin(), [A, B])
        assert _vec_approx(n[0], (0.0, 0.0, 1.0), 1e-9)
    finally:
        os.remove(path)


class _HalfSkin:
    """A two-influence skin: node 0 weight 0.5, node 1 weight 0.5."""

    def resolved_influences_of(self, vi):
        return [(0, 0.5), (1, 0.5)]


class _FakeNode:
    def __init__(self, index, parent, rotation_slot, movement_slot,
                 translation=(0.0, 0.0, 0.0)):
        self.index = index
        self.parent = parent
        self.rotation_slot = rotation_slot
        self.movement_slot = movement_slot
        self.translation = translation


class _FakeSeq:
    name = "fake"
    rotation_count = 3
    movement_count = 2


class _FakeSampler:
    """Rotation slots return +90 deg about Z; movement slot 0 returns +x."""

    def sample_rotation(self, seq, slot, t):
        h = math.sqrt(0.5)
        return (0.0, 0.0, h, h) if slot == 0 else (0.0, 0.0, 0.0, 1.0)

    def sample_move(self, seq, slot, t):
        if slot == 1:
            # the TRACK_FLAG_MOVEMENT case: the pool is QUATERNIONS, not MOVES
            raise ValueError("movement slot carries QUATERNIONS keys")
        return (1.0, 0.0, 0.0)


class _FakeSkeleton:
    def used_nodes(self):
        return [
            _FakeNode(0, 0, 0, 0),
            _FakeNode(1, 0, 1, 1),
            _FakeNode(2, 1, 5, mos_xmd.NODE_SLOT_NONE),  # rot slot past window
            _FakeNode(3, 2, 2, 9),                        # mov slot past window
        ]


def test_pose_from_sequence():
    deltas, notes = xmd_fk.pose_from_sequence(_FakeSkeleton(), _FakeSeq(),
                                              _FakeSampler(), 0.0)
    h = math.sqrt(0.5)
    assert deltas[0][0] == (0.0, 0.0, h, h), deltas[0]
    assert deltas[0][1] == (1.0, 0.0, 0.0), deltas[0]
    assert deltas[1][0] == (0.0, 0.0, 0.0, 1.0)
    # node 1's movement slot 1 raises -> zero translation + a reported note
    assert deltas[1][1] == (0.0, 0.0, 0.0), deltas[1]
    # node 2: rotation slot 5 is outside the clip's 3-slot rotation window
    assert deltas[2][0] == (0.0, 0.0, 0.0, 1.0)
    # node 3: movement slot 9 is outside the clip's 2-slot movement window
    assert deltas[3][1] == (0.0, 0.0, 0.0)
    joined = " | ".join(notes)
    assert "rotation window" in joined, notes
    assert "movement window" in joined, notes
    assert "QUATERNIONS" in joined, notes


def test_check_skips_boxtree_prop(tmp_dir):
    # GH #84: a static prop whose only NODES is a 12-byte BOXTREE chunk is
    # skipped (no skeleton) instead of aborting the whole corpus walk.
    d = os.path.join(tmp_dir, "boxtree-corpus")
    os.makedirs(d, exist_ok=True)
    skel = tm._write_tmp(te._single_cluster(), d)
    prop = tm._write_tmp(tm._boxtree_prop_container(), d)
    try:
        problems, totals = xmd_fk.check(d)
        assert problems == [], problems
        assert totals["models"] == 1, totals
    finally:
        os.remove(skel)
        os.remove(prop)


def test_cli(tmp_dir):
    path = tm._write_tmp(te._single_cluster())
    obj = os.path.join(tmp_dir, "posed.obj")
    try:
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            assert xmd_fk.main(["--model", path, "--json"]) == 0
            assert xmd_fk.main(["--model", path, "--joints"]) == 0
            assert xmd_fk.main(["--model", path, "--check"]) == 0
            assert xmd_fk.main(["--model", path, "--obj", obj]) == 0
            root = os.path.dirname(path)
            assert xmd_fk.main(["--inventory", root, "--check"]) == 0
            assert xmd_fk.main(["--inventory", os.path.join(root, "nope"),
                                "--check"]) == 1
        assert "coordinate_system" in buf.getvalue()
        assert os.path.isfile(obj)
        text = open(obj, encoding="utf-8").read()
        assert text.count("v ") == 4 and "f " in text
    finally:
        os.remove(path)


def test_cli_requires_input(tmp_dir):
    del tmp_dir
    for args in ([], ["--model", "x", "--anim", "a"]):
        try:
            xmd_fk.main(args)
        except SystemExit as e:
            assert e.code == 2, (args, e.code)
        else:
            raise AssertionError(f"expected SystemExit for {args}")


def test_cli_anim_errors(tmp_dir):
    path = tm._write_tmp(te._single_cluster())
    try:
        # a non-animation file is rejected, not a crash
        assert xmd_fk.main(["--model", path, "--anim", path,
                            "--sequence", "nope"]) == 1
    finally:
        os.remove(path)


def main() -> int:
    tests = (test_quat_and_mat3, test_local_from_bind, test_world_composition,
             test_bind_is_identity_end_to_end, test_bind_check_is_tautological,
             test_pose_rotates_about_bind,
             test_skin_weighted_sum, test_pose_from_sequence,
             test_check_skips_boxtree_prop, test_cli,
             test_cli_requires_input, test_cli_anim_errors)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            if inspect.signature(t).parameters:
                t(d)
            else:
                t()
            print(f"ok   {t.__name__}")
    print("PASS xmd_fk tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
