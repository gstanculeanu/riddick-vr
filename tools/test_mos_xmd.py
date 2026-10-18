#!/usr/bin/env python3
"""Self-contained structural test for ``tools/mos_xmd.py``.

Builds synthetic ``MOS DATAFILE2.0`` containers in a temp directory (no game
install, no input synthesis) whose layout matches the loader recovered from
MCCDyn.dll (see ``docs/adr/0414-mos-datafile2-loader-container.md``), then
asserts the parser and its validator agree.

Run headlessly:  python tools/test_mos_xmd.py
Exit code 0 = pass, 1 = fail.
"""
from __future__ import annotations

import atexit
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mos_xmd  # noqa: E402  (path injected above)

MAGIC = b"MOS DATAFILE2.0\x00"

# Every synthetic model is written under one per-process directory, *not* the OS
# temp root. The corpus checks (``xmd_pose.check`` / ``xmd_fk --inventory``)
# walk ``os.path.dirname(path)`` recursively, so writing to the shared temp root
# let any unrelated ``.xmd``/``.xmg`` that happens to live there (e.g. a whole
# game tree copied in by an XRSIM shadow install) be parsed by the test (GH #82).
_TMP_ROOT = tempfile.mkdtemp(prefix="riddickvr-xmd-tests-")
atexit.register(shutil.rmtree, _TMP_ROOT, ignore_errors=True)


def _node(name: str, sibling: int, child: int,
          data_offset: int, data_size: int, count: int = 0,
          flags: int = 0, name_len: int = mos_xmd.NODE_NAME_LEN) -> bytes:
    nm = name.encode("latin1")[:name_len].ljust(name_len, b"\x00")
    return nm + struct.pack("<6I", sibling, child, data_offset,
                            data_size, count, flags)


def build_container(nodes, payloads):
    """nodes: list of (name, sibling, child, size, count, flags) at 0x30 stride.

    Returns the container bytes. ``payloads`` maps a node index -> bytes and is
    concatenated in node order starting at ``data_start``."""
    stride = mos_xmd.NODE_RECORD_LEN
    node_start = stride
    data_start = node_start + stride * len(nodes)
    off = data_start
    data_offsets = []
    for _name, _sib, _child, size, _cnt, _flg in nodes:
        data_offsets.append((off, size))
        off += size
    recs = bytearray()
    for (name, sib, child, size, cnt, flg), (doff, _s) in zip(nodes, data_offsets):
        recs += _node(name, sib, child, doff, size, cnt, flg)
    root = _node("MOS DATAFILE2.0", node_start, 0, data_start, 0)
    body = b"".join(payloads)
    blob = root + bytes(recs) + body
    assert len(root) == stride and len(blob) == off, (len(blob), off)
    return blob


def _write_tmp(blob: bytes, directory: str | None = None) -> str:
    """Write ``blob`` to a fresh ``.xmd`` in an isolated temp directory.

    ``directory`` overrides the default per-process directory (``_TMP_ROOT``);
    callers that then scan ``os.path.dirname(path)`` are guaranteed to see only
    the models this process wrote."""
    fd, path = tempfile.mkstemp(suffix=".xmd", dir=directory or _TMP_ROOT)
    with os.fdopen(fd, "wb") as f:
        f.write(blob)
    return path


def test_roundtrip(tmp_dir):
    # root -> A -> (B, C): A has child B, B has sibling C.
    stride = mos_xmd.NODE_RECORD_LEN
    nodes = [
        ("CONTAINER", 0, 0x30 + stride, 1, 7, 0),      # @0x30
        ("CHILDB", 0x30 + 2 * stride, 0, 4, 0, 0x201),  # @0x60
        ("CHILDC", 0, 0, 3, 0, 0),                      # @0x90
    ]
    blob = build_container(nodes, [b"\xAA", b"\xBB\xBB\xBB\xBB", b"\xCC\xCC\xCC"])
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert len(x.nodes) == 3, len(x.nodes)
        assert [n.name for n in x.nodes] == ["CONTAINER", "CHILDB", "CHILDC"]
        assert x.root.next_sibling == x.node_start == stride
        assert x.root.data_offset == x.data_start == 0x30 + 3 * stride
        a, b, c = x.nodes
        assert (a.first_child, a.data_size, a.count) == (b.offset, 1, 7)
        assert (b.next_sibling, b.flags) == (c.offset, 0x201)
        walked = [n.name for n in x.walk_linked()]
        assert walked[1:] == ["CONTAINER", "CHILDB", "CHILDC"], walked
        problems = x.validate()
        assert problems == [], problems
    finally:
        os.remove(path)


def test_detects_dangling_pointer():
    stride = mos_xmd.NODE_RECORD_LEN
    nodes = [
        ("SOLO", 0, 0, 2, 0, 0),
    ]
    # Corrupt SOLO's child pointer to a non-node offset.
    blob = bytearray(build_container(nodes, [b"\x01\x02"]))
    struct.pack_into("<I", blob, stride + mos_xmd.NODE_NAME_LEN + 4, 0xDEAD)
    path = _write_tmp(bytes(blob))
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("not a node" in p for p in problems), problems
    finally:
        os.remove(path)


def test_rejects_bad_magic(tmp_dir):
    path = _write_tmp(b"NOT A MOS FILE" + b"\x00" * 64)
    try:
        try:
            mos_xmd.Xmd(path)
        except mos_xmd.XmdError:
            return
        raise AssertionError("bad magic was accepted")
    finally:
        os.remove(path)


def _vf204_frame(verts, normals, mn=(0.0, 0.0, 0.0), mx=(100.0, 200.0, 300.0),
                 lead=1) -> bytes:
    """Build a VERTEXFRAMES payload: one version-0x204 frame + leading count."""
    n = len(verts)
    out = bytearray()
    out += struct.pack("<I", lead)
    out += struct.pack("<I", 0x204)          # frame version
    out += struct.pack("<I", 0)              # header20
    out += struct.pack("<3f", *mn)           # bounds_min
    out += struct.pack("<3f", *mx)           # bounds_max
    out += struct.pack("<3f", 0.0, 0.0, 0.0)  # aux
    out += struct.pack("<I", 0)              # header30
    out += struct.pack("<I", n)              # vertex count
    for (x, y, z) in verts:
        out += struct.pack("<3H", x, y, z)
    for (x, y, z) in normals:
        out += struct.pack("<3b", x, y, z)
    assert len(out) == 56 + 9 * n, len(out)
    return bytes(out)


def _triangles(indices) -> bytes:
    out = bytearray(struct.pack("<I", len(indices) // 3))
    for i in indices:
        out += struct.pack("<H", i)
    return bytes(out)


def _tv200_frame(uvs, mn=(0.0, 0.0), mx=(1.0, 1.0)) -> bytes:
    """Build a TVERTEXFRAMES payload: ``u32 n; 4* f32; u16 uv[n][2]``."""
    n = len(uvs)
    out = bytearray(struct.pack("<I", n))
    out += struct.pack("<2f", *mn)
    out += struct.pack("<2f", *mx)
    for (u, v) in uvs:
        out += struct.pack("<2H", u, v)
    assert len(out) == 20 + 4 * n, len(out)
    return bytes(out)


def _uv_container(vf_payload, uv_payload, tri_payload, tri_count):
    stride = mos_xmd.NODE_RECORD_LEN
    # contiguous 0x30 records: VERTEXFRAMES @0x30, TVERTEXFRAMES @0x60, TRIANGLES @0x90
    nodes = [
        ("VERTEXFRAMES", stride * 2, 0, len(vf_payload), 1, 0),
        ("TVERTEXFRAMES", stride * 3, 0, len(uv_payload), 1, 0x201),
        ("TRIANGLES", 0, 0, len(tri_payload), tri_count, 0),
    ]
    return build_container(nodes, [vf_payload, uv_payload, tri_payload])


def _geometry_container(vf_payload, tri_payload, tri_count):
    stride = mos_xmd.NODE_RECORD_LEN
    node1 = stride * 2  # VERTEXFRAMES @0x60 when preceded by a 0x30 root
    nodes = [
        ("VERTEXFRAMES", node1, 0, len(vf_payload), 1, 0),
        ("TRIANGLES", 0, 0, len(tri_payload), tri_count, 0),
    ]
    return build_container(nodes, [vf_payload, tri_payload])


def test_geometry_roundtrip(tmp_dir):
    verts = [(0, 0, 0), (65535, 0, 0), (0, 65535, 0), (0, 0, 65535)]
    normals = [(127, 0, 0), (0, 127, 0), (0, 0, -127), (0, 0, 0)]
    blob = _geometry_container(_vf204_frame(verts, normals),
                               _triangles([0, 1, 2, 0, 2, 3]), 2)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        frames = x.vertex_frames()
        assert len(frames) == 1, frames
        f = frames[0]
        assert f.version == 0x204 and f.count == 4, (f.version, f.count)
        assert f.bounds_min == (0.0, 0.0, 0.0)
        assert f.bounds_max == (100.0, 200.0, 300.0)
        # q=65535 -> max, q=0 -> min
        assert f.positions[0] == (0.0, 0.0, 0.0), f.positions[0]
        assert f.positions[1] == (100.0, 0.0, 0.0), f.positions[1]
        assert f.positions[2] == (0.0, 200.0, 0.0), f.positions[2]
        assert abs(f.positions[3][2] - 300.0) < 1e-3, f.positions[3]
        # normals are re-normalized to unit length (or zeroed)
        assert abs(f.normals[0][0] - 1.0) < 1e-6, f.normals[0]
        assert f.normals[3] == (0.0, 0.0, 0.0), f.normals[3]
        cnt, idx = x.triangle_indices()
        assert cnt == 2 and max(idx) == 3, (cnt, idx)
        clusters = x.cluster_meshes()
        assert len(clusters) == 1 and clusters[0]["index_in_range"] is True
        assert x.validate() == [], x.validate()
        assert x.geometry_stats()["decoded_vertices"] == 4
        assert x.geometry_stats()["decoded_triangles"] == 2
    finally:
        os.remove(path)


def test_geometry_detects_index_out_of_range(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    # index 4 is one past the last vertex (count 4)
    blob = _geometry_container(_vf204_frame(verts, normals),
                               _triangles([0, 1, 4]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any(">= decoded vertex count" in p for p in problems), problems
    finally:
        os.remove(path)


def test_geometry_detects_size_mismatch(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 0)] * 3
    # declare 2 triangles but only supply 1
    good = _triangles([0, 1, 2, 0, 1, 2])
    bad = bytearray(good)
    struct.pack_into("<I", bad, 0, 5)
    blob = _geometry_container(_vf204_frame(verts, normals), bytes(bad), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("TRIANGLES size" in p for p in problems), problems
    finally:
        os.remove(path)


def _vf205_frame(verts, normals, extra_basis, mn=(0.0, 0.0, 0.0),
                 mx=(100.0, 200.0, 300.0)) -> bytes:
    """Build a VERTEXFRAMES payload: one version-0x205 frame, **no** leading
    count word, followed by the two extra i8 unit-vector arrays."""
    n = len(verts)
    out = bytearray()
    out += struct.pack("<I", 0x205)           # frame version (no count prefix)
    out += struct.pack("<I", 0)               # header20
    out += struct.pack("<3f", *mn)            # bounds_min
    out += struct.pack("<3f", *mx)            # bounds_max
    out += struct.pack("<3f", 0.0, 0.0, 0.0)  # aux
    out += struct.pack("<I", 0)               # header30
    out += struct.pack("<I", n)               # vertex count
    for (x, y, z) in verts:
        out += struct.pack("<3H", x, y, z)
    for (x, y, z) in normals:
        out += struct.pack("<3b", x, y, z)
    for arr in extra_basis:
        for (x, y, z) in arr:
            out += struct.pack("<3b", x, y, z)
    assert len(out) == 52 + 15 * n, len(out)
    return bytes(out)


def test_geometry_roundtrip_205(tmp_dir):
    # Version 0x205 (every 2009 model): the 0x204 header + position + normal
    # arrays, followed by two more i8 unit-vector arrays (GH #85 / ADR-0476).
    verts = [(0, 0, 0), (65535, 0, 0), (0, 65535, 0), (0, 0, 65535)]
    normals = [(127, 0, 0), (0, 127, 0), (0, 0, -127), (0, 0, 0)]
    basis1 = [(0, 0, 127), (0, 127, 0), (127, 0, 0), (0, 0, 127)]
    basis2 = [(0, -128, 0), (127, 0, 0), (0, 0, 127), (0, 127, 0)]
    payload = _vf205_frame(verts, normals, [basis1, basis2])
    assert len(payload) == 52 + 15 * 4, len(payload)   # no count prefix
    blob = _geometry_container(payload, _triangles([0, 1, 2, 0, 2, 3]), 2)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        frames = x.vertex_frames()
        assert len(frames) == 1, frames
        f = frames[0]
        assert f.version == 0x205 and f.count == 4, (f.version, f.count)
        assert f.size == 52 + 15 * 4, f.size
        assert f.bounds_min == (0.0, 0.0, 0.0)
        assert f.bounds_max == (100.0, 200.0, 300.0)
        assert f.positions[0] == (0.0, 0.0, 0.0), f.positions[0]
        assert f.positions[1] == (100.0, 0.0, 0.0), f.positions[1]
        assert f.positions[2] == (0.0, 200.0, 0.0), f.positions[2]
        assert abs(f.positions[3][2] - 300.0) < 1e-3, f.positions[3]
        # normals re-normalized to unit (or zeroed)
        assert abs(f.normals[0][0] - 1.0) < 1e-6, f.normals[0]
        assert f.normals[3] == (0.0, 0.0, 0.0), f.normals[3]
        # two extra basis arrays, each a unit vector per vertex
        assert len(f.extra_basis) == 2, f.extra_basis
        assert len(f.extra_basis[0]) == 4 and len(f.extra_basis[1]) == 4
        assert abs(f.extra_basis[0][0][2] - 1.0) < 1e-6, f.extra_basis[0][0]
        assert abs(f.extra_basis[1][0][1] + 1.0) < 1e-6, f.extra_basis[1][0]
        d = f.as_dict(with_geometry=True)
        assert d["extra_basis_count"] == 2 and len(d["extra_basis"]) == 2, d
        assert x.triangle_indices()[0] == 2
        assert x.validate() == [], x.validate()
        clusters = x.cluster_meshes()
        assert len(clusters) == 1 and clusters[0]["index_in_range"] is True
        assert x.geometry_stats()["decoded_vertices"] == 4
    finally:
        os.remove(path)


def test_geometry_204_has_no_extra_basis(tmp_dir):
    # The 0x204 frame must keep decoding with an empty basis (no regression).
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 127)] * 3
    blob = _geometry_container(_vf204_frame(verts, normals),
                               _triangles([0, 1, 2]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        f = x.vertex_frames()[0]
        assert f.version == 0x204 and f.extra_basis == [], f.as_dict()
        assert "extra_basis_count" not in f.as_dict()
        assert x.validate() == [], x.validate()
    finally:
        os.remove(path)


def test_geometry_205_truncated_basis_is_rejected(tmp_dir):
    # A 0x205 frame that declares its vertex count but omits the two extra
    # basis arrays no longer tiles the chunk and must be reported.
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 127)] * 3
    basis = [([(0, 0, 127)] * 3), ([(0, 127, 0)] * 3)]
    good = _vf205_frame(verts, normals, basis)
    # drop the whole second basis array (3 bytes/vertex) from the payload
    bad = good[:len(good) - 3 * 3]
    blob = _geometry_container(bad, _triangles([0, 1, 2]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("VERTEXFRAMES" in p for p in problems), problems
    finally:
        os.remove(path)


def test_uv_roundtrip(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    uvs = [(0, 0), (65535, 0), (0, 65535), (65535, 65535)]
    uv_payload = _tv200_frame(uvs, mn=(0.25, 0.5), mx=(0.75, 1.0))
    blob = _uv_container(_vf204_frame(verts, normals), uv_payload,
                         _triangles([0, 1, 2, 0, 2, 3]), 2)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        frames = x.tvertex_frames()
        assert len(frames) == 1, frames
        f = frames[0]
        assert f.version == 0x200 and f.count == 4, (f.version, f.count)
        assert f.bounds_min == (0.25, 0.5), f.bounds_min
        assert f.bounds_max == (0.75, 1.0), f.bounds_max
        # q=0 -> min, q=65535 -> max
        assert f.uvs[0] == (0.25, 0.5), f.uvs[0]
        assert abs(f.uvs[1][0] - 0.75) < 1e-6 and f.uvs[1][1] == 0.5, f.uvs[1]
        assert f.uvs[2][0] == 0.25 and abs(f.uvs[2][1] - 1.0) < 1e-6, f.uvs[2]
        assert x.validate() == [], x.validate()
        c = x.cluster_meshes()[0]
        assert c["uv_count"] == 4 and c["uv_matches"] is True, c
        stats = x.geometry_stats()
        assert stats["uv_clusters"] == 1 and stats["decoded_uvs"] == 4, stats
    finally:
        os.remove(path)


def test_uv_detects_count_mismatch(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    # only 3 UVs for 4 vertices
    uv_payload = _tv200_frame([(0, 0), (1, 0), (0, 1)])
    blob = _uv_container(_vf204_frame(verts, normals), uv_payload,
                         _triangles([0, 1, 2]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("UV count 3 != vertex count" in p for p in problems), problems
    finally:
        os.remove(path)


def test_uv_detects_bad_tiling(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 0)] * 3
    good = bytearray(_tv200_frame([(0, 0), (1, 0), (0, 1)]))
    # bump the declared count so size no longer tiles the chunk
    struct.pack_into("<I", good, 0, 5)
    blob = _uv_container(_vf204_frame(verts, normals), bytes(good),
                         _triangles([0, 1, 2]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("UV frame" in p for p in problems), problems
    finally:
        os.remove(path)


def test_uv_empty_chunk_is_ok(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 0)] * 3
    blob = _uv_container(_vf204_frame(verts, normals), b"",
                         _triangles([0, 1, 2]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert x.tvertex_frames() == []
        assert x.validate() == [], x.validate()
        c = x.cluster_meshes()[0]
        assert c["uv_count"] is None and c["uv_matches"] is None, c
    finally:
        os.remove(path)


def _bonedeform(vertices, influences, matrix_map_id=None, matrix_bones=()):
    """Build a BONEDEFORM payload: vertex records, influence pool, matrix map.

    ``vertices`` is a list of ``(flags, influence_count, influence_start)`` and
    ``influences`` a list of ``(bone, weight_q)`` (weight = q / 255)."""
    out = bytearray(struct.pack("<I", len(vertices)))
    for (flags, cnt, start) in vertices:
        out += struct.pack("<BBH", flags, cnt, start)
    out += struct.pack("<I", len(influences))
    for (bone, wq) in influences:
        out += struct.pack("<BB", bone, wq)
    if matrix_map_id is not None:
        out += struct.pack("<i", matrix_map_id)
        out += struct.pack("<I", len(matrix_bones))
        out += bytes(matrix_bones)
    return bytes(out)


def _flat_nodes(n: int) -> bytes:
    """A minimal valid skeleton: node 0 as root with ``n-1`` direct children."""
    recs = [dict(rotation_slot=0, movement_slot=0, child_start=0,
                 child_count=n - 1, parent=0)]
    for i in range(1, n):
        recs.append(dict(rotation_slot=i,
                         movement_slot=mos_xmd.NODE_SLOT_NONE, parent=0))
    return _nodes_payload(recs)


def _skin_container(vf_payload, bd_payload, bones):
    """VERTEXFRAMES + BONEDEFORM (+ NODES/NODEINDICES skeleton with ``bones``)."""
    stride = mos_xmd.NODE_RECORD_LEN
    nodes_payload = _flat_nodes(bones)
    idx = _node_indices(range(1, bones))
    nodes = [
        ("VERTEXFRAMES", stride * 2, 0, len(vf_payload), 1, 0),
        ("BONEDEFORM", stride * 3, 0, len(bd_payload), 0, 0),
        ("NODES", stride * 4, 0, len(nodes_payload), bones, 0),
        ("NODEINDICES", 0, 0, len(idx), len(idx) // 2, 0),
    ]
    return build_container(
        nodes, [vf_payload, bd_payload, nodes_payload, idx])


def test_skin_roundtrip(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    vrecs = [(0, 2, 0), (0, 1, 2), (0, 3, 3), (0, 1, 6)]
    infl = [(0, 255), (1, 128), (2, 255), (0, 64), (3, 64), (5, 128),
            (7, 255)]
    bd = _bonedeform(vrecs, infl, matrix_map_id=-1, matrix_bones=())
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=8)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        sk = x.bone_deform()
        assert sk.vertex_count == 4 and sk.influence_count == 7, sk.as_dict()
        assert sk.vertices[0].influence_count == 2
        assert sk.influences[1] == (1, 128 / 255.0), sk.influences[1]
        assert sk.max_bone() == 7
        assert sk.max_influences() == 3
        assert sk.matrix_map_id == -1 and sk.matrix_bones == []
        assert sk.exact is True and sk.size == len(bd), (sk.size, len(bd))
        assert x.validate() == [], x.validate()
        c = x.cluster_meshes()[0]
        assert c["skin_matches"] is True and c["max_bone"] == 7, c
        st = x.geometry_stats()
        assert st["skinned_clusters"] == 1, st
        assert st["decoded_influences"] == 7, st
        assert st["max_influences_per_vertex"] == 3, st
    finally:
        os.remove(path)


def test_skin_matrix_map_and_influences_of(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 0)] * 3
    vrecs = [(0, 1, 0), (0, 2, 1), (0, 1, 3)]
    # Influence bytes are palette indices; the palette resolves them to skeleton
    # nodes 1, 2 and 9 (ADR-0423).
    infl = [(0, 200), (1, 55), (2, 255), (0, 100)]
    bd = _bonedeform(vrecs, infl, matrix_map_id=3, matrix_bones=b"\x01\x02\x09")
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=16)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        sk = x.bone_deform()
        assert sk.matrix_map_id == 3, sk.matrix_map_id
        assert sk.matrix_bones == [1, 2, 9], sk.matrix_bones
        assert sk.has_palette() and sk.bone_space() == "palette", sk.bone_space()
        assert sk.influences_of(1) == [(1, 55 / 255.0), (2, 1.0)], \
            sk.influences_of(1)
        assert sk.resolved_influences_of(1) == [(2, 55 / 255.0), (9, 1.0)], \
            sk.resolved_influences_of(1)
        assert sk.resolve_bone(2) == 9, sk.resolve_bone(2)
        assert sk.max_bone() == 2, sk.max_bone()
        assert sk.max_resolved_bone() == 9, sk.max_resolved_bone()
        assert sk.used_palette_indices() == [0, 1, 2]
        assert sk.palette_fully_used() is True
        assert x.validate() == [], x.validate()
        c = x.cluster_meshes()[0]
        assert c["bone_space"] == "palette", c
        assert c["max_resolved_bone"] == 9 and c["max_bone"] == 2, c
        assert x.geometry_stats()["paletted_bone_clusters"] == 1, x.geometry_stats()
        assert x.geometry_stats()["direct_bone_clusters"] == 0
    finally:
        os.remove(path)


def test_skin_direct_bone_space_without_palette(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (3, 255)]          # direct skeleton node indices
    bd = _bonedeform(vrecs, infl, matrix_map_id=-1, matrix_bones=())
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=4)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        sk = x.bone_deform()
        assert not sk.has_palette() and sk.bone_space() == "node", sk.bone_space()
        assert sk.resolve_bone(3) == 3 and sk.max_resolved_bone() == 3
        assert sk.palette_fully_used() is True
        assert x.validate() == [], x.validate()
        assert x.geometry_stats()["direct_bone_clusters"] == 1
        assert x.geometry_stats()["paletted_bone_clusters"] == 0
    finally:
        os.remove(path)


def test_skin_id_minus_one_with_palette_still_palette(tmp_dir):
    # 33 shipped clusters store matrix_map_id == -1 *and* a palette; the palette
    # length alone decides the space, not the id (ADR-0423).
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (1, 255)]
    bd = _bonedeform(vrecs, infl, matrix_map_id=-1, matrix_bones=b"\x02\x03")
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=8)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        sk = x.bone_deform()
        assert sk.matrix_map_id == -1 and sk.matrix_bones == [2, 3]
        assert sk.bone_space() == "palette", sk.bone_space()
        assert sk.resolve_bone(1) == 3
        assert x.validate() == [], x.validate()
        assert x.geometry_stats()["paletted_bone_clusters"] == 1
    finally:
        os.remove(path)


def test_skin_palette_index_out_of_range(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (5, 255)]          # palette index 5 with a 3-entry palette
    bd = _bonedeform(vrecs, infl, matrix_map_id=1, matrix_bones=b"\x01\x02\x09")
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=8)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("bone palette index 5 >= palette size 3" in p
                   for p in problems), problems
    finally:
        os.remove(path)


def test_skin_palette_entry_out_of_range(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (1, 255)]          # only palette entries 0 and 1 used
    # entry 2 (20) is unused but still outside the 4-node skeleton
    bd = _bonedeform(vrecs, infl, matrix_map_id=1,
                     matrix_bones=b"\x01\x02\x14")
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=4)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("bone palette entry >= skeleton node count 4" in p
                   for p in problems), problems
    finally:
        os.remove(path)


def test_skin_palette_duplicate_entries(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 2, 1)]
    infl = [(0, 255), (1, 128), (2, 127)]
    bd = _bonedeform(vrecs, infl, matrix_map_id=2, matrix_bones=b"\x01\x01\x02")
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=8)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("bone palette has duplicate entries" in p
                   for p in problems), problems
    finally:
        os.remove(path)


def test_skin_palette_unused_entries_ok(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (1, 255)]          # entries 1 (node 9) unused
    bd = _bonedeform(vrecs, infl, matrix_map_id=0, matrix_bones=b"\x01\x02\x09")
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=16)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        sk = x.bone_deform()
        assert sk.palette_fully_used() is False, sk.used_palette_indices()
        assert x.validate() == [], x.validate()
        assert x.geometry_stats()["palette_unused_clusters"] == 1
    finally:
        os.remove(path)


def test_skin_detects_prefix_sum_mismatch(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    # second vertex must start at 1 but claims 0
    vrecs = [(0, 1, 0), (0, 1, 0)]
    infl = [(0, 255), (1, 255)]
    bd = _bonedeform(vrecs, infl, matrix_map_id=-1)
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=4)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("influence_start" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skin_detects_vertex_count_mismatch(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 0)] * 3
    # only 2 vertex records for 3 mesh vertices
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (1, 255)]
    bd = _bonedeform(vrecs, infl, matrix_map_id=-1)
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=4)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("skin vertex count 2 != vertex count" in p for p in problems), \
            problems
    finally:
        os.remove(path)


def test_skin_detects_bone_out_of_range(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (5, 255)]     # bone 5 with only 4 skeleton nodes
    bd = _bonedeform(vrecs, infl, matrix_map_id=-1)
    blob = _skin_container(_vf204_frame(verts, normals), bd, bones=4)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("bone index 5 >= skeleton node count 4" in p
                   for p in problems), problems
    finally:
        os.remove(path)


def test_skin_bad_matrix_map_tiling(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    infl = [(0, 255), (1, 255)]
    good = bytearray(_bonedeform(vrecs, infl, matrix_map_id=0,
                                 matrix_bones=b"\x01\x02"))
    struct.pack_into("<I", good, len(good) - 6, 5)   # claim 5 bones, supply 2
    blob = _skin_container(_vf204_frame(verts, normals), bytes(good), bones=4)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("matrix-map size" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skin_absent_is_ok(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 0)] * 3
    # geometry-only container: no BONEDEFORM node
    blob = _geometry_container(_vf204_frame(verts, normals),
                               _triangles([0, 1, 2]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert x.bone_deform() is None
        assert x.validate() == [], x.validate()
        c = x.cluster_meshes()[0]
        assert c["skin"] is None and c["skin_matches"] is None, c
        assert x.geometry_stats()["skinned_clusters"] == 0
    finally:
        os.remove(path)


def _node_record(version=mos_xmd.NODE_ENTRY_VERSION, translation=(0.0, 0.0, 0.0),
                 uc=0, child_start=0, child_count=0, parent=0, scale=(1.0, 1.0),
                 rotation_slot=0, movement_slot=mos_xmd.NODE_SLOT_NONE) -> bytes:
    out = struct.pack("<HfffHHHHffHH", version, translation[0],
                      translation[1], translation[2], uc, child_start,
                      child_count, parent, scale[0], scale[1], rotation_slot,
                      movement_slot)
    assert len(out) == mos_xmd.NODE_ENTRY_LEN, len(out)
    return out


def _nodes_payload(records) -> bytes:
    """records: list of keyword dicts fed to :func:`_node_record`."""
    return b"".join(_node_record(**r) for r in records)


def _node_indices(indices) -> bytes:
    return b"".join(struct.pack("<H", i) for i in indices)


def _skeleton_container(nodes_payload, count, indices=b"") -> bytes:
    stride = mos_xmd.NODE_RECORD_LEN
    nodes = [
        ("NODES", stride * 2, 0, len(nodes_payload), count, 0),
        ("NODEINDICES", 0, 0, len(indices), len(indices) // 2, 0),
    ]
    return build_container(nodes, [nodes_payload, indices])


def _basic_skeleton_records():
    # node 0 root, children 1 & 2; node 3 is an all-zero padding record.
    return [
        dict(rotation_slot=0, movement_slot=0, child_count=2, parent=0),
        dict(rotation_slot=1, movement_slot=1, parent=0),
        dict(rotation_slot=2, parent=0, translation=(1.0, 2.0, 3.0)),
        dict(rotation_slot=mos_xmd.NODE_SLOT_NONE,
             movement_slot=mos_xmd.NODE_SLOT_NONE, parent=0),
    ]


def test_skeleton_roundtrip(tmp_dir):
    payload = _nodes_payload(_basic_skeleton_records())
    blob = _skeleton_container(payload, 4, _node_indices([1, 2]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        sk = x.skeleton()
        assert len(sk.nodes) == 4, len(sk.nodes)
        assert len(sk.used_nodes()) == 3 and len(sk.unused_nodes()) == 1
        assert sk.node_indices == (1, 2), sk.node_indices
        assert sk.children_of(0) == [1, 2], sk.children_of(0)
        assert sk.roots() == [0], sk.roots()
        assert sk.is_tree() and sk.child_count_matches()
        assert sk.max_rotation_slot() == 2
        assert sk.max_movement_slot() == 1
        assert sk.nodes[2].translation == (1.0, 2.0, 3.0)
        assert not sk.nodes[3].is_used()
        assert x.validate() == [], x.validate()
        g = x.geometry_summary()
        assert g["skeleton_nodes"] == 4 and g["skeleton_used_nodes"] == 3, g
        assert g["skeleton_unused_nodes"] == 1 and g["skeleton_indices"] == 2, g
    finally:
        os.remove(path)


def test_skeleton_detects_bad_child_count(tmp_dir):
    recs = _basic_skeleton_records()
    recs[0]["child_count"] = 5           # really 2
    payload = _nodes_payload(recs)
    blob = _skeleton_container(payload, 4, _node_indices([1, 2]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("child_count" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skeleton_detects_bad_node_indices(tmp_dir):
    payload = _nodes_payload(_basic_skeleton_records())
    # 3 is padding and 2 is missing -> not the used non-root node set
    blob = _skeleton_container(payload, 4, _node_indices([1, 3]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("NODEINDICES" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skeleton_detects_size_mismatch(tmp_dir):
    # header claims 3 nodes but the payload only holds one record
    payload = _nodes_payload(_basic_skeleton_records()[:1])
    blob = _skeleton_container(payload, 3, _node_indices([1, 2]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("NODES size" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skeleton_detects_padding_not_zeroed(tmp_dir):
    recs = _basic_skeleton_records()
    recs[3]["translation"] = (9.0, 9.0, 9.0)   # padding must be zeroed
    payload = _nodes_payload(recs)
    blob = _skeleton_container(payload, 4, _node_indices([1, 2]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("padding record" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skeleton_detects_bad_version(tmp_dir):
    recs = _basic_skeleton_records()
    recs[1]["version"] = 0x200
    payload = _nodes_payload(recs)
    blob = _skeleton_container(payload, 4, _node_indices([1, 2]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("version" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skeleton_absent_is_ok(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normals = [(0, 0, 0)] * 3
    blob = _geometry_container(_vf204_frame(verts, normals),
                               _triangles([0, 1, 2]), 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert x.skeleton() is None
        assert x.validate() == [], x.validate()
        assert x.geometry_summary()["skeleton_nodes"] == 0
    finally:
        os.remove(path)


def test_skeleton_child_table(tmp_dir):
    # Three-level chain 0 -> 1 -> 2 -> 3. The child table lists 1,2,3 and each
    # node's child_start indexes into it (the FUN_1001e040 walk).
    recs = [
        dict(rotation_slot=0, movement_slot=0, child_start=0, child_count=1,
             parent=0),
        dict(rotation_slot=1, movement_slot=1, child_start=1, child_count=1,
             parent=0),
        dict(rotation_slot=2, child_start=2, child_count=1, parent=1),
        dict(rotation_slot=3, child_start=3, child_count=0, parent=2),
    ]
    payload = _nodes_payload(recs)
    blob = _skeleton_container(payload, 4, _node_indices([1, 2, 3]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        sk = x.skeleton()
        assert sk.node_indices == (1, 2, 3), sk.node_indices
        assert sk.children_of(0) == [1], sk.children_of(0)
        assert sk.children_of(1) == [2], sk.children_of(1)
        assert sk.children_of(2) == [3], sk.children_of(2)
        assert sk.children_of(3) == [] and sk.children_of(99) == []
        assert sk.child_ranges_tile() and sk.child_count_matches()
        assert sk.derived_parents() == {1: 0, 2: 1, 3: 2}, sk.derived_parents()
        assert x.validate() == [], x.validate()
        assert x.geometry_summary()["skeleton_child_table_ok"] is True
    finally:
        os.remove(path)


def test_skeleton_detects_child_range_overrun(tmp_dir):
    recs = _basic_skeleton_records()
    recs[0]["child_start"] = 1          # 1 + 2 children > 2 indices
    payload = _nodes_payload(recs)
    blob = _skeleton_container(payload, 4, _node_indices([1, 2]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("child range" in p for p in problems), problems
    finally:
        os.remove(path)


def test_skeleton_detects_parent_table_mismatch(tmp_dir):
    # node 2's parent field says root but the child table makes node 1 its parent
    recs = [
        dict(rotation_slot=0, movement_slot=0, child_start=0, child_count=1,
             parent=0),
        dict(rotation_slot=1, movement_slot=1, child_start=1, child_count=1,
             parent=0),
        dict(rotation_slot=2, child_start=2, child_count=0, parent=0),
    ]
    payload = _nodes_payload(recs)
    blob = _skeleton_container(payload, 3, _node_indices([1, 2]))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("child-table parent" in p for p in problems), problems
    finally:
        os.remove(path)


# --------------------------------------------------------------------------- #
# GH #84: "NODES" is not exclusive to the skeleton. The 2009 Assault on Dark
# Athena models put a 12-byte NODES under BOXTREE ahead of the SKELETON array,
# so `chunk("NODES")` used to pick the wrong one.
# --------------------------------------------------------------------------- #
def _boxtree_nodes_payload(records=19) -> bytes:
    """A BOXTREE bounding-volume ``NODES``: 12 bytes per record, not 34."""
    return b"\x00" * (records * 12)


def _boxtree_prop_container() -> bytes:
    """EXTENDEDMODEL -> BOXTREE -> NODES (12-byte records); no SKELETON."""
    stride = mos_xmd.NODE_RECORD_LEN
    payload = _boxtree_nodes_payload()
    nodes = [
        ("EXTENDEDMODEL", 0, stride * 2, 0, 0, 0),   # @0x30, child BOXTREE
        ("BOXTREE", 0, stride * 3, 0, 0, 0),          # @0x60, child NODES
        ("NODES", 0, 0, len(payload), 19, 0),         # @0x90 (BOXTREE child)
    ]
    return build_container(nodes, [payload])


def _boxtree_plus_skeleton_container() -> bytes:
    """A BOXTREE NODES *ahead of* a real SKELETON NODES/NODEINDICES pair."""
    stride = mos_xmd.NODE_RECORD_LEN
    bt = _boxtree_nodes_payload()
    sk = _nodes_payload(_basic_skeleton_records())
    idx = _node_indices([1, 2])
    nodes = [
        ("EXTENDEDMODEL", 0, stride * 2, 0, 0, 0),   # @0x30, child BOXTREE
        ("BOXTREE", 0, stride * 3, 0, 0, 0),          # @0x60, child NODES
        ("NODES", stride * 4, 0, len(bt), 19, 0),     # @0x90 BOXTREE NODES
        ("SKELETON", 0, stride * 5, 0, 0, 0),         # @0xC0, child NODES
        ("NODES", stride * 6, 0, len(sk), 4, 0),      # @0xF0 skeleton NODES
        ("NODEINDICES", 0, 0, len(idx), len(idx) // 2, 0),  # @0x120
    ]
    return build_container(nodes, [bt, sk, idx])


def test_boxtree_nodes_is_not_a_skeleton(tmp_dir):
    path = _write_tmp(_boxtree_prop_container())
    try:
        x = mos_xmd.Xmd(path)
        # The 12-byte BOXTREE NODES must not be decoded as a skeleton.
        assert x.skeleton() is None
        assert x.skeleton_node() is None
        assert x.validate() == [], x.validate()
        g = x.geometry_summary()
        assert g["skeleton_node_count"] == 0 and g["has_skinning"] is False, g
    finally:
        os.remove(path)


def test_skeleton_selects_skel_nodes_over_boxtree(tmp_dir):
    path = _write_tmp(_boxtree_plus_skeleton_container())
    try:
        x = mos_xmd.Xmd(path)
        # The trap: `chunk` returns the first NODES (the 12-byte BOXTREE one)...
        assert x.chunk("NODES").data_size == len(_boxtree_nodes_payload())
        # ...but skeleton selection skips it for the SKELETON child.
        nd = x.skeleton_node()
        assert nd.data_size == 4 * mos_xmd.NODE_ENTRY_LEN, nd.data_size
        sk = x.skeleton()
        assert len(sk.nodes) == 4 and sk.node_indices == (1, 2), sk.as_dict()
        assert x.validate() == [], x.validate()
        g = x.geometry_summary()
        assert g["skeleton_node_count"] == 4 and g["skeleton_indices"] == 2, g
    finally:
        os.remove(path)


def _misc_payload(version=0x102, flags=0, surface_index=0, reserved=0,
                  box_min=(0.0, 0.0, 0.0), box_max=(100.0, 200.0, 300.0),
                  trailer=0.0, tail=None) -> bytes:
    if tail is None:
        tail = struct.pack("<7f", *box_min, *box_max, trailer)
    out = struct.pack("<4I", version, flags, surface_index, reserved) + tail
    assert len(out) == mos_xmd.MISC_RECORD_LEN, len(out)
    return out


def _cluster_info_container(misc, vf_payload, uv_payload, surf_payload,
                            surf_count) -> bytes:
    """One CLUSTER (MISCELLANEOUS + VERTEXFRAMES + TVERTEXFRAMES) + SURFACES."""
    stride = mos_xmd.NODE_RECORD_LEN

    def at(i):
        return stride * (i + 1)

    nodes = [
        ("EXTENDEDMODEL", 0, at(1), 0, 0, 0),
        ("CLUSTERS", at(4), at(2), 0, 0, 0),
        ("CLUSTER", 0, at(3), 0, 0, 0),
        ("MISCELLANEOUS", at(5), 0, len(misc), 0, 0),
        ("SURFACES", 0, 0, len(surf_payload), surf_count, 0),
        ("VERTEXFRAMES", at(6), 0, len(vf_payload), 1, 0),
        ("TVERTEXFRAMES", 0, 0, len(uv_payload), 1, 0x201),
    ]
    return build_container(nodes, [b"", b"", b"", misc, surf_payload,
                                   vf_payload, uv_payload])


def test_cluster_info_roundtrip(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    blob = _cluster_info_container(_misc_payload(flags=3, surface_index=0),
                                   _vf204_frame(verts, normals), b"",
                                   b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        infos = x.cluster_infos()
        assert len(infos) == 1, infos
        assert infos[0].version == 0x102 and infos[0].flags == 3, infos[0]
        assert infos[0].surface_index == 0 and infos[0].reserved == 0
        assert len(infos[0].payload) == 28, len(infos[0].payload)
        assert infos[0].box_min == (0.0, 0.0, 0.0), infos[0].box_min
        assert infos[0].box_max == (100.0, 200.0, 300.0), infos[0].box_max
        assert infos[0].trailer == 0.0
        c = x.cluster_meshes()[0]
        assert c["surface_index"] == 0 and c["cluster_flags"] == 3, c
        assert c["reference_box_min"] == [0.0, 0.0, 0.0], c
        assert c["reference_box_matches_frame_bounds"] is True, c
        assert x.validate() == [], x.validate()
        assert x.geometry_summary()["distinct_cluster_surfaces"] == 1
        assert x.geometry_summary()["reference_box_clusters"] == 1
        assert x.geometry_summary()["reference_box_mismatch_clusters"] == 0
    finally:
        os.remove(path)


def test_cluster_info_flags_match_uv(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    uvs = [(0, 0), (65535, 0), (0, 65535), (65535, 65535)]
    blob = _cluster_info_container(_misc_payload(flags=0, surface_index=0),
                                   _vf204_frame(verts, normals),
                                   _tv200_frame(uvs), b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        c = x.cluster_meshes()[0]
        assert c["uv_count"] == 4 and c["cluster_flags"] == 0, c
        assert x.validate() == [], x.validate()
    finally:
        os.remove(path)


def test_cluster_info_detects_flags_uv_mismatch(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    # flags == 0 claims a textured cluster, but there is no UV payload
    blob = _cluster_info_container(_misc_payload(flags=0, surface_index=0),
                                   _vf204_frame(verts, normals), b"",
                                   b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("disagrees with UV presence" in p for p in problems), problems
    finally:
        os.remove(path)


def test_cluster_info_detects_surface_index_out_of_range(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    blob = _cluster_info_container(_misc_payload(flags=3, surface_index=2),
                                   _vf204_frame(verts, normals), b"",
                                   b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("surface index 2 >= SURFACES count 1" in p
                   for p in problems), problems
    finally:
        os.remove(path)


def test_cluster_reference_box_equals_frame_bounds(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    blob = _cluster_info_container(
        _misc_payload(flags=3, surface_index=0,
                      box_min=(0.0, 0.0, 0.0), box_max=(100.0, 200.0, 300.0)),
        _vf204_frame(verts, normals), b"", b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert x.validate() == [], x.validate()
        assert x.geometry_summary()["reference_box_clusters"] == 1
        info = x.cluster_infos()[0]
        assert info.encloses((0.0, 0.0, 0.0), (100.0, 200.0, 300.0))
        assert info.as_dict()["box_max"] == [100.0, 200.0, 300.0]
    finally:
        os.remove(path)


def test_cluster_reference_box_detects_frame_bounds_mismatch(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    # the box (max == 1) does not mirror the frame quantization range (max x 100)
    blob = _cluster_info_container(
        _misc_payload(flags=3, surface_index=0,
                      box_min=(0.0, 0.0, 0.0), box_max=(1.0, 1.0, 1.0)),
        _vf204_frame(verts, normals), b"", b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("reference box" in p for p in problems), problems
        assert x.geometry_summary()["reference_box_mismatch_clusters"] == 1
    finally:
        os.remove(path)


def test_cluster_reference_box_decodes_box_and_trailer(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    blob = _cluster_info_container(
        _misc_payload(flags=3, surface_index=0,
                      box_min=(-2.5, -1.0, 0.0), box_max=(150.0, 250.0, 350.0),
                      trailer=0.0),
        _vf204_frame(verts, normals), b"", b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        info = x.cluster_infos()[0]
        assert info.box_min == (-2.5, -1.0, 0.0), info.box_min
        assert info.box_max == (150.0, 250.0, 350.0), info.box_max
        assert info.trailer == 0.0
        assert info.encloses((0.0, 0.0, 0.0), (100.0, 200.0, 300.0))
    finally:
        os.remove(path)


def test_cluster_info_detects_truncated_misc(tmp_dir):
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    normals = [(0, 0, 0)] * 4
    short = struct.pack("<4I", 0x102, 3, 0, 0) + b"\x00" * 20   # 36 bytes
    blob = _cluster_info_container(short, _vf204_frame(verts, normals),
                                   b"", b"PA1_INMATE_P17", 1)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        try:
            x.cluster_infos()
        except mos_xmd.GeometryError:
            pass
        else:
            raise AssertionError("truncated MISCELLANEOUS was accepted")
    finally:
        os.remove(path)


def _keys_payload(pairs, count=None):
    """Build a ``KEYS`` payload: u32 count + length-prefixed, NUL-terminated
    key/value strings. An entry with an empty value stores no value bytes (the
    loader's ``value_len == 0`` case)."""
    out = bytearray()
    out += struct.pack("<I", len(pairs) if count is None else count)
    for key, value in pairs:
        kb = key.encode("latin1")
        out += struct.pack("<I", len(kb)) + kb + b"\x00"
        vb = value.encode("latin1")
        out += struct.pack("<I", len(vb))
        if vb:
            out += vb + b"\x00"
    return bytes(out)


def _keys_container(payload, count=0) -> bytes:
    stride = mos_xmd.NODE_RECORD_LEN
    nodes = [
        ("EXTENDEDMODEL", 0, stride * 2, 0, 0, 0),      # @0x30 -> KEYS @0x60
        ("KEYS", 0, 0, len(payload), count, 0),         # @0x60
    ]
    return build_container(nodes, [b"", payload])


def test_keys_roundtrip(tmp_dir):
    pairs = [("MODEL", "Characters\\Aliens\\Alien2_Big.xmd"),
             ("OGR_RENDERFLAGS", "Triangles+PerPixel"),
             ("NMSHAPE", ""),                      # empty value -> no bytes
             ("XTXPATH", "S:\\Models\\x.xtx")]
    blob = _keys_container(_keys_payload(pairs))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert x.has_keys()
        kvs = x.keys()
        assert [(kv.key, kv.value) for kv in kvs] == pairs, kvs
        assert x.key_map()["NMSHAPE"] == ""
        assert x.key_map()["MODEL"] == pairs[0][1]
        assert x.validate() == [], x.validate()
    finally:
        os.remove(path)


def test_keys_absent_is_ok(tmp_dir):
    nodes = [("EXTENDEDMODEL", 0, 0, 0, 0, 0)]
    blob = build_container(nodes, [b""])
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert not x.has_keys()
        assert x.keys() == []
        assert x.key_map() == {}
        assert x.validate() == [], x.validate()
    finally:
        os.remove(path)


def test_keys_detects_bad_tiling(tmp_dir):
    payload = _keys_payload([("MODEL", "x.xmd")]) + b"\xAA"   # stray byte
    blob = _keys_container(payload)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("keys:" in p for p in problems), problems
    finally:
        os.remove(path)


def test_keys_detects_count_overrun(tmp_dir):
    # Declared count larger than the entries actually present -> overrun.
    payload = _keys_payload([("MODEL", "x.xmd")], count=3)
    blob = _keys_container(payload)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("keys:" in p for p in problems), problems
    finally:
        os.remove(path)


def test_keys_detects_missing_terminator(tmp_dir):
    # Key text without its NUL terminator must be rejected: the byte after the
    # key is 'X', not the terminating NUL the loader expects.
    payload = struct.pack("<I", 1) + struct.pack("<I", 5) + b"MODEL" + b"X" \
        + struct.pack("<I", 0)
    blob = _keys_container(payload)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        problems = x.validate()
        assert any("not NUL terminated" in p for p in problems), problems
    finally:
        os.remove(path)


def test_keys_decodes_latin1_binary_key(tmp_dir):
    # The shipped corpus has one non-printable first key (dweller.xmd); the
    # decoder must stay tolerant and round-trip it (latin1), not crash.
    raw = b"\x9e\x0c\x90\x0c"
    pairs = [(raw.decode("latin1"), "Characters\\Dwellers\\dweller.xmd")]
    blob = _keys_container(_keys_payload(pairs))
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        kv = x.keys()[0]
        assert kv.value == pairs[0][1]
        assert kv.key == raw.decode("latin1")
        assert x.validate() == [], x.validate()
    finally:
        os.remove(path)


def _two_skin_container(vf1, bd1, vf2, bd2, bones):
    """Two VERTEXFRAMES+BONEDEFORM clusters + a shared skeleton.

    Nodes form a flat sibling chain so ``_skin_owner_map`` pairs cluster 0 with
    ``bd1`` and cluster 1 with ``bd2`` (ADR-0426 matrix-map grouping tests)."""
    stride = mos_xmd.NODE_RECORD_LEN

    def at(i):
        return stride * (i + 1)

    np = _flat_nodes(bones)
    idx = _node_indices(range(1, bones))
    nodes = [
        ("VERTEXFRAMES", at(1), 0, len(vf1), 1, 0),
        ("BONEDEFORM", at(2), 0, len(bd1), 0, 0),
        ("VERTEXFRAMES", at(3), 0, len(vf2), 1, 0),
        ("BONEDEFORM", at(4), 0, len(bd2), 0, 0),
        ("NODES", at(5), 0, len(np), bones, 0),
        ("NODEINDICES", 0, 0, len(idx), len(idx) // 2, 0),
    ]
    return build_container(nodes, [vf1, bd1, vf2, bd2, np, idx])


def test_matrix_map_groups_conflicting_palettes(tmp_dir):
    # Two clusters share matrix_map_id 0 but carry different palettes: the id is
    # NOT a shared bone-set registry key (ADR-0426).
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    bd1 = _bonedeform(vrecs, [(0, 255), (1, 255)],
                      matrix_map_id=0, matrix_bones=b"\x01\x02")
    bd2 = _bonedeform(vrecs, [(0, 255), (1, 255)],
                      matrix_map_id=0, matrix_bones=b"\x01\x03")
    vf = _vf204_frame(verts, normals)
    blob = _two_skin_container(vf, bd1, vf, bd2, bones=8)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        assert x.validate() == [], x.validate()
        st = x.geometry_stats()
        assert st["matrix_map_ids"] == {"0": 2}, st["matrix_map_ids"]
        assert st["matrix_map_shared_id_groups"] == 1, st
        assert st["matrix_map_palette_conflict_groups"] == 1, st
        groups = x.matrix_map_groups()
        assert len(groups) == 1 and groups[0]["matrix_map_id"] == 0, groups
        assert groups[0]["clusters"] == [0, 1], groups
        assert groups[0]["palette_count"] == 2, groups
        assert groups[0]["palettes_agree"] is False, groups
    finally:
        os.remove(path)


def test_matrix_map_groups_agreeing_palettes(tmp_dir):
    # Same id *and* same palette: agree, no conflict. Empty-palette clusters are
    # ignored entirely.
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    pal = b"\x01\x02"
    bd1 = _bonedeform(vrecs, [(0, 255), (1, 255)],
                      matrix_map_id=3, matrix_bones=pal)
    bd2 = _bonedeform(vrecs, [(0, 255), (1, 255)],
                      matrix_map_id=3, matrix_bones=pal)
    vf = _vf204_frame(verts, normals)
    blob = _two_skin_container(vf, bd1, vf, bd2, bones=8)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        groups = x.matrix_map_groups()
        assert len(groups) == 1 and groups[0]["palettes_agree"] is True, groups
        st = x.geometry_stats()
        assert st["matrix_map_shared_id_groups"] == 1, st
        assert st["matrix_map_palette_conflict_groups"] == 0, st
        assert st["matrix_map_ids"] == {"3": 2}, st["matrix_map_ids"]
    finally:
        os.remove(path)


def test_matrix_map_groups_ignores_direct_clusters(tmp_dir):
    # A direct-node cluster (empty palette) has no bone matrix map and must not
    # contribute a group.
    verts = [(0, 0, 0), (1, 0, 0)]
    normals = [(0, 0, 0)] * 2
    vrecs = [(0, 1, 0), (0, 1, 1)]
    pal = b"\x01\x02"
    bd1 = _bonedeform(vrecs, [(0, 255), (1, 255)],
                      matrix_map_id=0, matrix_bones=pal)
    bd2 = _bonedeform(vrecs, [(0, 255), (1, 255)],
                      matrix_map_id=-1, matrix_bones=())
    vf = _vf204_frame(verts, normals)
    blob = _two_skin_container(vf, bd1, vf, bd2, bones=8)
    path = _write_tmp(blob)
    try:
        x = mos_xmd.Xmd(path)
        groups = x.matrix_map_groups()
        assert len(groups) == 1 and groups[0]["clusters"] == [0], groups
        st = x.geometry_stats()
        assert st["matrix_map_shared_id_groups"] == 0, st
        assert st["matrix_map_ids"] == {"0": 1}, st["matrix_map_ids"]
    finally:
        os.remove(path)


def test_write_tmp_is_isolated(tmp_dir):
    # GH #82: synthetic models must not land in the OS temp root, or a corpus
    # scan of `os.path.dirname(path)` walks unrelated files there.
    path = _write_tmp(b"\x00" * 16)
    try:
        assert os.path.dirname(path) == _TMP_ROOT, path
        assert os.path.abspath(os.path.dirname(path)) != \
            os.path.abspath(tempfile.gettempdir()), path
        # An explicit directory wins (the tests' own TemporaryDirectory).
        other = _write_tmp(b"\x00" * 16, tmp_dir)
        try:
            assert os.path.dirname(other) == os.path.abspath(tmp_dir), other
        finally:
            os.remove(other)
    finally:
        os.remove(path)


def main() -> int:
    tests = (test_roundtrip, test_rejects_bad_magic,
             test_geometry_roundtrip, test_geometry_detects_index_out_of_range,
             test_geometry_detects_size_mismatch,
             test_geometry_roundtrip_205, test_geometry_204_has_no_extra_basis,
             test_geometry_205_truncated_basis_is_rejected,
             test_uv_roundtrip, test_uv_detects_count_mismatch,
             test_uv_detects_bad_tiling, test_uv_empty_chunk_is_ok,
             test_skin_roundtrip, test_skin_matrix_map_and_influences_of,
             test_skin_direct_bone_space_without_palette,
             test_skin_id_minus_one_with_palette_still_palette,
             test_skin_palette_index_out_of_range,
             test_skin_palette_entry_out_of_range,
             test_skin_palette_duplicate_entries,
             test_skin_palette_unused_entries_ok,
             test_skin_detects_prefix_sum_mismatch,
             test_skin_detects_vertex_count_mismatch,
             test_skin_detects_bone_out_of_range,
             test_skin_bad_matrix_map_tiling, test_skin_absent_is_ok,
             test_skeleton_roundtrip, test_skeleton_detects_bad_child_count,
             test_skeleton_detects_bad_node_indices,
             test_skeleton_detects_size_mismatch,
             test_skeleton_detects_padding_not_zeroed,
             test_skeleton_detects_bad_version, test_skeleton_absent_is_ok,
             test_skeleton_child_table,
             test_skeleton_detects_child_range_overrun,
             test_skeleton_detects_parent_table_mismatch,
             test_boxtree_nodes_is_not_a_skeleton,
             test_skeleton_selects_skel_nodes_over_boxtree,
             test_cluster_info_roundtrip,
             test_cluster_info_flags_match_uv,
             test_cluster_info_detects_flags_uv_mismatch,
             test_cluster_info_detects_surface_index_out_of_range,
             test_cluster_reference_box_equals_frame_bounds,
             test_cluster_reference_box_detects_frame_bounds_mismatch,
             test_cluster_reference_box_decodes_box_and_trailer,
             test_cluster_info_detects_truncated_misc,
             test_keys_roundtrip, test_keys_absent_is_ok,
             test_keys_detects_bad_tiling, test_keys_detects_count_overrun,
             test_keys_detects_missing_terminator,
             test_keys_decodes_latin1_binary_key,
             test_matrix_map_groups_conflicting_palettes,
             test_matrix_map_groups_agreeing_palettes,
             test_matrix_map_groups_ignores_direct_clusters,
             test_write_tmp_is_isolated)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            t(d)
            print(f"ok   {t.__name__}")
        test_detects_dangling_pointer()
        print("ok   test_detects_dangling_pointer")
    print("PASS mos_xmd structural tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
