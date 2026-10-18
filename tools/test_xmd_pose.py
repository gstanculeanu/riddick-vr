#!/usr/bin/env python3
"""Self-contained test for ``tools/xmd_pose.py``.

Builds synthetic ``MOS DATAFILE2.0`` containers in a temp directory (no game
install, no input synthesis) and asserts the reference-pose analysis is
faithful: weighted bone centroids, the bind table, per-cluster bind offsets,
cluster relations and the corpus ``--check`` invariants.

Run headlessly:  python tools/test_xmd_pose.py
Exit code 0 = pass, 1 = fail.
"""
from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import mos_xmd  # noqa: E402
import xmd_pose  # noqa: E402
import xmd_export  # noqa: E402
import test_mos_xmd as tm  # noqa: E402
import test_xmd_export as te  # noqa: E402


def test_bone_centroids_and_pose(tmp_dir):
    path = tm._write_tmp(te._single_cluster())
    try:
        x = mos_xmd.Xmd(path)
        mesh = xmd_export.decode_cluster(x, 0)
        cents = xmd_pose.bone_centroids(mesh)
        # v0 -> bone0 (w 1.0), v1 -> bones 1,2, v2 -> bone3, v3 -> bone5
        assert abs(cents[0]["centroid"][0]) < 1e-3
        assert abs(cents[1]["centroid"][0] - 100.0) < 1e-2, cents[1]
        assert abs(cents[2]["centroid"][0] - 100.0) < 1e-2, cents[2]
        assert abs(cents[2]["weight"] - 1.0) < 1e-3, cents[2]
        assert abs(cents[3]["centroid"][1] - 200.0) < 1e-2, cents[3]
        assert abs(cents[5]["centroid"][2] - 300.0) < 1e-2, cents[5]

        pose = xmd_pose.cluster_pose(x, 0)
        # The synthetic flat skeleton has every bind translation at the origin,
        # so each bind offset equals the geometry centroid.
        by_node = {o["node"]: o for o in pose["offsets"]}
        assert abs(by_node[5]["distance"] - 300.0) < 1e-2, by_node[5]
        assert pose["skinned"] is True
        assert pose["bone_space"] == "node"
        assert pose["bind_offset_max"] >= 299.9
        assert set(pose["bones"]) == {0, 1, 2, 3, 5}, pose["bones"]
    finally:
        os.remove(path)


def _three_node_skeleton() -> bytes:
    recs = [
        dict(rotation_slot=0, movement_slot=0, child_start=0, child_count=1,
             parent=0, translation=(0.0, 0.0, 0.0)),
        dict(rotation_slot=1, child_start=1, child_count=1, parent=0,
             translation=(0.0, 0.0, 10.0)),
        dict(rotation_slot=2, child_start=0, child_count=0, parent=1,
             translation=(0.0, 0.0, 20.0)),
    ]
    payload = tm._nodes_payload(recs)
    indices = tm._node_indices([1, 2])
    return tm._skeleton_container(payload, 3, indices)


def test_bind_table_tree(tmp_dir):
    path = tm._write_tmp(_three_node_skeleton())
    try:
        x = mos_xmd.Xmd(path)
        table = {t["index"]: t for t in xmd_pose.bind_table(x.skeleton())}
        assert set(table) == {0, 1, 2}, table
        assert table[0]["children"] == [1] and table[1]["children"] == [2]
        assert table[2]["is_leaf"] is True and table[1]["is_leaf"] is False
        # local offset is the model-space delta to the parent.
        assert table[1]["local_offset"] == [0.0, 0.0, 10.0], table[1]
        assert table[2]["local_offset"] == [0.0, 0.0, 10.0], table[2]
        # bone axis points from the node toward its children (Z-up here).
        assert table[1]["bind_axis"] == [0.0, 0.0, 1.0], table[1]
        assert table[2]["bind_axis"] == [0.0, 0.0, 1.0], table[2]
    finally:
        os.remove(path)


def test_cluster_relation_same_and_shifted(tmp_dir):
    def pose(cluster, offset, mn, mx):
        return {
            "cluster": cluster,
            "offsets": [{"node": 1, "offset": [offset, 0.0, 0.0],
                         "centroid": [offset, 0.0, 0.0]},
                        {"node": 2, "offset": [offset, 5.0, 0.0],
                         "centroid": [offset, 5.0, 0.0]}],
            "bounds_min": list(mn), "bounds_max": list(mx),
        }

    a = pose(0, 0.0, (0, 0, 0), (1, 1, 1))
    same = xmd_pose.cluster_relation(a, pose(1, 0.0, (0.5, 0, 0), (1.5, 1, 1)))
    assert same["shared"] == 2
    assert same["offset_delta_rms"] == 0.0, same
    assert same["bounds_overlap"] is True, same

    shifted = xmd_pose.cluster_relation(a, pose(2, 25.0, (9, 9, 9), (10, 10, 10)))
    assert shifted["shared"] == 2
    assert abs(shifted["offset_delta_rms"] - 25.0) < 1e-9, shifted
    assert shifted["bounds_overlap"] is False, shifted

    none = xmd_pose.cluster_relation(
        a, {"cluster": 3, "offsets": [{"node": 9, "offset": [0, 0, 0],
                                       "centroid": [0, 0, 0]}],
            "bounds_min": [0, 0, 0], "bounds_max": [1, 1, 1]})
    assert none["shared"] == 0 and none["offset_delta_rms"] == 0.0, none


def test_analyze_relations(tmp_dir):
    path = tm._write_tmp(te._single_cluster())
    try:
        x = mos_xmd.Xmd(path)
        report = xmd_pose.analyze(x)
        assert report["skeleton"]["used_nodes"] == 8, report["skeleton"]
        assert len(report["clusters"]) == 1
        assert report["relations"] == []  # a single cluster has no relation
        assert report["coordinate_system"].startswith("engine model space")
        assert len(report["bind_table"]) == 8
    finally:
        os.remove(path)


def test_check_clean_and_empty(tmp_dir):
    path = tm._write_tmp(te._single_cluster())
    try:
        problems, totals = xmd_pose.check(os.path.dirname(path))
        assert problems == [], problems
        assert totals["models"] == 1 and totals["clusters"] == 1, totals
        assert totals["skinned_clusters"] == 1, totals
    finally:
        os.remove(path)
    empty = os.path.join(tmp_dir, "nodir-models")
    os.makedirs(empty, exist_ok=True)
    problems, totals = xmd_pose.check(empty)
    assert problems == [] and totals["models"] == 0, (problems, totals)


def test_check_skips_boxtree_prop(tmp_dir):
    # GH #84: a static prop whose only NODES is a 12-byte BOXTREE chunk is
    # skipped (no skeleton) instead of aborting the whole corpus walk.
    d = os.path.join(tmp_dir, "boxtree-corpus")
    os.makedirs(d, exist_ok=True)
    skel = tm._write_tmp(te._single_cluster(), d)
    prop = tm._write_tmp(tm._boxtree_prop_container(), d)
    try:
        problems, totals = xmd_pose.check(d)
        assert problems == [], problems
        assert totals["models"] == 1 and totals["clusters"] == 1, totals
    finally:
        os.remove(skel)
        os.remove(prop)


def test_check_pairs_boxtree_triangles(tmp_dir):
    # GH #86: a BOXTREE leaf TRIANGLES (a different run layout, not a mesh
    # index list) must not be paired with a cluster — doing so made the corpus
    # check red across 19 files of the 2009 install.
    d = os.path.join(tmp_dir, "boxtree-tri-corpus")
    os.makedirs(d, exist_ok=True)
    path = tm._write_tmp(tm._boxtree_triangles_container(), d)
    try:
        problems, totals = xmd_pose.check(d)
        assert problems == [], problems
        assert totals["models"] == 1 and totals["clusters"] == 2, totals
    finally:
        os.remove(path)


def test_check_reports_skeleton_size_mismatch(tmp_dir):
    # GH #84: a genuine 34-byte count mismatch is still *reported* per file --
    # not swallowed, and not allowed to abort the corpus walk.
    d = os.path.join(tmp_dir, "bad-skel-corpus")
    os.makedirs(d, exist_ok=True)
    blob = tm._skeleton_container(
        tm._nodes_payload(tm._basic_skeleton_records()[:1]), 3,
        tm._node_indices([1, 2]))
    path = tm._write_tmp(blob, d)
    try:
        problems, totals = xmd_pose.check(d)
        assert any("NODES size" in p for p in problems), problems
        assert totals["models"] == 0, totals
    finally:
        os.remove(path)


def test_main_cli(tmp_dir):
    path = tm._write_tmp(te._single_cluster())
    try:
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            assert xmd_pose.main(["--model", path, "--json"]) == 0
            assert xmd_pose.main(["--model", path, "--bind-table"]) == 0
            assert xmd_pose.main(["--model", path]) == 0
            # --relation with a nonexistent pair is an error, not a crash.
            assert xmd_pose.main(["--model", path, "--relation", "0", "5"]) == 1
            root = os.path.dirname(path)
            assert xmd_pose.main(["--inventory", root, "--check"]) == 0
            assert xmd_pose.main(["--inventory", os.path.join(root, "nope"),
                                  "--check"]) == 1
        assert "coordinate_system" in buf.getvalue()
    finally:
        os.remove(path)


def test_main_requires_input(tmp_dir):
    try:
        xmd_pose.main([])
    except SystemExit as e:
        assert e.code == 2
    else:
        raise AssertionError("expected SystemExit when no --model/--inventory")


def main() -> int:
    tests = (test_bone_centroids_and_pose, test_bind_table_tree,
             test_cluster_relation_same_and_shifted, test_analyze_relations,
             test_check_clean_and_empty, test_check_skips_boxtree_prop,
             test_check_pairs_boxtree_triangles,
             test_check_reports_skeleton_size_mismatch, test_main_cli,
             test_main_requires_input)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            t(d)
            print(f"ok   {t.__name__}")
    print("PASS xmd_pose tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
