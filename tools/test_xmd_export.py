#!/usr/bin/env python3
"""Self-contained test for ``tools/xmd_export.py``.

Builds synthetic ``MOS DATAFILE2.0`` containers in a temp directory (no game
install, no input synthesis), exports them with the real exporter, then parses
the resulting OBJ / skin JSON back and asserts the round-trip is faithful.

Run headlessly:  python tools/test_xmd_export.py
Exit code 0 = pass, 1 = fail.
"""
from __future__ import annotations

import json
import os
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import mos_xmd  # noqa: E402
import xmd_export  # noqa: E402

# Reuse the already-tested synthetic-container builders.
import test_mos_xmd as tm  # noqa: E402


def _export_container(vf, tv, bd, tri, tri_count, nodes_payload, nodes_count,
                      indices) -> bytes:
    """A single-cluster model: VF -> TV -> BONEDEFORM -> TRIANGLES -> skeleton."""
    stride = mos_xmd.NODE_RECORD_LEN
    nodes = [
        ("VERTEXFRAMES", stride * 2, 0, len(vf), 1, 0),
        ("TVERTEXFRAMES", stride * 3, 0, len(tv), 1, 0x201),
        ("BONEDEFORM", stride * 4, 0, len(bd), 0, 0),
        ("TRIANGLES", stride * 5, 0, len(tri), tri_count, 0),
        ("NODES", stride * 6, 0, len(nodes_payload), nodes_count, 0),
        ("NODEINDICES", 0, 0, len(indices), len(indices) // 2, 0),
    ]
    return tm.build_container(nodes, [vf, tv, bd, tri, nodes_payload, indices])


def _single_cluster(bones=8, with_uv=True):
    verts = [(0, 0, 0), (65535, 0, 0), (0, 65535, 0), (0, 0, 65535)]
    normals = [(127, 0, 0), (0, 127, 0), (0, 0, -127), (0, 0, 0)]
    vf = tm._vf204_frame(verts, normals)
    tri = tm._triangles([0, 1, 2, 0, 2, 3])
    tv = tm._tv200_frame([(0, 0), (65535, 0), (0, 65535), (65535, 65535)]) \
        if with_uv else b""
    vrecs = [(0, 1, 0), (0, 2, 1), (0, 1, 3), (0, 1, 4)]
    infl = [(0, 255), (1, 128), (2, 255), (3, 255), (5, 64)]
    bd = tm._bonedeform(vrecs, infl, matrix_map_id=-1)
    nodes_payload = tm._flat_nodes(bones)
    indices = tm._node_indices(range(1, bones))
    return _export_container(vf, tv, bd, tri, 2, nodes_payload, bones, indices)


def _parse_obj(text):
    v, vn, vt, faces = [], [], [], []
    for line in text.splitlines():
        if line.startswith("v "):
            v.append(tuple(float(x) for x in line[2:].split()))
        elif line.startswith("vn "):
            vn.append(tuple(float(x) for x in line[3:].split()))
        elif line.startswith("vt "):
            vt.append(tuple(float(x) for x in line[3:].split()))
        elif line.startswith("f "):
            faces.append(line[2:].split())
    return v, vn, vt, faces


def _stem(path):
    return os.path.splitext(os.path.basename(path))[0]


def test_export_roundtrip(tmp_dir):
    path = tm._write_tmp(_single_cluster())
    out = os.path.join(tmp_dir, "export1")
    stem = _stem(path)
    try:
        man = xmd_export.export_model(path, out, combined=True)
        assert man["totals"]["vertices"] == 4, man["totals"]
        assert man["totals"]["triangles"] == 2, man["totals"]
        assert man["totals"]["uvs"] == 4, man["totals"]
        assert man["totals"]["skinned_clusters"] == 1, man["totals"]

        obj = open(os.path.join(out, f"{stem}_c00.obj"), encoding="utf-8").read()
        v, vn, vt, faces = _parse_obj(obj)
        assert len(v) == 4 and len(vt) == 4 and len(vn) == 4, (len(v), len(vt))
        assert len(faces) == 2, faces
        # positions: q=65535 -> max bound (100, 200, 300)
        assert abs(v[1][0] - 100.0) < 1e-3 and v[1][1] == 0.0, v[1]
        assert abs(v[3][2] - 300.0) < 1e-3, v[3]
        for f in faces:
            for ref in f:
                i = int(ref.split("/")[0])
                assert 1 <= i <= len(v), ref

        skin = json.load(open(os.path.join(out, f"{stem}.skin.json"),
                              encoding="utf-8"))
        assert skin["skeleton"]["node_count"] == 8, skin["skeleton"]
        assert skin["skeleton"]["rotations_available"] is False
        cl = skin["clusters"][0]
        assert cl["vertex_count"] == 4 and cl["influence_count"] == 5, cl
        assert cl["max_influences_per_vertex"] == 2, cl
        assert cl["influences"][0] == [[0, 1.0]], cl["influences"][0]
        assert cl["influences"][1] == [[1, 128 / 255.0], [2, 1.0]], \
            cl["influences"][1]

        # combined OBJ: all vertices in one file with rebased indices
        comb = open(os.path.join(out, f"{stem}.obj"), encoding="utf-8").read()
        cv, _cvn, cvt, cfaces = _parse_obj(comb)
        assert len(cv) == 4 and len(cfaces) == 2 and len(cvt) == 4
    finally:
        os.remove(path)


def test_export_without_uv(tmp_dir):
    path = tm._write_tmp(_single_cluster(with_uv=False))
    out = os.path.join(tmp_dir, "export2")
    stem = _stem(path)
    try:
        man = xmd_export.export_model(path, out)
        assert man["totals"]["uvs"] == 0, man["totals"]
        obj = open(os.path.join(out, f"{stem}_c00.obj"), encoding="utf-8").read()
        v, _vn, vt, faces = _parse_obj(obj)
        assert len(v) == 4 and len(vt) == 0 and len(faces) == 2
        # no UVs -> faces are "v//vn" (positions + normals only)
        assert all("//" in ref for f in faces for ref in f), faces
    finally:
        os.remove(path)


def test_export_cluster_select_and_out_of_range(tmp_dir):
    path = tm._write_tmp(_single_cluster())
    out = os.path.join(tmp_dir, "export3")
    try:
        man = xmd_export.export_model(path, out, clusters=[0])
        assert man["cluster_count"] == 1 and man["clusters"][0]["index"] == 0
        x = mos_xmd.Xmd(path)
        try:
            xmd_export.decode_cluster(x, 5)
        except mos_xmd.XmdError as e:
            assert "out of range" in str(e)
        else:
            raise AssertionError("expected XmdError for out-of-range cluster")
    finally:
        os.remove(path)


def test_manifest_bones_and_surfaces(tmp_dir):
    path = tm._write_tmp(_single_cluster(bones=8))
    out = os.path.join(tmp_dir, "export4")
    try:
        man = xmd_export.export_model(path, out)
        c = man["clusters"][0]
        assert c["bones_used"] == [0, 1, 2, 3, 5], c["bones_used"]
        assert c["bone_count_used"] == 5, c
        assert c["skinned"] is True and c["influences"] == 5, c
        assert man["coordinate_system"].startswith("engine model space")
        assert man["skeleton"]["used_nodes"] == 8, man["skeleton"]
    finally:
        os.remove(path)


def test_skeleton_dict_children(tmp_dir):
    path = tm._write_tmp(_single_cluster(bones=4))
    try:
        x = mos_xmd.Xmd(path)
        sk = xmd_export.skeleton_dict(x)
        assert sk["node_count"] == 4 and sk["padding_nodes"] == 0, sk
        # flat skeleton: node 0 is the root, children 1..3
        assert sk["nodes"][0]["children"] == [1, 2, 3], sk["nodes"][0]
        assert sk["node_indices"] == [1, 2, 3], sk["node_indices"]
    finally:
        os.remove(path)


def test_manifest_bone_space_palette(tmp_dir):
    # Same mesh, but the BONEDEFORM carries a matrix_bones palette: the manifest
    # must report the palette space and resolve the bones to skeleton nodes
    # (ADR-0423).
    verts = [(0, 0, 0), (65535, 0, 0), (0, 65535, 0), (0, 0, 65535)]
    normals = [(127, 0, 0), (0, 127, 0), (0, 0, -127), (0, 0, 0)]
    vf = tm._vf204_frame(verts, normals)
    tv = tm._tv200_frame([(0, 0), (65535, 0), (0, 65535), (65535, 65535)])
    tri = tm._triangles([0, 1, 2, 0, 2, 3])
    vrecs = [(0, 1, 0), (0, 2, 1), (0, 1, 3), (0, 1, 4)]
    infl = [(0, 255), (1, 128), (2, 255), (3, 255), (5, 64)]
    # palette: index 0->node 2, 1->node 3, 2->node 4, 3->node 5, 5->node 7
    bd = tm._bonedeform(vrecs, infl, matrix_map_id=0,
                        matrix_bones=b"\x02\x03\x04\x05\x06\x07")
    nodes_payload = tm._flat_nodes(8)
    indices = tm._node_indices(range(1, 8))
    path = tm._write_tmp(_export_container(vf, tv, bd, tri, 2, nodes_payload,
                                           8, indices))
    out = os.path.join(tmp_dir, "export5")
    try:
        man = xmd_export.export_model(path, out)
        c = man["clusters"][0]
        assert c["bone_space"] == "palette", c
        # raw bytes {0,1,2,3,5} -> nodes {2,3,4,5,7}
        assert c["bones_used"] == [2, 3, 4, 5, 7], c["bones_used"]
        skin = json.load(open(os.path.join(out, f"{_stem(path)}.skin.json"),
                              encoding="utf-8"))
        cl = skin["clusters"][0]
        assert cl["bone_space"] == "palette", cl
        assert cl["matrix_bones"] == [2, 3, 4, 5, 6, 7], cl["matrix_bones"]
        assert cl["influences"][1] == [[1, 128 / 255.0], [2, 1.0]], cl
        assert cl["resolved_influences"][1] == [[3, 128 / 255.0], [4, 1.0]], cl
    finally:
        os.remove(path)


def test_decode_cluster_skips_boxtree_triangles(tmp_dir):
    # GH #86: a BOXTREE TRIANGLES leaf sits between the two clusters; pairing by
    # index must use the mesh chunks, so cluster 1 keeps its own triangles.
    path = tm._write_tmp(tm._boxtree_triangles_container())
    try:
        x = mos_xmd.Xmd(path)
        m0 = xmd_export.decode_cluster(x, 0)
        m1 = xmd_export.decode_cluster(x, 1)
        assert m0.triangles == (0, 1, 2), m0.triangles
        assert m1.triangles == (2, 1, 0), m1.triangles
        assert m1.vertex_count == 3 and m1.tri_count == 1, (m1.vertex_count,
                                                            m1.tri_count)
        # `chunk("TRIANGLES")` is the BOXTREE leaf; the mesh selector is not.
        assert x.chunk("TRIANGLES").flags == 256
        assert xmd_export.triangle_nodes(x) == x.triangle_nodes()
    finally:
        os.remove(path)


def test_main_list_and_errors(tmp_dir):
    path = tm._write_tmp(_single_cluster())
    try:
        assert xmd_export.main([path, "--list"]) == 0
        # --out missing is an argparse error (SystemExit 2)
        try:
            xmd_export.main([path])
        except SystemExit as e:
            assert e.code == 2
        else:
            raise AssertionError("expected SystemExit for missing --out")
    finally:
        os.remove(path)


def main() -> int:
    tests = (test_export_roundtrip, test_export_without_uv,
             test_export_cluster_select_and_out_of_range,
             test_manifest_bones_and_surfaces, test_skeleton_dict_children,
             test_manifest_bone_space_palette,
             test_decode_cluster_skips_boxtree_triangles,
             test_main_list_and_errors)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            t(d)
            print(f"ok   {t.__name__}")
    print("PASS xmd_export round-trip tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
