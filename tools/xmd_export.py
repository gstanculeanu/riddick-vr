#!/usr/bin/env python3
"""Export decoded mesh + skin data from Starbreeze ``MOS DATAFILE2.0`` (``.xmd``).

Read-only companion to :mod:`tools.mos_xmd`. That parser already decodes the
container, the per-cluster geometry/UV/skinning and the skeleton; this tool turns
the decoded data into portable artifacts so a native render path can be built
without re-deriving the binary format (GH #55 -> GH #46):

  * ``<prefix>_c<NN>.obj``      one Wavefront OBJ (``v``/``vn``/``vt``/``f``) per
                                cluster, in the engine's raw model space;
  * ``<prefix>.obj``            (``--combined``) every exported cluster in one
                                OBJ (indices rebased per cluster);
  * ``<prefix>.skin.json``      the skeleton hierarchy + per-vertex bone
                                influences for every exported cluster;
  * ``<prefix>.export.json``    a compact manifest (counts, bounds, bones used)
                                that is small enough to commit as evidence.

Coordinate system: positions are the engine's raw model-space floats and are
**not** axis-converted, so the data stays faithful to the game. The Riddick
skeleton is **Z-up** (spine translations increase in Z); consumers convert to
whatever convention their renderer wants. The manifest records this.

Skinning caveat: ``.xmd`` stores only bind-pose node **translations** plus the
parent/child hierarchy. Node **rotations** are not in the file at all — ``KEYS``
is a string metadata map (see ADR-0421), and animation lives in the
``.xsa``/``.xag`` animation groups (or the engine's live skeleton), so a consumer
that poses the mesh must supply the bone rotations itself. The exporter states
this explicitly in the ``.skin.json``.

Usage::

    python tools/xmd_export.py <file.xmd> --list
    python tools/xmd_export.py <file.xmd> --out DIR [--cluster N ...] [--combined]
    python tools/xmd_export.py <file.xmd> --out DIR --json
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mos_xmd import (  # noqa: E402  (path injected above)
    BoneDeform, GeometryError, Skeleton, Xmd, XmdError)

# Recorded in the manifest so a consumer never has to guess the convention.
COORDINATE_SYSTEM = "engine model space (Z-up), unconverted"
ROTATIONS_NOTE = (
    "NODES stores bind-pose translations only; bone rotations are not in the "
    ".xmd at all (KEYS is a string metadata map; animation lives in the "
    ".xsa/.xag groups or the engine's live skeleton) and must be supplied by "
    "the caller"
)


class ClusterMesh:
    """One decoded cluster ready for export."""

    __slots__ = ("index", "frames", "positions", "normals", "uvs", "triangles",
                 "tri_count", "skin", "bounds_min", "bounds_max")

    def __init__(self, index, frames, positions, normals, uvs, triangles,
                 skin, bounds_min, bounds_max):
        self.index = index
        self.frames = frames                    # number of decoded VF frames
        self.positions = positions              # list[(x, y, z)]
        self.normals = normals                  # list[(x, y, z)]
        self.uvs = uvs                          # list[(u, v)] or None
        self.triangles = triangles              # flat tuple of vertex indices
        self.tri_count = len(triangles) // 3
        self.skin = skin                        # BoneDeform or None
        self.bounds_min = bounds_min
        self.bounds_max = bounds_max

    @property
    def vertex_count(self) -> int:
        return len(self.positions)

    def bones_used(self) -> list[int]:
        """Distinct **skeleton node** indices referenced by this cluster's skin.

        Applies the BONEDEFORM ``matrix_bones`` palette (ADR-0423): influence
        bytes are palette indices when the palette is present, node indices
        otherwise."""
        if self.skin is None:
            return []
        return sorted({self.skin.resolve_bone(bone)
                       for bone, _w in self.skin.influences})

    @property
    def bone_space(self) -> str:
        return self.skin.bone_space() if self.skin else "none"


def vertex_frame_nodes(x: Xmd):
    """``VERTEXFRAMES`` nodes in traversal order (one per cluster)."""
    return [n for n in x.walk_linked() if n.name == "VERTEXFRAMES"]


def _sibling_nodes(x: Xmd, name: str):
    return [n for n in x.walk_linked() if n.name == name]


def triangle_nodes(x: Xmd):
    """The *mesh* ``TRIANGLES`` chunks in traversal order (GH #86).

    ``TRIANGLES`` is not exclusive to the mesh: the 2009 models also carry one
    under a ``BOXTREE`` bounding-volume node, in a different layout, and
    ``CDataFile::GetNext(cluster, "TRIANGLES")`` only ever sees a sibling of the
    cluster. Pairing clusters with ``[n for n in walk_linked() if ...]`` used to
    pick the ``BOXTREE`` leaf and mis-align every later cluster (or fail to
    decode it), so this defers to :meth:`mos_xmd.Xmd.triangle_nodes`."""
    return x.triangle_nodes()


def decode_cluster(x: Xmd, index: int) -> ClusterMesh:
    """Decode cluster ``index`` (VERTEXFRAMES/TVERTEXFRAMES/BONEDEFORM/TRIANGLES)."""
    vf_nodes = vertex_frame_nodes(x)
    if not 0 <= index < len(vf_nodes):
        raise XmdError(f"cluster {index} out of range (0..{len(vf_nodes) - 1})")
    vn = vf_nodes[index]
    frames = x.vertex_frames(vn)
    if not frames:
        raise GeometryError(f"cluster {index}: no vertex frames")
    frame = frames[-1]
    positions = list(frame.positions)
    normals = list(frame.normals)

    uvs = None
    tv_nodes = _sibling_nodes(x, "TVERTEXFRAMES")
    if index < len(tv_nodes):
        uv_frames = x.tvertex_frames(tv_nodes[index])
        if uv_frames:
            uvs = list(uv_frames[-1].uvs)
            if len(uvs) != len(positions):
                raise GeometryError(
                    f"cluster {index}: {len(uvs)} UVs != {len(positions)} verts")

    triangles: tuple[int, ...] = ()
    tri_nodes = triangle_nodes(x)
    if index < len(tri_nodes):
        _count, triangles = x.triangle_indices(tri_nodes[index])

    skin = None
    owner = x._skin_owner_map()
    if vn.offset in owner:
        skin = x.bone_deform(owner[vn.offset])

    if positions:
        mn = tuple(min(p[i] for p in positions) for i in range(3))
        mx = tuple(max(p[i] for p in positions) for i in range(3))
    else:
        mn = mx = (0.0, 0.0, 0.0)
    return ClusterMesh(index, len(frames), positions, normals, uvs, triangles,
                       skin, mn, mx)


def _g(v: float) -> str:
    return f"{v:.6g}"


def mesh_obj(mesh: ClusterMesh, object_name: str | None = None) -> str:
    """Render one cluster as Wavefront OBJ text (1-based indices)."""
    lines: list[str] = []
    if object_name:
        lines.append(f"# exported by tools/xmd_export.py ({COORDINATE_SYSTEM})")
        lines.append(f"o {object_name}")
    for (px, py, pz) in mesh.positions:
        lines.append(f"v {_g(px)} {_g(py)} {_g(pz)}")
    has_n = len(mesh.normals) == mesh.vertex_count and mesh.vertex_count > 0
    if has_n:
        for (nx, ny, nz) in mesh.normals:
            lines.append(f"vn {_g(nx)} {_g(ny)} {_g(nz)}")
    has_uv = mesh.uvs is not None and len(mesh.uvs) == mesh.vertex_count
    if has_uv:
        for (u, v) in mesh.uvs:
            lines.append(f"vt {_g(u)} {_g(v)}")
    idx = mesh.triangles
    for t in range(0, len(idx) - 2, 3):
        a, b, c = idx[t] + 1, idx[t + 1] + 1, idx[t + 2] + 1
        if has_uv and has_n:
            lines.append(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}")
        elif has_uv:
            lines.append(f"f {a}/{a} {b}/{b} {c}/{c}")
        elif has_n:
            lines.append(f"f {a}//{a} {b}//{b} {c}//{c}")
        else:
            lines.append(f"f {a} {b} {c}")
    return "\n".join(lines) + "\n"


def combined_obj(meshes, object_name: str) -> str:
    """One OBJ containing every mesh, with per-cluster index rebasing."""
    lines = [f"# exported by tools/xmd_export.py ({COORDINATE_SYSTEM})"]
    if object_name:
        lines.append(f"o {object_name}")
    base = 0
    has_any_normals = any(len(m.normals) == m.vertex_count and m.vertex_count
                          for m in meshes)
    for m in meshes:
        for (px, py, pz) in m.positions:
            lines.append(f"v {_g(px)} {_g(py)} {_g(pz)}")
    if has_any_normals:
        for m in meshes:
            for (nx, ny, nz) in m.normals:
                lines.append(f"vn {_g(nx)} {_g(ny)} {_g(nz)}")
    for m in meshes:
        if m.uvs is not None and len(m.uvs) == m.vertex_count:
            for (u, v) in m.uvs:
                lines.append(f"vt {_g(u)} {_g(v)}")
    for m in meshes:
        uv_ok = m.uvs is not None and len(m.uvs) == m.vertex_count
        lines.append(f"g {object_name}_cluster{m.index:02d}")
        for t in range(0, len(m.triangles) - 2, 3):
            a = m.triangles[t] + 1 + base
            b = m.triangles[t + 1] + 1 + base
            c = m.triangles[t + 2] + 1 + base
            if uv_ok:
                lines.append(f"f {a}/{a} {b}/{b} {c}/{c}")
            else:
                lines.append(f"f {a} {b} {c}")
        base += m.vertex_count
    return "\n".join(lines) + "\n"


def skeleton_dict(x: Xmd) -> dict | None:
    """Serialise the decoded skeleton (hierarchy + bind translations)."""
    sk: Skeleton | None = x.skeleton()
    if sk is None:
        return None
    return {
        "node_count": len(sk.nodes),
        "used_nodes": len(sk.used_nodes()),
        "padding_nodes": len(sk.unused_nodes()),
        "node_indices": list(sk.node_indices),
        "tree": sk.is_tree(),
        "rotations_available": False,
        "rotations_note": ROTATIONS_NOTE,
        "nodes": [
            {
                "index": n.index,
                "parent": n.parent,
                "translation": list(n.translation),
                "child_start": n.child_start,
                "child_count": n.child_count,
                "children": sk.children_of(n.index),
                "rotation_slot": n.rotation_slot,
                "movement_slot": n.movement_slot,
                "used": n.is_used(),
            }
            for n in sk.nodes
        ],
    }


def cluster_skin_dict(mesh: ClusterMesh) -> dict | None:
    """Serialise one cluster's per-vertex bone influences."""
    sk: BoneDeform | None = mesh.skin
    if sk is None:
        return None
    return {
        "index": mesh.index,
        "vertex_count": sk.vertex_count,
        "influence_count": sk.influence_count,
        "max_influences_per_vertex": sk.max_influences(),
        "bone_space": sk.bone_space(),
        "max_bone": sk.max_bone(),
        "max_resolved_bone": sk.max_resolved_bone(),
        "matrix_map_id": sk.matrix_map_id,
        "matrix_bone_count": len(sk.matrix_bones),
        "matrix_bones": list(sk.matrix_bones),
        # Raw influence bytes: a palette index when matrix_bones is non-empty,
        # a skeleton node index otherwise (ADR-0423).
        "influences": [
            [[bone, w] for bone, w in sk.influences_of(i)]
            for i in range(sk.vertex_count)
        ],
        # The same weights with the palette already applied (skeleton nodes).
        "resolved_influences": [
            [[node, w] for node, w in sk.resolved_influences_of(i)]
            for i in range(sk.vertex_count)
        ],
    }


def manifest(x: Xmd, meshes) -> dict:
    """Compact, committable summary (no bulk vertex data)."""
    try:
        infos = x.cluster_infos()
    except GeometryError:
        infos = []
    surface_of = {i: info.surface_index for i, info in enumerate(infos)}
    clusters = []
    for m in meshes:
        bones = m.bones_used()
        clusters.append({
            "index": m.index,
            "vertices": m.vertex_count,
            "triangles": m.tri_count,
            "uvs": len(m.uvs) if m.uvs is not None else 0,
            "vertex_frames": m.frames,
            "bounds_min": list(m.bounds_min),
            "bounds_max": list(m.bounds_max),
            "surface_index": surface_of.get(m.index),
            "skinned": m.skin is not None,
            "influences": m.skin.influence_count if m.skin else 0,
            "bone_space": m.bone_space,
            "bones_used": bones,
            "bone_count_used": len(bones),
        })
    sk = x.skeleton()
    return {
        "source": os.path.abspath(x.path),
        "source_size": len(x.data),
        "coordinate_system": COORDINATE_SYSTEM,
        "rotations_available": False,
        "surfaces": x.surface_names(),
        "skeleton": {
            "node_count": len(sk.nodes) if sk else 0,
            "used_nodes": len(sk.used_nodes()) if sk else 0,
            "index_count": len(sk.node_indices) if sk else 0,
        } if sk else None,
        "cluster_count": len(clusters),
        "clusters": clusters,
        "totals": {
            "vertices": sum(c["vertices"] for c in clusters),
            "triangles": sum(c["triangles"] for c in clusters),
            "uvs": sum(c["uvs"] for c in clusters),
            "skinned_clusters": sum(1 for c in clusters if c["skinned"]),
        },
    }


def _json_dump(path: str, doc: dict) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")


def export_model(path: str, out_dir: str, prefix: str | None = None,
                 clusters: list[int] | None = None, combined: bool = False,
                 want_skin: bool = True) -> dict:
    """Decode and write the requested clusters. Returns a manifest dict."""
    x = Xmd(path)
    total = len(vertex_frame_nodes(x))
    if clusters is None:
        clusters = list(range(total))
    meshes = [decode_cluster(x, i) for i in clusters]
    stem = prefix or os.path.splitext(os.path.basename(path))[0]
    os.makedirs(out_dir, exist_ok=True)

    written: list[str] = []
    for m in meshes:
        p = os.path.join(out_dir, f"{stem}_c{m.index:02d}.obj")
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(mesh_obj(m, object_name=f"{stem}_cluster{m.index:02d}"))
        written.append(p)
    if combined:
        p = os.path.join(out_dir, f"{stem}.obj")
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(combined_obj(meshes, stem))
        written.append(p)
    if want_skin:
        doc = {
            "source": os.path.abspath(path),
            "coordinate_system": COORDINATE_SYSTEM,
            "skeleton": skeleton_dict(x),
            "clusters": [cluster_skin_dict(m) for m in meshes],
        }
        p = os.path.join(out_dir, f"{stem}.skin.json")
        _json_dump(p, doc)
        written.append(p)

    man = manifest(x, meshes)
    man["written"] = [os.path.basename(p) for p in written]
    p = os.path.join(out_dir, f"{stem}.export.json")
    _json_dump(p, man)
    written.append(p)
    man["manifest_path"] = p
    return man


def _summarise(x: Xmd) -> list[dict]:
    out = []
    for i, vn in enumerate(vertex_frame_nodes(x)):
        m = decode_cluster(x, i)
        out.append(manifest(x, [m])["clusters"][0])
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="a .xmd file")
    ap.add_argument("--out", metavar="DIR", help="output directory")
    ap.add_argument("--prefix", metavar="NAME",
                    help="output filename prefix (default: model stem)")
    ap.add_argument("--cluster", type=int, action="append", metavar="N",
                    help="export only cluster N (repeatable; default: all)")
    ap.add_argument("--combined", action="store_true",
                    help="also write one combined OBJ")
    ap.add_argument("--no-skin", action="store_true",
                    help="skip the per-vertex .skin.json")
    ap.add_argument("--list", action="store_true",
                    help="list clusters with counts/bounds/bones (no writing)")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    args = ap.parse_args(argv)

    try:
        if args.list:
            x = Xmd(args.path)
            rows = _summarise(x)
            if args.json:
                print(json.dumps(rows, indent=2))
            else:
                print(f"== {os.path.basename(args.path)}: "
                      f"{len(rows)} clusters, "
                      f"surfaces={x.surface_names()}")
                for c in rows:
                    print(f"   cluster {c['index']:2d}: "
                          f"verts={c['vertices']:5d} tris={c['triangles']:5d} "
                          f"uvs={c['uvs']:5d} skinned={c['skinned']!s:5} "
                          f"bones={c['bone_count_used']} "
                          f"bbox=({c['bounds_min']} .. {c['bounds_max']})")
            return 0

        if not args.out:
            ap.error("--out DIR is required unless --list is used")
        man = export_model(args.path, args.out, prefix=args.prefix,
                           clusters=args.cluster, combined=args.combined,
                           want_skin=not args.no_skin)
        if args.json:
            print(json.dumps(man, indent=2))
        else:
            t = man["totals"]
            print(f"== {os.path.basename(args.path)} -> {args.out}")
            print(f"   clusters={man['cluster_count']}  "
                  f"vertices={t['vertices']}  triangles={t['triangles']}  "
                  f"uvs={t['uvs']}  skinned_clusters={t['skinned_clusters']}")
            for c in man["clusters"]:
                print(f"   cluster {c['index']:2d}: verts={c['vertices']:5d} "
                      f"tris={c['triangles']:5d} uvs={c['uvs']:5d} "
                      f"bones={c['bone_count_used']}")
            print("   wrote: " + ", ".join(man["written"]))
        return 0
    except (XmdError, GeometryError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
