#!/usr/bin/env python3
"""Read-only parser for Starbreeze "MOS DATAFILE2.0" ``.xmd`` model containers.

The retail Riddick (Escape from Butcher Bay) character/weapon models are
``MOS DATAFILE2.0`` files. Layout recovered two ways:

  * by static inspection of the on-disk bytes, and
  * by decompiling the shipped loader ``CDataFile::Open`` (MCCDyn.dll
    @0x10032FB0) and ``CDataFile::ReadTree_r`` (@0x10032840) with Ghidra
    (see ``docs/adr/0414-mos-datafile2-loader-container.md``).

The container *is* a node tree, and the file header is simply the **root
node record**:

  root node : 0x30-byte record at file offset 0
      +0x00  char[0x18] name = "MOS DATAFILE2.0\\0" (NUL padded to 24)
      +0x18  u32        next sibling node offset   (0 = none) == first node
      +0x1C  u32        first child node offset     (0 = none)
      +0x20  u32        data offset (absolute file offset) == data region start
      +0x24  u32        data size in bytes
      +0x28  u32        element/frame count
      +0x2C  u32        flags (e.g. TVERTEXFRAMES = 0x201)
  node      : same 0x30-byte record shape, linked by sibling (+0x18) and
              child (+0x1C) offsets into an in-file tree.
  data      : chunk payloads; for the shipped models they tile the region
              ``[data_start, EOF)`` exactly (no gaps, no overlaps).

Because the root name field holds the magic, the loader's 16-byte magic
compare at file 0x00 reads the root node's first 16 name bytes. ``node_start``
and ``data_start`` below are therefore just ``root.next_sibling`` and
``root.data_offset``; they are not separate header fields.

This parser is intentionally dependency-free and strictly read-only: it never
writes next to the game install. It backs the native first-person hand/arm
investigation (docs/adr/0413) and the REA track.

Usage:
    python tools/mos_xmd.py <file.xmd> [--json] [--strings] [--tree]
    python tools/mos_xmd.py <file.xmd> --mesh [--cluster N] [--json]
    python tools/mos_xmd.py <file.xmd> --skeleton [--json]
    python tools/mos_xmd.py --inventory <root-dir> [--json]
    python tools/mos_xmd.py <file.xmd> --check
    python tools/mos_xmd.py --inventory <root-dir> --check

Geometry (recovered from MXR.dll ``CTriangleMeshCore::Read`` +
``CVertexFrame::Read`` + ``CVertexFrame204::Read`` + the TVERTEXFRAMES reader
``FUN_100807d0``, see ``docs/adr/0415-xmd-vertex-frame-geometry.md`` and
``docs/adr/0416-xmd-uv-tvertexframes.md``):

  VERTEXFRAMES : ``u32 frame_count`` (only when the model version is > 0x100),
                 then ``frame_count`` frames. Each decoded frame (version 0x204,
                 used by every shipped model) is::

                     u32  version            = 0x204
                     u32  header20           (unknown; likely a scale/flag)
                     f32  bounds_min[3]      (quantization lower bound)
                     f32  bounds_max[3]      (quantization upper bound)
                     f32  aux[3]             (third header vector, unread by paths)
                     u32  header30           (unknown; likely a scale/flag)
                     u32  vertex_count  n
                     u16  positions[n][3]    (x,y,z in 0..65535)
                     s8   normals[n][3]      (components in -127..127)

                 positions are dequantized with
                 ``min + q * (max - min) / 65535`` and normals with
                 ``c / 127`` re-normalized (exactly as the loader does).

  TRIANGLES    : ``u32 triangle_count`` then ``triangle_count`` *u16* triplets.

  TVERTEXFRAMES: one quantized UV frame, recovered from MXR.dll
                 ``FUN_100807d0`` (the version-0x200 branch, which every
                 shipped asset selects)::

                     u32  uv_count  n
                     f32  min_u
                     f32  min_v
                     f32  max_u
                     f32  max_v
                     u16  uv[n][2]           (u,v in 0..65535)

                 ``u = min_u + q_u * (max_u - min_u) / 65535`` (and *v*
                 likewise) with the same ``1.5259022e-05`` reciprocal. Size is
                 exactly ``20 + 4n``. A cluster with **no** UVs stores an empty
                 (``data_size == 0``) ``TVERTEXFRAMES`` node.

  BONEDEFORM   : per-vertex skinning, recovered from MXR.dll
                 ``FUN_1007d260`` (the embedded form selected whenever the
                 model version is > 0x100). Layout::

                     u32  vertex_count    N
                     N * { u8 flags; u8 influence_count; u16 influence_start }
                     u32  influence_count M
                     M * { u8 bone; u8 weight_q }        (weight = q / 255)
                     u32  matrix_map_id   (s32; 0xFFFFFFFF = none)
                     u32  matrix_bone_count B
                     B * u8               (bone matrix-map palette indices)

                 ``influence_start`` is the running prefix sum of
                 ``influence_count`` (the reader repairs it in place), so a
                 vertex's influences are ``influences[start:start+count]``.
                 A cluster with **no** skinning omits the ``BONEDEFORM`` node.
                 The trailing matrix-map block is present for every shipped
                 asset (model version 0x302); it is validated by ``--check``.

                 **Field identity** (ADR-0426): the trailing block *is* the
                 engine's **bone matrix map** (legacy chunk ``BONEMATRIXMAP``).
                 ``CTriangleMeshCore::Read`` (`FUN_10080f50`) stores it at mesh
                 ``+0x24`` and its id at ``+0x28``; the reader does **not** remap
                 the influence bytes through it (``FUN_1007c150`` stores the raw
                 ``u8`` verbatim), so palette->node resolution happens in the
                 runtime skinning consumer, not at load time.

                 **Bone-index space** (ADR-0423): an influence's ``bone`` byte
                 is *not* always a skeleton node index. When the trailing
                 ``matrix_bones`` palette is **non-empty** the byte is an index
                 into that palette and the skeleton node is
                 ``matrix_bones[bone]``; when the palette is **empty** the byte
                 is the node index directly (``matrix_map_id`` is ``-1`` for
                 map-less clusters but is *not* a reliable discriminator — 33
                 shipped clusters store ``-1`` *and* a palette, so the palette
                 length alone decides). Use
                 :meth:`BoneDeform.resolve_bone` /
                 :meth:`BoneDeform.resolved_influences_of`.

  Clusters     : one ``VERTEXFRAMES``/``TVERTEXFRAMES``/``TRIANGLES`` triple per
                 submesh, in traversal order; every index must be < the paired
                 frame's vertex count and a non-empty UV frame's count must
                 equal its position frame's count (validated by ``--check``).
                 Each ``CLUSTER``'s ``MISCELLANEOUS`` header additionally mirrors
                 the cluster frame's quantization range as a 6-float reference
                 bounding box (ADR-0425; see ``ClusterInfo``).

  MISCELLANEOUS: the 44-byte per-cluster header (see ``ClusterInfo`` /
                 :func:`decode_cluster_info`)::

                     u32  version         (0x102 on every shipped cluster)
                     u32  flags           (0 exactly when a UV set is present)
                     u32  surface_index   (< the SURFACES count)
                     u32  reserved        (0 on every shipped cluster)
                     f32  box_min[3]      reference bound, lower corner
                     f32  box_max[3]      reference bound, upper corner
                     f32  trailer         (0.0 on every shipped cluster)

                 The six floats are **exactly** the paired ``VERTEXFRAMES``
                 frame's quantization range (``bounds_min``/``bounds_max``,
                 ADR-0415): they match on all 4662 shipped clusters, so the
                 ``MISCELLANEOUS`` header mirrors the cluster's reference /
                 dequantization box. That box encloses the decoded geometry, but
                 it is a **bound, not a matrix** - the per-cluster
                 reference/bind-inverse transform is not stored here (GH #59).
                 ``--check`` enforces the box == frame-bounds equality. See
                 ``ClusterInfo``.

  SKELETON     : the ``NODES`` and ``NODEINDICES`` chunks, recovered from
                 MXR.dll ``FUN_100219d0`` (skeleton reader), ``FUN_1001f7a0``
                 (writer) and the node record reader/writer ``FUN_1001fc30`` /
                 ``FUN_1001eec0`` (see
                 ``docs/adr/0418-xmd-skeleton-nodes-nodeindices.md``). ``NODES``
                 is an array of fixed **34-byte** records (version ``0x100``)::

                     u16  version           = 0x100
                     f32  translation[3]    bind-pose model-space position
                     u16  reserved          (0 in every shipped asset)
                     u16  child_start       first child's slot in NODEINDICES
                     u16  child_count       number of direct children
                     u16  parent            parent index (0..N-1)
                     f32  scale[2]           (1.0f, 1.0f in every shipped asset)
                     u16  rotation_slot     == node index; 0xFFFF = padding
                     u16  movement_slot     < 0x20, or 0xFFFF for "none"

                 A record with ``rotation_slot == 0xFFFF`` is a **padding** node
                 (also ``movement_slot == 0xFFFF``, zero translation,
                 ``parent == 0``, ``child_count == 0``) and is excluded from
                 ``NODEINDICES``.

                 ``NODEINDICES`` is the **flattened child table**: a ``u16`` per
                 entry, consumed as ``node.child_count`` consecutive values at
                 ``node.child_start``. The used nodes' ``[child_start,
                 child_start + child_count)`` ranges tile the whole table exactly,
                 so ``node.children == NODEINDICES[child_start:child_start+count]``
                 and ``len(NODEINDICES) == used_nodes - 1`` — this is exactly how
                 the load-time hierarchy builder ``FUN_1001e040`` walks the tree.
                 Validated by ``--check`` on all 249 shipped skeletons.

                 The name ``NODES`` is **not** exclusive to the skeleton: the
                 shipped 2009 *Assault on Dark Athena* models also carry a 12-byte
                 ``NODES`` chunk under a ``BOXTREE`` bounding-volume node, which
                 precedes the skeleton in node-table order. ``skeleton()`` and
                 ``skeleton_node()`` therefore select the ``SKELETON`` child (or
                 the ``NODES`` with a ``NODEINDICES`` sibling / 34-byte tiling)
                 rather than the first ``NODES``; a container with no qualifying
                 chunk yields ``None`` (GH #84).

  KEYS         : a flat **string metadata map** (build provenance, texture and
                 render options) — **not** animation. The model reader
                 ``FUN_10083060`` calls ``CDataFile::GetNext(...,"KEYS")`` and
                 hands the payload to ``CKeyContainer::Read``
                 (MCCDyn.dll @0x100195d0), which reads it as (see
                 ``docs/adr/0421-xmd-keys-metadata-map.md``)::

                     u32  count
                     count * {
                         u32  key_len
                         char key[key_len + 1]       (NUL terminated)
                         u32  value_len
                         char value[value_len + 1]   (NUL terminated;
                                                      zero bytes when value_len == 0)
                     }

                 Every shipped model carries exactly one ``KEYS`` chunk. Across
                 all 631 models its **54 distinct keys** are asset/texture/render
                 metadata (``MODEL``, ``XTXPATH``, ``OGR_RENDERFLAGS``,
                 ``NORMALMAP_*``, ``GI_*``, ``VERTEXSPLITANGLE``, ...). It holds
                 **no bone rotations**: ``.xmd`` stores bind-pose translations
                 only, so a consumer that poses the mesh must obtain per-bone
                 rotations from the ``.xsa``/``.xag`` animation groups or from
                 the engine's live skeleton. Validated by ``--check`` on all 631
                 shipped models.

This parser is intentionally dependency-free and strictly read-only: it never
writes next to the game install. It backs the native first-person hand/arm
investigation (docs/adr/0413) and the REA track.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

MAGIC = b"MOS DATAFILE2.0\x00"
# Node names live in a 0x18-byte field (see module docstring).
NODE_NAME_LEN = 0x18
NODE_RECORD_LEN = 0x30
NAME_RE = re.compile(rb"^[A-Za-z][A-Za-z0-9_ ]{1,23}$")
# Chunks whose payload we know how to sanity-check (first u32 == element count).
COUNT_PREFIX_CHUNKS = {"TRIANGLES", "PRIMITIVES", "EDGES", "EDGETRIS", "TRIEDGES"}

# VERTEXFRAMES payload versions (recovered from MXR.dll, see ADR-0415). Every
# shipped model uses 0x204 (quantized u16 positions + i8 normals); 0x202/0x203
# share its shape but store raw (unpacked) records, and are implemented from the
# same decompilation though no shipped asset exercises them.
VERTEX_FRAME_VERSIONS = (0x200, 0x201, 0x202, 0x203, 0x204)
# Quantized-position readers we can fully decode from the decompiled loader.
QUANTIZED_VF_VERSIONS = (0x202, 0x203, 0x204)
# 1/65535: u16 position component -> fraction of the bounding-box extent.
_Q16_SCALE = 1.0 / 65535.0
# 1/127: i8 normal component scale (re-normalized after decode).
_I8_NORMAL_SCALE = 1.0 / 127.0
# TVERTEXFRAMES payload (MXR.dll FUN_100807d0). Every shipped asset takes the
# version-0x200 branch: one quantized UV frame, size == 20 + 4n (see docstring).
TVERTEX_FRAME_VERSION = 0x200


class XmdError(Exception):
    pass


class GeometryError(XmdError):
    pass


class VertexFrame:
    """One decoded ``VERTEXFRAMES`` frame (a static pose / blend target)."""

    __slots__ = ("version", "count", "bounds_min", "bounds_max", "aux",
                 "header_extra", "positions", "normals", "size", "exact")

    def __init__(self, version, count, bounds_min, bounds_max, aux,
                 header_extra, positions, normals, size, exact):
        self.version = version
        self.count = count
        self.bounds_min = bounds_min
        self.bounds_max = bounds_max
        self.aux = aux
        self.header_extra = header_extra
        self.positions = positions
        self.normals = normals
        self.size = size
        self.exact = exact

    def as_dict(self, with_geometry=False):
        d = {
            "version": self.version,
            "vertex_count": self.count,
            "bounds_min": list(self.bounds_min),
            "bounds_max": list(self.bounds_max),
            "size": self.size,
            "exact": self.exact,
        }
        if with_geometry:
            d["positions"] = [list(p) for p in self.positions]
            d["normals"] = [list(n) for n in self.normals]
        return d


def decode_vertex_frame(b: bytes, off: int) -> tuple[VertexFrame, int]:
    """Decode one ``VERTEXFRAMES`` frame at ``off``.

    Returns ``(frame, size)`` where ``size`` is the frame's byte length. Raises
    ``GeometryError`` for unsupported versions (0x200/0x201 store unquantized
    float data and are not exercised by any shipped asset)."""
    ver = _u32(b, off)
    if ver not in QUANTIZED_VF_VERSIONS:
        raise GeometryError(
            f"VERTEXFRAMES version 0x{ver:04x} not decodable (only 0x202-0x204)")
    p = off + 4
    extra20 = _u32(b, p)
    p += 4
    mn = struct.unpack_from("<3f", b, p)
    p += 12
    mx = struct.unpack_from("<3f", b, p)
    p += 12
    extra = None
    if ver == 0x202:
        extra = _u32(b, p)
        p += 4
    aux = struct.unpack_from("<3f", b, p)
    p += 12
    extra30 = _u32(b, p)
    p += 4
    n = _u32(b, p)
    p += 4
    pos_end = p + n * 6
    nrm_end = pos_end + n * 3
    size = nrm_end - off
    if nrm_end > len(b):
        raise GeometryError(
            f"VERTEXFRAMES frame at 0x{off:X} runs past EOF "
            f"(needs {size} bytes)")
    sx = (mx[0] - mn[0]) * _Q16_SCALE
    sy = (mx[1] - mn[1]) * _Q16_SCALE
    sz = (mx[2] - mn[2]) * _Q16_SCALE
    positions = []
    for i in range(n):
        qx, qy, qz = struct.unpack_from("<3H", b, p + i * 6)
        positions.append((mn[0] + qx * sx, mn[1] + qy * sy, mn[2] + qz * sz))
    normals = []
    for i in range(n):
        qx, qy, qz = struct.unpack_from("<3b", b, pos_end + i * 3)
        vx = qx * _I8_NORMAL_SCALE
        vy = qy * _I8_NORMAL_SCALE
        vz = qz * _I8_NORMAL_SCALE
        ln = (vx * vx + vy * vy + vz * vz) ** 0.5
        normals.append((0.0, 0.0, 0.0) if ln == 0.0
                       else (vx / ln, vy / ln, vz / ln))
    frame = VertexFrame(ver, n, mn, mx, aux, (extra20, extra, extra30),
                        positions, normals, size, exact=True)
    return frame, size


def decode_triangles(b: bytes, off: int, size: int) -> tuple[int, tuple[int, ...]]:
    """Decode a ``TRIANGLES`` payload: ``u32 count`` + ``count`` *u16* triplets."""
    cnt = _u32(b, off)
    need = 4 + cnt * 3 * 2
    if need != size:
        raise GeometryError(
            f"TRIANGLES size {size} != 4 + {cnt}*6 = {need}")
    return cnt, struct.unpack_from("<%dH" % (cnt * 3), b, off + 4)


class TVertexFrame:
    """One decoded ``TVERTEXFRAMES`` frame (a quantized UV set)."""

    __slots__ = ("version", "count", "bounds_min", "bounds_max", "uvs",
                 "size", "exact")

    def __init__(self, version, count, bounds_min, bounds_max, uvs, size, exact):
        self.version = version
        self.count = count
        self.bounds_min = bounds_min          # (min_u, min_v)
        self.bounds_max = bounds_max          # (max_u, max_v)
        self.uvs = uvs                        # list of (u, v)
        self.size = size
        self.exact = exact

    def as_dict(self, with_geometry=False):
        d = {
            "version": self.version,
            "uv_count": self.count,
            "bounds_min": list(self.bounds_min),
            "bounds_max": list(self.bounds_max),
            "size": self.size,
            "exact": self.exact,
        }
        if with_geometry:
            d["uvs"] = [list(uv) for uv in self.uvs]
        return d


def decode_tvertex_frame(b: bytes, off: int) -> tuple[TVertexFrame, int]:
    """Decode one ``TVERTEXFRAMES`` payload at ``off``.

    Mirrors the version-0x200 branch of ``FUN_100807d0`` in MXR.dll (the branch
    every shipped asset selects). Returns ``(frame, size)`` where ``size`` is
    the payload length ``20 + 4 * uv_count``. Raises ``GeometryError`` if the
    payload runs past EOF."""
    if off + 4 > len(b):
        raise GeometryError(
            f"TVERTEXFRAMES frame at 0x{off:X} is truncated (no count word)")
    n = _u32(b, off)
    p = off + 4
    mn = struct.unpack_from("<2f", b, p)
    p += 8
    mx = struct.unpack_from("<2f", b, p)
    p += 8
    size = 20 + 4 * n
    if off + size > len(b):
        raise GeometryError(
            f"TVERTEXFRAMES frame at 0x{off:X} runs past EOF (needs {size} bytes)")
    su = (mx[0] - mn[0]) * _Q16_SCALE
    sv = (mx[1] - mn[1]) * _Q16_SCALE
    uvs = []
    for i in range(n):
        qu, qv = struct.unpack_from("<2H", b, p + i * 4)
        uvs.append((mn[0] + qu * su, mn[1] + qv * sv))
    frame = TVertexFrame(TVERTEX_FRAME_VERSION, n, mn, mx, uvs, size, exact=True)
    return frame, size


def _u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def _i32(b: bytes, off: int) -> int:
    return struct.unpack_from("<i", b, off)[0]


# BONEDEFORM (embedded skinning) constants, recovered from MXR.dll FUN_1007d260
# + FUN_1007c150 (version 0x200 branch). Weights are stored as u8 and scaled by
# 1/255; influence indices are u8 (so at most 256 bones).
BONE_DEFORM_VERSION = 0x200
_BONE_WEIGHT_SCALE = 1.0 / 255.0
# The trailing matrix-map id is a signed word; this is the "no matrix map" value.
BONE_MATRIX_MAP_NONE = -1


class BoneVertex:
    """One per-vertex ``BONEDEFORM`` record: ``{u8 flags; u8 n; u16 start}``."""

    __slots__ = ("flags", "influence_count", "influence_start")

    def __init__(self, flags, influence_count, influence_start):
        self.flags = flags
        self.influence_count = influence_count
        self.influence_start = influence_start

    def as_dict(self):
        return {
            "flags": self.flags,
            "influence_count": self.influence_count,
            "influence_start": self.influence_start,
        }


class BoneDeform:
    """Decoded ``BONEDEFORM`` payload: per-vertex bone influences + matrix map.

    Influence ``bone`` bytes live in one of two spaces (ADR-0423): a
    ``matrix_bones`` palette index when that palette is non-empty, otherwise a
    direct skeleton node index. :meth:`resolve_bone` /
    :meth:`resolved_influences_of` apply the mapping; :meth:`influences_of`
    keeps the raw bytes.
    """

    __slots__ = ("vertex_count", "vertices", "influence_count", "influences",
                 "matrix_map_id", "matrix_bones", "size", "exact")

    def __init__(self, vertex_count, vertices, influence_count, influences,
                 matrix_map_id, matrix_bones, size, exact):
        self.vertex_count = vertex_count
        self.vertices = vertices                # list of BoneVertex
        self.influence_count = influence_count
        self.influences = influences            # list of (bone, weight)
        self.matrix_map_id = matrix_map_id      # signed, or None when absent
        self.matrix_bones = matrix_bones        # list of u8 palette indices
        self.size = size
        self.exact = exact

    def has_palette(self) -> bool:
        """True when the trailing ``matrix_bones`` palette is present."""
        return bool(self.matrix_bones)

    def bone_space(self) -> str:
        """``"palette"`` when ``bone`` bytes index ``matrix_bones``, else ``"node"``."""
        return "palette" if self.matrix_bones else "node"

    def resolve_bone(self, index: int) -> int:
        """Map a raw influence ``bone`` byte to a skeleton node index.

        Applies the ``matrix_bones`` palette when present; otherwise the raw
        index is already a node index. Raises ``GeometryError`` when a palette
        is present and ``index`` falls outside it."""
        if not self.matrix_bones:
            return index
        if not 0 <= index < len(self.matrix_bones):
            raise GeometryError(
                f"BONEDEFORM bone palette index {index} out of range "
                f"(palette size {len(self.matrix_bones)})")
        return self.matrix_bones[index]

    def influences_of(self, vertex_index: int):
        """Return raw ``[(bone_byte, weight), ...]`` for one vertex."""
        v = self.vertices[vertex_index]
        s = v.influence_start
        return self.influences[s:s + v.influence_count]

    def resolved_influences_of(self, vertex_index: int):
        """Return ``[(skeleton_node, weight), ...]`` for one vertex."""
        v = self.vertices[vertex_index]
        s = v.influence_start
        return [(self.resolve_bone(b), w)
                for b, w in self.influences[s:s + v.influence_count]]

    def max_bone(self) -> int:
        """Largest raw ``bone`` byte (a palette index when a palette is present)."""
        return max((bone for bone, _w in self.influences), default=-1)

    def max_resolved_bone(self) -> int:
        """Largest resolved skeleton node index over every in-range influence.

        Out-of-palette indices are ignored here (they are reported by
        ``validate()`` / raise from :meth:`resolve_bone`) so aggregation never
        fails on a malformed cluster."""
        m = -1
        for b, _w in self.influences:
            if self.matrix_bones:
                if 0 <= b < len(self.matrix_bones):
                    m = max(m, self.matrix_bones[b])
            else:
                m = max(m, b)
        return m

    def used_palette_indices(self) -> list:
        """Distinct raw ``bone`` bytes actually referenced by influences."""
        return sorted({b for b, _w in self.influences})

    def palette_fully_used(self) -> bool:
        """True when every ``matrix_bones`` entry is referenced (vacuously true
        without a palette)."""
        if not self.matrix_bones:
            return True
        return len(self.used_palette_indices()) == len(self.matrix_bones)

    def max_influences(self) -> int:
        return max((v.influence_count for v in self.vertices), default=0)

    def as_dict(self, with_skin=False):
        d = {
            "vertex_count": self.vertex_count,
            "influence_count": self.influence_count,
            "max_bone": self.max_bone(),
            "max_resolved_bone": self.max_resolved_bone(),
            "bone_space": self.bone_space(),
            "max_influences": self.max_influences(),
            "matrix_map_id": self.matrix_map_id,
            "matrix_bone_count": len(self.matrix_bones),
            "matrix_bones": list(self.matrix_bones),
            "palette_fully_used": self.palette_fully_used(),
            "size": self.size,
            "exact": self.exact,
        }
        if with_skin:
            d["vertices"] = [v.as_dict() for v in self.vertices]
            d["influences"] = [[bone, w] for bone, w in self.influences]
        return d



def decode_bonedeform(b: bytes, off: int,
                      size: int | None = None) -> tuple[BoneDeform, int]:
    """Decode a ``BONEDEFORM`` payload at ``off``.

    Mirrors ``FUN_1007d260`` in MXR.dll: a vertex-record array, a shared
    influence pool addressed by prefix-sum starts, and a trailing bone
    matrix-map block. When ``size`` is given, the payload must tile exactly
    (``exact=True``). Returns ``(BoneDeform, consumed_size)``. Raises
    ``GeometryError`` when a structural invariant is violated."""

    def need(p: int, n: int, what: str):
        if p + n > len(b):
            raise GeometryError(f"BONEDEFORM {what} at 0x{p:X} runs past EOF")
        if size is not None and p + n > off + size:
            raise GeometryError(f"BONEDEFORM {what} at 0x{p:X} runs past chunk")

    need(off, 4, "vertex count")
    n = _u32(b, off)
    p = off + 4
    need(p, n * 4, "vertex records")
    vertices = []
    for i in range(n):
        flags, cnt, start = struct.unpack_from("<BBH", b, p + i * 4)
        vertices.append(BoneVertex(flags, cnt, start))
    p += n * 4

    need(p, 4, "influence count")
    m = _u32(b, p)
    p += 4
    need(p, m * 2, "influences")
    influences = []
    for i in range(m):
        bone = b[p + i * 2]
        weight = b[p + i * 2 + 1] * _BONE_WEIGHT_SCALE
        influences.append((bone, weight))
    p += m * 2

    # The vertex records must address the influence pool as a running prefix
    # sum, and together cover it exactly (the loader relies on this).
    running = 0
    for i, v in enumerate(vertices):
        if v.influence_start != running:
            raise GeometryError(
                f"BONEDEFORM vertex {i} influence_start {v.influence_start} "
                f"!= running {running}")
        running += v.influence_count
    if running != m:
        raise GeometryError(
            f"BONEDEFORM influences covered {running} != declared {m}")

    matrix_map_id = None
    matrix_bones: list[int] = []
    if size is not None:
        remain = off + size - p
        if remain:
            need(p, 8, "matrix-map header")
            matrix_map_id = _i32(b, p)
            count = _u32(b, p + 4)
            if 8 + count != remain:
                raise GeometryError(
                    f"BONEDEFORM matrix-map size {remain} != 8 + {count}")
            matrix_bones = list(b[p + 8:p + 8 + count])
            p += 8 + count
    consumed = p - off
    exact = size is None or consumed == size
    return BoneDeform(n, vertices, m, influences, matrix_map_id,
                      matrix_bones, consumed, exact), consumed


class KeyValue:
    """One ``KEYS`` entry: a metadata key and its string value."""

    __slots__ = ("key", "value")

    def __init__(self, key: str, value: str):
        self.key = key
        self.value = value

    def as_dict(self):
        return {"key": self.key, "value": self.value}


def decode_keys(b: bytes, off: int,
                size: int | None = None) -> tuple[list[KeyValue], int]:
    """Decode a ``KEYS`` metadata map at ``off``.

    Mirrors ``CKeyContainer::Read`` in MCCDyn.dll (0x100195d0): a ``u32`` entry
    count, then per entry a length-prefixed, NUL-terminated key and a
    length-prefixed, NUL-terminated value. The reader always consumes
    ``key_len + 1`` bytes for the key; for the value it consumes ``value_len +
    1`` bytes **except** when ``value_len == 0``, in which case it stores an
    empty string and reads nothing. When ``size`` is given the payload must tile
    exactly. Returns ``(entries, consumed_size)`` and raises ``GeometryError``
    on any structural violation (overrun, missing NUL terminator)."""

    def need(p: int, n: int, what: str):
        if p + n > len(b):
            raise GeometryError(f"KEYS {what} at 0x{p:X} runs past EOF")
        if size is not None and p + n > off + size:
            raise GeometryError(f"KEYS {what} at 0x{p:X} runs past chunk")

    need(off, 4, "count")
    count = _u32(b, off)
    p = off + 4
    entries: list[KeyValue] = []
    for i in range(count):
        need(p, 4, f"key {i} length")
        klen = _u32(b, p)
        p += 4
        need(p, klen + 1, f"key {i} text")
        key = b[p:p + klen]
        if b[p + klen] != 0:
            raise GeometryError(f"KEYS key {i} is not NUL terminated")
        p += klen + 1

        need(p, 4, f"value {i} length")
        vlen = _u32(b, p)
        p += 4
        if vlen == 0:
            value = b""
        else:
            need(p, vlen + 1, f"value {i} text")
            value = b[p:p + vlen]
            if b[p + vlen] != 0:
                raise GeometryError(f"KEYS value {i} is not NUL terminated")
            p += vlen + 1
        entries.append(KeyValue(key.decode("latin1"), value.decode("latin1")))
    return entries, p - off


# CLUSTER/MISCELLANEOUS (the per-cluster header that precedes VERTEXFRAMES).
# 44 bytes on every shipped model. Recovered from the on-disk bytes + the loader
# (MXR.dll FUN_10080f50 reads only the leading u32 model version) + an aggregate
# check over all 631 models:
#
#   u32 model version   (0x102 on every shipped cluster; the value the
#                        BONEDEFORM reader branches on)
#   u32 flags           (0 exactly when the cluster carries a non-empty
#                        TVERTEXFRAMES UV set: 2099/2099 textured, 2563/2563
#                        untextured)
#   u32 surface_index   (always < the SURFACES count; the distinct values cover
#                        every surface)
#   u32 reserved        (0 on every shipped cluster)
#   f32 box_min[3]      \ the cluster's reference box - it MIRRORS the paired
#   f32 box_max[3]       \ VERTEXFRAMES frame's quantization range exactly on
#                         all 4662 clusters (and encloses the decoded geometry)
#   f32 trailer         (0.0 on every shipped cluster)
#
# See docs/adr/0420-native-hand-mesh-asset.md and
# docs/adr/0425-xmd-cluster-miscellaneous-bounds.md.
MISC_RECORD_LEN = 44
MISC_BOX_OFFSET = 16           # first box float, right after the 4 u32s
MISC_BOX_FLOATS = 6            # box_min[3] + box_max[3]


class ClusterInfo:
    """Decoded ``CLUSTER``/``MISCELLANEOUS`` header (one per cluster).

    Beyond the model/surface/flag words, the record carries a 6-float bounding
    box (``box_min``/``box_max``) plus a trailing float. The box is a *reference*
    bound that mirrors the cluster's ``VERTEXFRAMES`` quantization range exactly
    on every shipped cluster (ADR-0425); it encloses the decoded geometry but is
    not a transform. ``payload`` keeps the raw 28 trailing bytes for callers that
    want the undecoded bytes.
    """

    __slots__ = ("version", "flags", "surface_index", "reserved",
                 "box_min", "box_max", "trailer", "payload")

    def __init__(self, version, flags, surface_index, reserved,
                 box_min, box_max, trailer, payload):
        self.version = version
        self.flags = flags
        self.surface_index = surface_index
        self.reserved = reserved
        self.box_min = box_min          # (x, y, z) reference lower bound
        self.box_max = box_max          # (x, y, z) reference upper bound
        self.trailer = trailer          # the trailing f32 (0.0 shipped)
        self.payload = payload          # the trailing 28 raw bytes

    def encloses(self, mn, mx, tol=0.0) -> bool:
        """True when this reference box contains the AABB ``[mn, mx]``.

        The box mirrors the cluster's ``VERTEXFRAMES`` quantization range, so the
        decoded geometry AABB always lies inside it; a caller can still use this
        to cross-check a box against geometry it decoded itself."""
        return all(self.box_min[k] <= mn[k] + tol
                   and self.box_max[k] >= mx[k] - tol for k in range(3))

    def as_dict(self):
        return {
            "version": self.version,
            "flags": self.flags,
            "surface_index": self.surface_index,
            "reserved": self.reserved,
            "box_min": list(self.box_min),
            "box_max": list(self.box_max),
            "trailer": self.trailer,
        }


def decode_cluster_info(b: bytes, off: int, size: int) -> ClusterInfo:
    if size < MISC_RECORD_LEN:
        raise GeometryError(f"MISCELLANEOUS size {size} < {MISC_RECORD_LEN}")
    version, flags, surface_index, reserved = struct.unpack_from("<IIII", b, off)
    box = struct.unpack_from(
        "<%df" % (MISC_BOX_FLOATS + 1), b, off + MISC_BOX_OFFSET)
    box_min = tuple(box[0:3])
    box_max = tuple(box[3:6])
    trailer = box[MISC_BOX_FLOATS]
    return ClusterInfo(version, flags, surface_index, reserved,
                       box_min, box_max, trailer,
                       b[off + MISC_BOX_OFFSET:off + size])



# SKELETON/NODES (recovered from MXR.dll: the skeleton reader FUN_100219d0,
# writer FUN_1001f7a0, and the node record reader FUN_1001fc30 / writer
# FUN_1001eec0, which agree byte for byte; finalizer FUN_1001ff60 caps the two
# animation slots). Every shipped node record is a fixed 34-byte structure,
# version 0x100. See docs/adr/0418-xmd-skeleton-nodes-nodeindices.md.
NODE_ENTRY_VERSION = 0x100
NODE_ENTRY_LEN = 34
# FUN_1001ff60 rejects > these; a slot index of NODE_SLOT_NONE means "none".
MAX_ROTATION_SLOTS = 0x80
MAX_MOVEMENT_SLOTS = 0x20
NODE_SLOT_NONE = 0xFFFF


class SkeletonNode:
    """One ``NODES`` record (34 bytes, version 0x100).

    Field widths are proven from the decompiled loader (``FUN_1001eec0`` writer
    and ``FUN_1001fc30`` reader agree byte for byte)::

        u16  version
        f32  translation[3]    bind-pose model-space position (not a local offset;
                               same coordinate space as VERTEXFRAMES - see
                               ADR-0419 "Update - NODES translations are
                               model-space")
        u16  unknown_c         (0 in every shipped asset)
        u16  child_start       first child's slot in NODEINDICES
        u16  child_count       number of direct children
        u16  parent            parent index (0..N-1; node 0 is the root)
        f32  scale_xy[2]       (1.0f, 1.0f in every shipped asset)
        u16  rotation_slot     == node index for used nodes; 0xFFFF = padding
        u16  movement_slot     < 0x20, or 0xFFFF for "none"

    A record whose ``rotation_slot`` is ``NODE_SLOT_NONE`` (0xFFFF) is a
    **padding/unused** node: it also carries ``movement_slot == 0xFFFF``, a zero
    translation, ``parent == 0`` and ``child_count == 0``, and is excluded from
    ``NODEINDICES``.

    A used node's children are ``NODEINDICES[child_start : child_start +
    child_count]`` — see :meth:`Skeleton.children_of` and the load-time walker
    ``FUN_1001e040`` (ADR-0418 §Update).
    """

    __slots__ = ("index", "version", "translation", "unknown_c", "child_start",
                 "child_count", "parent", "scale_xy", "rotation_slot",
                 "movement_slot")

    def __init__(self, index, version, translation, unknown_c, child_start,
                 child_count, parent, scale_xy, rotation_slot, movement_slot):
        self.index = index
        self.version = version
        self.translation = translation      # (x, y, z) floats
        self.unknown_c = unknown_c
        self.child_start = child_start
        self.child_count = child_count
        self.parent = parent
        self.scale_xy = scale_xy            # (sx, sy) floats (1.0, 1.0)
        self.rotation_slot = rotation_slot
        self.movement_slot = movement_slot

    def is_root(self) -> bool:
        return self.parent == self.index

    def is_used(self) -> bool:
        """A real skeleton node. Padding records have ``rotation_slot == none``."""
        return self.rotation_slot != NODE_SLOT_NONE

    def as_dict(self):
        return {
            "index": self.index,
            "version": self.version,
            "translation": list(self.translation),
            "parent": self.parent,
            "child_start": self.child_start,
            "child_count": self.child_count,
            "rotation_slot": self.rotation_slot,
            "movement_slot": self.movement_slot,
            "unknown_c": self.unknown_c,
            "scale_xy": list(self.scale_xy),
        }


class Skeleton:
    """Decoded ``SKELETON`` sub-tree: the ``NODES`` array + ``NODEINDICES``."""

    __slots__ = ("nodes", "node_indices", "size", "indices_size", "exact")

    def __init__(self, nodes, node_indices, size, indices_size, exact):
        self.nodes = nodes                  # list of SkeletonNode
        self.node_indices = node_indices    # tuple of u16 (bone traversal order)
        self.size = size
        self.indices_size = indices_size
        self.exact = exact

    def used_nodes(self) -> list[SkeletonNode]:
        """Real skeleton nodes (padding records have ``rotation_slot == none``)."""
        return [n for n in self.nodes if n.is_used()]

    def unused_nodes(self) -> list[SkeletonNode]:
        return [n for n in self.nodes if not n.is_used()]

    def children_of(self, index: int) -> list[int]:
        """Direct children of ``index`` via the NODEINDICES child table.

        This mirrors the load-time walker ``FUN_1001e040``: a used node's
        children are ``NODEINDICES[child_start : child_start + child_count]``.
        Padding records and out-of-range ranges yield ``[]``."""
        if not 0 <= index < len(self.nodes):
            return []
        n = self.nodes[index]
        if not n.is_used():
            return []
        end = n.child_start + n.child_count
        if end > len(self.node_indices):
            return []
        return list(self.node_indices[n.child_start:end])

    def child_ranges_tile(self) -> bool:
        """True when the used nodes' child ranges tile NODEINDICES exactly."""
        m = len(self.node_indices)
        cover = [0] * m
        for n in self.nodes:
            if n.child_count == 0:
                continue
            if n.child_start + n.child_count > m:
                return False
            for k in range(n.child_start, n.child_start + n.child_count):
                cover[k] += 1
        return all(c == 1 for c in cover)

    def derived_parents(self) -> dict[int, int]:
        """Map each non-root child to its parent, read from the child table.

        Independent of the ``parent`` field, so it cross-checks the hierarchy."""
        out: dict[int, int] = {}
        for n in self.used_nodes():
            for c in self.children_of(n.index):
                out.setdefault(c, n.index)
        return out

    def roots(self) -> list[int]:
        """Used nodes that are their own parent (node 0 normally)."""
        return [n.index for n in self.used_nodes() if n.index == n.parent]

    def is_tree(self) -> bool:
        """True when every used node reaches a root without a cycle."""
        used = {n.index for n in self.used_nodes()}
        for n in self.used_nodes():
            if not (0 <= n.parent < len(self.nodes)):
                return False
            seen = set()
            j = n.index
            while j != 0 and j not in seen:
                if j not in used or not (0 <= self.nodes[j].parent < len(self.nodes)):
                    return False
                seen.add(j)
                j = self.nodes[j].parent
            if j != 0 and j != n.index:
                return False
        return True

    def max_rotation_slot(self) -> int:
        slots = [n.rotation_slot for n in self.nodes if n.is_used()]
        return max(slots, default=-1)

    def max_movement_slot(self) -> int:
        slots = [n.movement_slot for n in self.nodes
                 if n.movement_slot != NODE_SLOT_NONE]
        return max(slots, default=-1)

    def child_count_matches(self) -> bool:
        """True when every node's child table agrees with its ``parent`` field.

        The child table (``NODEINDICES``) is authoritative for the walker; the
        ``parent`` field must be its inverse for the skeleton to be coherent."""
        for n in self.used_nodes():
            if n.child_start + n.child_count > len(self.node_indices):
                return False
            table = set(self.children_of(n.index))
            scan = {m.index for m in self.used_nodes()
                    if m.parent == n.index and m.index != n.index}
            if n.index == 0:
                scan.discard(0)
            if table != scan:
                return False
        return True

    def as_dict(self, with_nodes=False):
        used = self.used_nodes()
        d = {
            "node_count": len(self.nodes),
            "used_nodes": len(used),
            "unused_nodes": len(self.nodes) - len(used),
            "index_count": len(self.node_indices),
            "roots": self.roots(),
            "tree": self.is_tree(),
            "child_count_matches": self.child_count_matches(),
            "child_ranges_tile": self.child_ranges_tile(),
            "max_rotation_slot": self.max_rotation_slot(),
            "max_movement_slot": self.max_movement_slot(),
            "size": self.size,
            "exact": self.exact,
        }
        if with_nodes:
            d["nodes"] = [n.as_dict() for n in self.nodes]
            d["node_indices"] = list(self.node_indices)
        return d


def decode_skeleton(b: bytes, nodes_off: int, nodes_size: int, count: int,
                    indices_off: int | None = None,
                    indices_size: int = 0) -> Skeleton:
    """Decode the ``NODES`` (and optional ``NODEINDICES``) payload.

    Mirrors MXR.dll ``FUN_100219d0`` + ``FUN_1001fc30`` (node reader). Raises
    ``GeometryError`` when the node payload does not tile exactly or the record
    count disagrees with the chunk header."""
    if nodes_size < 4 and count == 0:
        return Skeleton([], (), nodes_size, indices_size, True)
    if count and nodes_size != count * NODE_ENTRY_LEN:
        raise GeometryError(
            f"NODES size {nodes_size} != {count} * {NODE_ENTRY_LEN}")
    n = count or nodes_size // NODE_ENTRY_LEN
    if nodes_size != n * NODE_ENTRY_LEN:
        raise GeometryError(
            f"NODES size {nodes_size} is not a multiple of {NODE_ENTRY_LEN}")
    if nodes_off + nodes_size > len(b):
        raise GeometryError("NODES payload runs past EOF")
    nodes = []
    for i in range(n):
        (ver, ax, ay, az, uc, cs, cc, par, sx, sy, rot, mov
         ) = struct.unpack_from("<HfffHHHHffHH", b,
                                nodes_off + i * NODE_ENTRY_LEN)
        nodes.append(SkeletonNode(
            i, ver, (ax, ay, az), uc, cs, cc, par, (sx, sy), rot, mov))
    node_indices: tuple[int, ...] = ()
    if indices_off is not None and indices_size:
        if indices_size % 2:
            raise GeometryError(f"NODEINDICES size {indices_size} is odd")
        m = indices_size // 2
        if indices_off + indices_size > len(b):
            raise GeometryError("NODEINDICES payload runs past EOF")
        node_indices = struct.unpack_from("<%dH" % m, b, indices_off)
    exact = (nodes_size == n * NODE_ENTRY_LEN
             and indices_size % 2 == 0)
    return Skeleton(nodes, node_indices, nodes_size, indices_size, exact)


class Node:
    __slots__ = ("offset", "name", "next_sibling", "first_child",
                 "data_offset", "data_size", "count", "flags", "raw")

    def __init__(self, offset, name):
        self.offset = offset
        self.name = name
        self.raw = None
        self.next_sibling = 0
        self.first_child = 0
        self.data_offset = 0
        self.data_size = 0
        self.count = 0
        self.flags = 0

    @classmethod
    def from_fields(cls, offset, name, fields):
        n = cls(offset, name)
        n.raw = fields
        n.next_sibling, n.first_child = fields[0], fields[1]
        n.data_offset, n.data_size = fields[2], fields[3]
        n.count, n.flags = fields[4], fields[5]
        return n

    def as_dict(self):
        return {
            "offset": self.offset,
            "name": self.name,
            "next_sibling": self.next_sibling,
            "first_child": self.first_child,
            "data_offset": self.data_offset,
            "data_size": self.data_size,
            "count": self.count,
            "flags": self.flags,
        }


class Xmd:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        self.notes: list[str] = []
        self._parse_header()
        self._parse_nodes()

    def _read_node(self, off: int) -> Node:
        b = self.data
        name = b[off:off + NODE_NAME_LEN].rstrip(b"\x00")
        fields = struct.unpack_from("<6I", b, off + NODE_NAME_LEN)
        return Node.from_fields(off, name.decode("latin1"), fields)

    def _parse_header(self):
        b = self.data
        if len(b) < NODE_RECORD_LEN or b[:16] != MAGIC:
            raise XmdError(f"{self.path}: not a MOS DATAFILE2.0 file")
        # The "header" is the root node record at file offset 0: its 0x18-byte
        # name field holds the magic. node_start/data_start are just the root's
        # sibling pointer and its payload offset (they are not extra fields).
        self.root = self._read_node(0)
        self.node_start = self.root.next_sibling
        self.data_start = self.root.data_offset
        if self.node_start < NODE_RECORD_LEN or self.data_start < self.node_start:
            raise XmdError(f"{self.path}: implausible root node offsets")

    def _parse_nodes(self):
        b = self.data
        self.nodes: list[Node] = []
        self.by_offset: dict[int, Node] = {}
        off = self.node_start
        while off + NODE_RECORD_LEN <= self.data_start:
            name = b[off:off + NODE_NAME_LEN].rstrip(b"\x00")
            if not NAME_RE.match(name):
                self.notes.append(
                    f"node table ended early at 0x{off:X} (name {name!r})")
                break
            node = self._read_node(off)
            self.nodes.append(node)
            self.by_offset[off] = node
            off += NODE_RECORD_LEN

    # -- reporting -------------------------------------------------------
    def chunk(self, name: str):
        for n in self.nodes:
            if n.name == name:
                return n
        return None

    def node_parents(self) -> dict[int, int]:
        """Map every node offset to its parent node offset.

        The container stores only ``first_child``/``next_sibling`` links, so a
        node's parent is derived by walking each node's child chain. The root
        node (offset 0) has no entry."""
        parents: dict[int, int] = {}
        for n in self.nodes:
            child = n.first_child
            seen: set[int] = set()
            while child and child in self.by_offset and child not in seen:
                seen.add(child)
                parents.setdefault(child, n.offset)
                child = self.by_offset[child].next_sibling
        return parents

    def skeleton_node(self) -> Node | None:
        """The container's *skeleton* ``NODES`` chunk, or ``None``.

        :meth:`chunk` returns the first node named ``NODES`` in node-table order,
        but the shipped 2009 *Assault on Dark Athena* models frequently place a
        ``NODES`` chunk under a ``BOXTREE`` bounding-volume node (12-byte
        records) ahead of the ``SKELETON`` array, so the first match is not the
        skeleton (GH #84). A ``NODES`` is judged the skeleton, strongest signal
        first, when its parent is ``SKELETON``, when its sibling is
        ``NODEINDICES``, or when its payload tiles ``NODE_ENTRY_LEN`` (34) records
        with the chunk header count agreeing. ``None`` means no ``NODES``
        qualifies, i.e. the container has no skeleton (a static prop)."""
        candidates = [n for n in self.nodes if n.name == "NODES"]
        if not candidates:
            return None
        parents = self.node_parents()

        def rank(n: Node) -> int:
            parent = self.by_offset.get(parents.get(n.offset, 0))
            if parent is not None and parent.name == "SKELETON":
                return 3
            sib = self.by_offset.get(n.next_sibling) if n.next_sibling else None
            if sib is not None and sib.name == "NODEINDICES":
                return 2
            if n.data_size % NODE_ENTRY_LEN == 0 and (
                    n.count == 0 or n.data_size == n.count * NODE_ENTRY_LEN):
                return 1
            return 0

        best = max(candidates, key=rank)
        return best if rank(best) > 0 else None

    def _skeleton_indices_node(self, nodes_node: Node) -> Node | None:
        """The ``NODEINDICES`` sibling of a skeleton ``NODES`` (else the first)."""
        if nodes_node.next_sibling:
            sib = self.by_offset.get(nodes_node.next_sibling)
            if sib is not None and sib.name == "NODEINDICES":
                return sib
        return self.chunk("NODEINDICES")

    def geometry_summary(self):
        tri = self.chunk("TRIANGLES")
        boned = self.chunk("BONEDEFORM")
        nodes = self.skeleton_node()
        verts = self.chunk("VERTEXFRAMES")
        surf = self.chunk("SURFACES")
        summary = {
            "triangle_count": None,
            "triangle_count_matches_header": None,
            "surface_count": surf.count if surf else 0,
            "surfaces": self.surface_names() if surf else [],
            "distinct_cluster_surfaces": len(
                {i.surface_index for i in self.cluster_infos()}),
            "skeleton_node_count": nodes.count if nodes else 0,
            "bone_deform_vertices": None,   # filled from decoded skin below
            "has_skinning": bool(boned and nodes),
            "vertex_frame_bytes": verts.data_size if verts else 0,
        }
        if tri and 0 < tri.data_offset < len(self.data) - 4:
            declared = _u32(self.data, tri.data_offset)
            summary["triangle_count"] = declared
            summary["triangle_count_matches_header"] = (declared == tri.count)
        dec = self.geometry_stats()
        summary["clusters"] = dec["cluster_count"]
        summary["decoded_vertices"] = dec["decoded_vertices"]
        summary["decoded_triangles"] = dec["decoded_triangles"]
        summary["uv_clusters"] = dec["uv_clusters"]
        summary["decoded_uvs"] = dec["decoded_uvs"]
        summary["skinned_clusters"] = dec["skinned_clusters"]
        summary["paletted_bone_clusters"] = dec["paletted_bone_clusters"]
        summary["direct_bone_clusters"] = dec["direct_bone_clusters"]
        summary["bone_deform_vertices"] = dec["skinned_vertices"]
        summary["decoded_influences"] = dec["decoded_influences"]
        summary["max_influences_per_vertex"] = dec["max_influences_per_vertex"]
        summary["bad_clusters"] = dec["bad_clusters"]
        summary["reference_box_clusters"] = dec["reference_box_matched_clusters"]
        summary["reference_box_mismatch_clusters"] = (
            dec["reference_box_mismatch_clusters"])
        summary["reference_box_missing_clusters"] = (
            dec["reference_box_missing_clusters"])
        summary["matrix_map_ids"] = dec["matrix_map_ids"]
        summary["matrix_map_shared_id_groups"] = dec["matrix_map_shared_id_groups"]
        summary["matrix_map_palette_conflict_groups"] = (
            dec["matrix_map_palette_conflict_groups"])
        try:
            skel = self.skeleton()
        except GeometryError:
            skel = None
        summary["skeleton_nodes"] = len(skel.nodes) if skel else 0
        summary["skeleton_used_nodes"] = (
            len(skel.used_nodes()) if skel else 0)
        summary["skeleton_unused_nodes"] = (
            len(skel.unused_nodes()) if skel else 0)
        summary["skeleton_indices"] = len(skel.node_indices) if skel else 0
        summary["skeleton_tree_ok"] = skel.is_tree() if skel else None
        summary["skeleton_child_table_ok"] = (
            skel.child_ranges_tile() if skel else None)
        summary["skeleton_max_rotation_slot"] = (
            skel.max_rotation_slot() if skel else None)
        summary["skeleton_max_movement_slot"] = (
            skel.max_movement_slot() if skel else None)
        # KEYS is a string metadata map (no rotations) — see ADR-0421.
        keys_node = self.chunk("KEYS")
        summary["has_keys"] = keys_node is not None
        try:
            kv = self.keys()
        except GeometryError:
            kv = []
        summary["keys_count"] = len(kv)
        summary["key_names"] = [e.key for e in kv]
        return summary

    def surface_names(self) -> list[str]:
        """Extract the per-surface name strings (e.g. RIDDICK_ARMSCAR_D)."""
        surf = self.chunk("SURFACES")
        if not surf:
            return []
        start = surf.data_offset
        end = min(start + surf.data_size, len(self.data))
        return [s for s in _printable(self.data[start:end], 6)
                if re.fullmatch(r"[A-Za-z0-9_ .\-]+", s)]

    def text_strings(self) -> list[str]:
        """All printable runs >= 5 chars anywhere in the file (metadata)."""
        return _printable(self.data, 5)

    def tree_lines(self) -> list[str]:
        lines: list[str] = []
        seen: set[int] = set()

        def walk(off: int, depth: int):
            if off == 0 or off not in self.by_offset or off in seen:
                return
            seen.add(off)
            n = self.by_offset[off]
            lines.append("  " * depth + f"{n.name}  @0x{n.offset:X} "
                         f"off=0x{n.data_offset:X} size={n.data_size} "
                         f"count={n.count}")
            child = n.first_child
            while child and child in self.by_offset and child not in seen:
                walk(child, depth + 1)
                child = self.by_offset[child].next_sibling

        root = self.chunk("EXTENDEDMODEL")
        if root:
            walk(root.offset, 0)
        else:
            for n in self.nodes:
                walk(n.offset, 0)
        return lines

    def walk_linked(self) -> list[Node]:
        """Walk the node tree the way the engine loader does: from the root
        record, following sibling (+0x18) then descending into child (+0x1C).

        Unlike the contiguous scan this makes no assumption that records are
        packed; it is the authoritative traversal (see ``ReadTree_r``)."""
        order: list[Node] = []
        seen: set[int] = set()

        def visit(node: Node):
            # Mirrors CDataFile::ReadTree_r: process node, descend into the
            # child subtree, then continue along this node's sibling chain.
            while node is not None:
                if node.offset in seen:
                    return
                seen.add(node.offset)
                order.append(node)
                child = node.first_child
                if child and child in self.by_offset:
                    visit(self.by_offset[child])
                nxt = node.next_sibling
                node = self.by_offset.get(nxt) if nxt else None

        visit(self.root)
        return order

    def validate(self) -> list[str]:
        """Structural self-check. Returns a list of problems (empty = consistent).

        Confirms (a) root/header agreement, (b) every sibling/child pointer
        targets a parsed node, (c) the linked walk reaches every scanned node,
        and (d) node payload regions tile ``[data_start, EOF)``."""
        problems: list[str] = []
        if self.root.next_sibling != self.node_start:
            problems.append("root.next_sibling != node_start")
        if self.root.data_offset != self.data_start:
            problems.append("root.data_offset != data_start")

        valid = set(self.by_offset)
        for n in self.nodes:
            for label, ptr in (("sibling", n.next_sibling),
                               ("child", n.first_child)):
                if ptr and ptr not in valid:
                    problems.append(
                        f"{n.name}@0x{n.offset:X}: {label} 0x{ptr:X} not a node")

        reached = {n.offset for n in self.walk_linked()}
        missed = valid - reached
        if missed:
            problems.append(
                "linked walk missed %d node(s): %s"
                % (len(missed), ", ".join(f"0x{o:X}" for o in sorted(missed))))

        regions = sorted((n.data_offset, n.data_size)
                         for n in self.nodes if n.data_size > 0)
        prev = self.data_start
        for off, size in regions:
            if off < prev:
                problems.append(f"overlapping payload @0x{off:X}")
            elif off > prev:
                problems.append(f"gap in payloads 0x{prev:X}..0x{off:X}")
            prev = max(prev, off + size)
        if regions and prev != len(self.data):
            problems.append(
                f"payloads end at 0x{prev:X}, file is 0x{len(self.data):X}")

        # Geometry cross-check (GH #55): each decoded VERTEXFRAMES/
        # TVERTEXFRAMES/TRIANGLES/BONEDEFORM cluster must agree - triangle
        # indices inside the vertex range, UV/vertex count agreement, skinning
        # vertex count agreement, and every referenced bone inside the skeleton.
        nodes_chunk = self.skeleton_node()
        bone_total = nodes_chunk.count if nodes_chunk else 0
        meshes = self.cluster_meshes()
        for c in meshes:
            if c.get("error_kind") == "corrupt":
                problems.append(f"cluster {c['index']}: {c['error']}")
            elif c["index_in_range"] is False:
                problems.append(
                    f"cluster {c['index']}: triangle index {c['max_index']} "
                    f">= decoded vertex count")
            elif c.get("uv_matches") is False:
                problems.append(
                    f"cluster {c['index']}: UV count {c['uv_count']} != vertex "
                    f"count")
            elif c.get("skin_matches") is False:
                problems.append(
                    f"cluster {c['index']}: skin vertex count "
                    f"{c['skin_vertices']} != vertex count")
            elif c.get("skin") is not None and bone_total:
                # Bone-index space (ADR-0423): a non-empty palette means the raw
                # influence byte indexes matrix_bones and the resolved node must
                # be inside the skeleton; an empty palette means the byte is the
                # node index directly.
                skin = c["skin"]
                if skin["bone_space"] == "palette":
                    plen = skin["matrix_bone_count"]
                    if c["max_bone"] >= plen:
                        problems.append(
                            f"cluster {c['index']}: bone palette index "
                            f"{c['max_bone']} >= palette size {plen}")
                    elif c["max_resolved_bone"] >= bone_total:
                        problems.append(
                            f"cluster {c['index']}: resolved bone index "
                            f"{c['max_resolved_bone']} >= skeleton node count "
                            f"{bone_total}")
                    palette = skin.get("matrix_bones") or []
                    if len(palette) != len(set(palette)):
                        problems.append(
                            f"cluster {c['index']}: bone palette has duplicate "
                            f"entries")
                    elif any(b >= bone_total for b in palette):
                        problems.append(
                            f"cluster {c['index']}: bone palette entry >= "
                            f"skeleton node count {bone_total}")
                elif c["max_bone"] >= bone_total:
                    problems.append(
                        f"cluster {c['index']}: bone index {c['max_bone']} "
                        f">= skeleton node count {bone_total}")

        # Cluster MISCELLANEOUS cross-check (GH #55): each cluster's
        # surface_index must resolve inside SURFACES, the used indices must
        # cover every surface, and the flags field must agree with whether the
        # cluster actually carries a UV set. All three hold on all 631 shipped
        # models (see docs/adr/0420-native-hand-mesh-asset.md).
        surf_chunk = self.chunk("SURFACES")
        surface_total = surf_chunk.count if surf_chunk else 0
        infos = self.cluster_infos()
        if len(infos) not in (0, len(meshes)):
            problems.append(
                f"cluster MISCELLANEOUS count {len(infos)} != cluster count "
                f"{len(meshes)}")
        else:
            used_surfaces = set()
            for c, info in zip(meshes, infos):
                used_surfaces.add(info.surface_index)
                if surface_total and info.surface_index >= surface_total:
                    problems.append(
                        f"cluster {c['index']}: surface index "
                        f"{info.surface_index} >= SURFACES count {surface_total}")
                if (info.flags == 0) != (c["uv_count"] is not None):
                    problems.append(
                        f"cluster {c['index']}: MISCELLANEOUS flags "
                        f"{info.flags} disagrees with UV presence")
                if c.get("reference_box_matches_frame_bounds") is False:
                    problems.append(
                        f"cluster {c['index']}: MISCELLANEOUS reference box "
                        f"{list(info.box_min)}..{list(info.box_max)} != the "
                        f"VERTEXFRAMES quantization range "
                        f"{c['frames'][-1]['bounds_min']}.."
                        f"{c['frames'][-1]['bounds_max']}")
            if surface_total and meshes and len(used_surfaces) != surface_total:
                problems.append(
                    f"cluster surface indices cover {len(used_surfaces)} of "
                    f"{surface_total} surfaces")

        # Skeleton cross-check (GH #55): NODES must tile exactly, use version
        # 0x100, keep the padding/slot invariants, form an acyclic tree with a
        # correct child_count, and NODEINDICES must list each used non-root node
        # exactly once (the bone traversal order). Every rule below holds on all
        # 268 shipped NODES-bearing models (see ADR-0418).
        try:
            skel = self.skeleton()
        except GeometryError as e:
            skel = None
            problems.append(f"skeleton: {e}")
        if skel is not None and skel.nodes:
            n = len(skel.nodes)
            for nd in skel.nodes:
                if nd.version != NODE_ENTRY_VERSION:
                    problems.append(
                        f"skeleton node {nd.index}: version 0x{nd.version:04x} "
                        f"!= expected 0x{NODE_ENTRY_VERSION:04x}")
                    break
                if nd.unknown_c != 0:
                    problems.append(
                        f"skeleton node {nd.index}: reserved field != 0 "
                        f"({nd.unknown_c})")
                    break
                if tuple(nd.scale_xy) != (1.0, 1.0):
                    problems.append(
                        f"skeleton node {nd.index}: scale {nd.scale_xy} != 1.0")
                    break
                if not 0 <= nd.parent < n:
                    problems.append(
                        f"skeleton node {nd.index}: parent {nd.parent} out of "
                        f"range 0..{n - 1}")
                    break
                if nd.is_used():
                    if nd.rotation_slot != nd.index:
                        problems.append(
                            f"skeleton node {nd.index}: rotation slot "
                            f"{nd.rotation_slot} != index")
                        break
                    if nd.rotation_slot >= MAX_ROTATION_SLOTS:
                        problems.append(
                            f"skeleton node {nd.index}: rotation slot "
                            f"{nd.rotation_slot} >= {MAX_ROTATION_SLOTS}")
                        break
                else:
                    if (nd.movement_slot != NODE_SLOT_NONE
                            or nd.parent != 0
                            or nd.child_count != 0
                            or tuple(nd.translation) != (0.0, 0.0, 0.0)):
                        problems.append(
                            f"skeleton node {nd.index}: padding record not "
                            f"zeroed")
                        break
                if (nd.movement_slot != NODE_SLOT_NONE
                        and nd.movement_slot >= MAX_MOVEMENT_SLOTS):
                    problems.append(
                        f"skeleton node {nd.index}: movement slot "
                        f"{nd.movement_slot} >= {MAX_MOVEMENT_SLOTS}")
                    break
            if not skel.is_tree():
                problems.append("skeleton is not an acyclic tree")
            if not skel.child_count_matches():
                problems.append("skeleton child_count mismatches used children")
            used_non_root = sorted(nd.index for nd in skel.used_nodes()
                                   if nd.index != 0)
            if skel.node_indices and (sorted(skel.node_indices) != used_non_root
                                      or len(set(skel.node_indices))
                                      != len(skel.node_indices)):
                problems.append(
                    "NODEINDICES is not exactly the used non-root node set")
            # The child table: NODEINDICES[child_start : child_start+child_count]
            # is the authoritative child list (FUN_1001e040). The used nodes'
            # ranges must stay in bounds, tile the table exactly, and the derived
            # hierarchy must match the parent field.
            m = len(skel.node_indices)
            for nd in skel.used_nodes():
                if nd.child_start + nd.child_count > m:
                    problems.append(
                        f"skeleton node {nd.index}: child range "
                        f"{nd.child_start}+{nd.child_count} past NODEINDICES "
                        f"({m} entries)")
                    break
            else:
                if not skel.child_ranges_tile():
                    problems.append(
                        "skeleton child ranges do not tile NODEINDICES")
            if sum(nd.child_count for nd in skel.used_nodes()) != m:
                problems.append(
                    f"skeleton child_count sum != NODEINDICES length {m}")
            derived = skel.derived_parents()
            for nd in skel.used_nodes():
                if nd.index == 0:
                    continue
                if derived.get(nd.index) != nd.parent:
                    problems.append(
                        f"skeleton node {nd.index}: parent field {nd.parent} "
                        f"!= child-table parent {derived.get(nd.index)}")
                    break

        # KEYS cross-check (GH #46): the metadata map must tile its chunk
        # exactly. Keys/values are length-prefixed, NUL-terminated C strings; a
        # consumer that poses the mesh does NOT get bone rotations here (see
        # docs/adr/0421-xmd-keys-metadata-map.md). The exact-tiling invariant
        # holds on all 631 shipped models; the string-content invariants do not
        # (dweller.xmd ships one corrupt binary first key), so they are recorded
        # as a corpus observation rather than a hard failure.
        keys_node = self.chunk("KEYS")
        if keys_node is not None:
            try:
                _entries, consumed = decode_keys(
                    self.data, keys_node.data_offset, keys_node.data_size)
            except GeometryError as e:
                problems.append(f"keys: {e}")
            else:
                if consumed != keys_node.data_size:
                    problems.append(
                        f"keys: decoded {consumed} bytes but chunk is "
                        f"{keys_node.data_size}")
        return problems

    # -- geometry ---------------------------------------------------------
    def vertex_frames(self, node: Node | None = None) -> list[VertexFrame]:
        """Decode the frames of one ``VERTEXFRAMES`` chunk.

        Mirrors ``CTriangleMeshCore::Read`` + ``CVertexFrame::Read`` in MXR.dll:
        when the model version is > 0x100 a single ``u32`` frame-count word is
        read before the per-frame payloads, so a leading word that is not itself
        a known frame version is treated as that header."""
        if node is None:
            node = self.chunk("VERTEXFRAMES")
        if node is None:
            return []
        b = self.data
        off, end = node.data_offset, node.data_offset + node.data_size
        if _u32(b, off) not in VERTEX_FRAME_VERSIONS:
            off += 4  # per-model frame-count word (model version > 0x100)
        frames: list[VertexFrame] = []
        while off < end:
            frame, size = decode_vertex_frame(b, off)
            if size <= 0 or off + size > end:
                raise GeometryError(
                    f"{node.name}@0x{node.offset:X}: frame tiling overflow")
            frames.append(frame)
            off += size
        if off != end:
            raise GeometryError(
                f"{node.name}@0x{node.offset:X}: frames end at 0x{off:X}, "
                f"chunk ends at 0x{end:X}")
        return frames

    def triangle_indices(self, node: Node | None = None):
        if node is None:
            node = self.chunk("TRIANGLES")
        if node is None:
            return 0, ()
        return decode_triangles(self.data, node.data_offset, node.data_size)

    def tvertex_frames(self, node: Node | None = None) -> list[TVertexFrame]:
        """Decode the UV frame of one ``TVERTEXFRAMES`` chunk.

        Mirrors ``FUN_100807d0`` in MXR.dll. An empty chunk (``data_size == 0``)
        means the cluster carries no UVs and decodes to ``[]``."""
        if node is None:
            node = self.chunk("TVERTEXFRAMES")
        if node is None or node.data_size == 0:
            return []
        b = self.data
        off, end = node.data_offset, node.data_offset + node.data_size
        frames: list[TVertexFrame] = []
        while off < end:
            frame, size = decode_tvertex_frame(b, off)
            if size <= 0 or off + size > end:
                raise GeometryError(
                    f"{node.name}@0x{node.offset:X}: UV frame tiling overflow")
            frames.append(frame)
            off += size
        if off != end:
            raise GeometryError(
                f"{node.name}@0x{node.offset:X}: UV frames end at 0x{off:X}, "
                f"chunk ends at 0x{end:X}")
        return frames

    def bone_deform(self, node: Node | None = None) -> BoneDeform | None:
        """Decode a ``BONEDEFORM`` chunk (see :func:`decode_bonedeform`).

        Returns ``None`` when the model/cluster carries no ``BONEDEFORM`` node
        (an unskinned, rigidly-bound cluster)."""
        if node is None:
            node = self.chunk("BONEDEFORM")
        if node is None:
            return None
        return decode_bonedeform(self.data, node.data_offset,
                                 node.data_size)[0]

    def skeleton(self) -> Skeleton | None:
        """Decode the ``SKELETON`` ``NODES``/``NODEINDICES`` sub-tree.

        Returns ``None`` when the file has no *skeleton* ``NODES`` chunk (a
        static prop, or a ``BOXTREE`` ``NODES`` that is not a skeleton -- see
        :meth:`skeleton_node`, GH #84). Mirrors MXR.dll ``FUN_100219d0`` (see
        :func:`decode_skeleton`)."""
        nd = self.skeleton_node()
        if nd is None:
            return None
        ni = self._skeleton_indices_node(nd)
        return decode_skeleton(
            self.data, nd.data_offset, nd.data_size, nd.count,
            ni.data_offset if ni is not None else None,
            ni.data_size if ni is not None else 0)

    def keys(self) -> list[KeyValue]:
        """Decode the ``KEYS`` metadata map (see :func:`decode_keys`).

        Returns an empty list when the file has no ``KEYS`` chunk. This is
        build/texture/render provenance, **not** animation."""
        node = self.chunk("KEYS")
        if node is None:
            return []
        entries, _consumed = decode_keys(
            self.data, node.data_offset, node.data_size)
        return entries

    def key_map(self) -> dict[str, str]:
        """The ``KEYS`` map as a plain ``{key: value}`` dict (last wins)."""
        return {kv.key: kv.value for kv in self.keys()}

    def has_keys(self) -> bool:
        return self.chunk("KEYS") is not None

    def _skin_owner_map(self) -> dict[int, Node]:
        """Map each ``VERTEXFRAMES`` node offset to its sibling ``BONEDEFORM``.

        In ``CTriangleMeshCore::Read`` a cluster's chunks are read as siblings
        (``VERTEXFRAMES`` -> ``TVERTEXFRAMES`` -> ``BONEDEFORM`` -> ...), so the
        ``BONEDEFORM`` that follows a ``VERTEXFRAMES`` in traversal order belongs
        to it. Clusters without skinning simply have no ``BONEDEFORM``."""
        owner: dict[int, Node] = {}
        last_vf: Node | None = None
        for n in self.walk_linked():
            if n.name == "VERTEXFRAMES":
                last_vf = n
            elif n.name == "BONEDEFORM" and last_vf is not None:
                owner.setdefault(last_vf.offset, n)
        return owner

    def cluster_infos(self) -> list[ClusterInfo]:
        """Decode each ``CLUSTER``'s ``MISCELLANEOUS`` header, traversal order.

        Index-aligned with :meth:`cluster_meshes`: the i-th ``CLUSTER`` node owns
        the i-th ``VERTEXFRAMES`` chunk. Empty when the model has no clusters."""
        infos: list[ClusterInfo] = []
        for n in self.walk_linked():
            if n.name != "CLUSTER":
                continue
            child = n.first_child
            seen: set[int] = set()
            while child and child in self.by_offset and child not in seen:
                seen.add(child)
                k = self.by_offset[child]
                if k.name == "MISCELLANEOUS":
                    infos.append(decode_cluster_info(
                        self.data, k.data_offset, k.data_size))
                    break
                child = k.next_sibling
        return infos

    def cluster_meshes(self) -> list[dict]:
        """Pair ``VERTEXFRAMES``/``TVERTEXFRAMES``/``TRIANGLES`` chunks in order.

        Each model stores one such triple per cluster (submesh). Decoding
        geometrically cross-checks them: every triangle index must fall inside
        the paired frame's vertex range, and a non-empty UV frame must have the
        same count as its position frame."""
        order = self.walk_linked()
        vf_nodes = [n for n in order if n.name == "VERTEXFRAMES"]
        tv_nodes = [n for n in order if n.name == "TVERTEXFRAMES"]
        tri_nodes = [n for n in order if n.name == "TRIANGLES"]
        skin_owner = self._skin_owner_map()
        infos = self.cluster_infos()
        aligned = len(infos) == len(vf_nodes)
        clusters = []
        for i, vn in enumerate(vf_nodes):
            entry: dict = {
                "index": i,
                "vertex_chunk": vn.as_dict(),
                "surface_index": infos[i].surface_index if aligned else None,
                "cluster_flags": infos[i].flags if aligned else None,
                "reference_box_min": (list(infos[i].box_min)
                                      if aligned else None),
                "reference_box_max": (list(infos[i].box_max)
                                      if aligned else None),
                "reference_box_matches_frame_bounds": None,
                "frames": [],
                "uv_chunk": None,
                "uv_count": None,
                "uv_matches": None,
                "skin_chunk": None,
                "skin": None,
                "skin_vertices": None,
                "skin_matches": None,
                "max_bone": None,
                "max_resolved_bone": None,
                "bone_space": None,
                "palette_fully_used": None,
                "triangle_chunk": None,
                "triangle_count": None,
                "max_index": None,
                "index_in_range": None,
                "error": None,
                "error_kind": None,
            }
            try:
                frames = self.vertex_frames(vn)
                entry["frames"] = [f.as_dict() for f in frames]
            except GeometryError as e:
                entry["error"] = str(e)
                entry["error_kind"] = ("unsupported" if "not decodable" in str(e)
                                       else "corrupt")
                frames = []
            if i < len(tv_nodes):
                tv = tv_nodes[i]
                entry["uv_chunk"] = tv.as_dict()
                try:
                    uv_frames = self.tvertex_frames(tv)
                    if uv_frames:
                        entry["uv_count"] = uv_frames[-1].count
                        if frames:
                            entry["uv_matches"] = (
                                uv_frames[-1].count == frames[-1].count)
                except GeometryError as e:
                    entry["error"] = (entry["error"] or "") + f"; {e}"
                    entry["error_kind"] = "corrupt"
            bdn = skin_owner.get(vn.offset)
            if bdn is not None:
                entry["skin_chunk"] = bdn.as_dict()
                try:
                    skin = self.bone_deform(bdn)
                    entry["skin"] = skin.as_dict()
                    entry["skin_vertices"] = skin.vertex_count
                    entry["max_bone"] = skin.max_bone()
                    entry["max_resolved_bone"] = skin.max_resolved_bone()
                    entry["bone_space"] = skin.bone_space()
                    entry["palette_fully_used"] = skin.palette_fully_used()
                    if frames:
                        entry["skin_matches"] = (
                            skin.vertex_count == frames[-1].count)
                except GeometryError as e:
                    entry["error"] = (entry["error"] or "") + f"; {e}"
                    entry["error_kind"] = "corrupt"
            if i < len(tri_nodes):
                tn = tri_nodes[i]
                entry["triangle_chunk"] = tn.as_dict()
                try:
                    cnt, idx = self.triangle_indices(tn)
                    entry["triangle_count"] = cnt
                    if idx:
                        entry["max_index"] = max(idx)
                    if frames:
                        n = frames[-1].count
                        entry["index_in_range"] = (
                            entry["max_index"] is None or entry["max_index"] < n)
                except GeometryError as e:
                    entry["error"] = (entry["error"] or "") + f"; {e}"
                    entry["error_kind"] = "corrupt"
            if aligned and entry["frames"]:
                fr = entry["frames"][-1]
                info = infos[i]
                entry["reference_box_matches_frame_bounds"] = (
                    all(abs(fr["bounds_min"][k] - info.box_min[k]) <= 1e-3
                        for k in range(3))
                    and all(abs(fr["bounds_max"][k] - info.box_max[k]) <= 1e-3
                            for k in range(3)))
            clusters.append(entry)
        return clusters

    def matrix_map_groups(self) -> list[dict]:
        """Group paletted skinned clusters by ``matrix_map_id`` (ADR-0426).

        The trailing ``BONEMATRIXMAP`` block carries a signed ``matrix_map_id``
        and a bone palette. This reports, per id, the clusters that share it and
        whether their palettes agree. It exists to test the hypothesis that the
        id names a *shared* bone-set registry: across the shipped corpus it does
        **not** (a within-model id group usually holds different palettes), so
        the id is a per-cluster identifier, not a palette key.

        Only clusters whose palette is **non-empty** are considered (an empty
        palette is the direct-node space and carries no bone matrix map)."""
        groups: dict = {}
        for c in self.cluster_meshes():
            sk = c.get("skin")
            if not sk or not sk["matrix_bone_count"]:
                continue
            groups.setdefault(sk["matrix_map_id"], []).append(
                (c["index"], tuple(sk["matrix_bones"])))
        out = []
        for mid, members in groups.items():
            palettes = {p for _i, p in members}
            out.append({
                "matrix_map_id": mid,
                "clusters": [i for i, _p in members],
                "palette_count": len(palettes),
                "palettes_agree": len(palettes) == 1,
            })
        out.sort(key=lambda g: (g["matrix_map_id"] is None,
                                g["matrix_map_id"] or 0))
        return out

    def geometry_stats(self) -> dict:
        """Aggregate decoded-geometry totals + a pass/fail cross-check."""
        clusters = self.cluster_meshes()
        total_verts = 0
        total_tris = 0
        total_uvs = 0
        total_infl = 0
        total_skin_verts = 0
        uv_clusters = 0
        skinned = 0
        paletted = 0
        direct = 0
        palette_unused = 0
        max_infl = 0
        bad = 0
        box_ok = 0
        box_bad = 0
        box_missing = 0
        map_groups: dict = {}
        for c in clusters:
            if c["frames"]:
                total_verts += c["frames"][-1]["vertex_count"]
            if c["triangle_count"] is not None:
                total_tris += c["triangle_count"]
            if c["uv_count"] is not None:
                total_uvs += c["uv_count"]
                uv_clusters += 1
            match = c.get("reference_box_matches_frame_bounds")
            if match is None:
                box_missing += 1
            elif match:
                box_ok += 1
            else:
                box_bad += 1
            if c.get("skin") is not None:
                skinned += 1
                total_infl += c["skin"]["influence_count"]
                total_skin_verts += c["skin_vertices"]
                max_infl = max(max_infl, c["skin"]["max_influences"])
                if c["bone_space"] == "palette":
                    paletted += 1
                    map_groups.setdefault(c["skin"]["matrix_map_id"], []).append(
                        tuple(c["skin"]["matrix_bones"]))
                    if c["palette_fully_used"] is False:
                        palette_unused += 1
                else:
                    direct += 1
            if (c["error"] or c["index_in_range"] is False
                    or c.get("uv_matches") is False
                    or c.get("skin_matches") is False):
                bad += 1
        return {
            "cluster_count": len(clusters),
            "decoded_vertices": total_verts,
            "decoded_triangles": total_tris,
            "uv_clusters": uv_clusters,
            "decoded_uvs": total_uvs,
            "skinned_clusters": skinned,
            "paletted_bone_clusters": paletted,
            "direct_bone_clusters": direct,
            "palette_unused_clusters": palette_unused,
            "skinned_vertices": total_skin_verts,
            "decoded_influences": total_infl,
            "max_influences_per_vertex": max_infl,
            "bad_clusters": bad,
            "reference_box_matched_clusters": box_ok,
            "reference_box_mismatch_clusters": box_bad,
            "reference_box_missing_clusters": box_missing,
            "matrix_map_ids": {str(mid): len(v)
                               for mid, v in sorted(
                                   map_groups.items(),
                                   key=lambda kv: (kv[0] is None,
                                                   kv[0] or 0))},
            "matrix_map_shared_id_groups": sum(
                1 for v in map_groups.values() if len(v) > 1),
            "matrix_map_palette_conflict_groups": sum(
                1 for v in map_groups.values() if len(set(v)) > 1),
        }


def _fmt3(v) -> str:
    return "(%6.2f,%6.2f,%6.2f)" % (v[0], v[1], v[2])


def _printable(b: bytes, minlen: int) -> list[str]:
    out, cur = [], bytearray()
    for c in b:
        if 32 <= c < 127:
            cur.append(c)
        else:
            if len(cur) >= minlen:
                out.append(cur.decode("latin1"))
            cur = bytearray()
    if len(cur) >= minlen:
        out.append(cur.decode("latin1"))
    return out


def report_file(path: str, want_json: bool, with_strings: bool,
                with_tree: bool, with_mesh: bool = False,
                cluster: int | None = None,
                with_skeleton: bool = False,
                with_keys: bool = False,
                with_matrix_map: bool = False) -> dict:
    x = Xmd(path)
    rec = {
        "path": path,
        "size": len(x.data),
        "node_start": x.node_start,
        "data_start": x.data_start,
        "root": x.root.as_dict(),
        "node_count": len(x.nodes),
        "chunks": [n.as_dict() for n in x.nodes],
        "geometry": x.geometry_summary(),
        "problems": x.validate(),
        "notes": x.notes,
    }
    if with_strings:
        rec["strings"] = x.text_strings()
    if with_mesh or cluster is not None:
        rec["clusters"] = x.cluster_meshes()
    skel = None
    if with_skeleton:
        skel = x.skeleton()
        rec["skeleton"] = skel.as_dict(with_nodes=True) if skel else None
    if with_keys:
        rec["keys"] = [kv.as_dict() for kv in x.keys()]
    if with_matrix_map:
        rec["matrix_map_groups"] = x.matrix_map_groups()
    if cluster is not None:
        vf_nodes = [n for n in x.walk_linked() if n.name == "VERTEXFRAMES"]
        if not 0 <= cluster < len(vf_nodes):
            raise XmdError(f"cluster {cluster} out of range "
                           f"(0..{len(vf_nodes) - 1})")
        rec["cluster_detail"] = [
            f.as_dict(with_geometry=True) for f in x.vertex_frames(vf_nodes[cluster])]
        tv_nodes = [n for n in x.walk_linked() if n.name == "TVERTEXFRAMES"]
        if cluster < len(tv_nodes):
            rec["uv_detail"] = [
                f.as_dict(with_geometry=True)
                for f in x.tvertex_frames(tv_nodes[cluster])]
        owner = x._skin_owner_map()
        if vf_nodes[cluster].offset in owner:
            sk = x.bone_deform(owner[vf_nodes[cluster].offset])
            rec["skin_detail"] = [sk.as_dict(with_skin=True)]
    if not want_json:
        print(f"== {os.path.basename(path)}  ({len(x.data)} bytes, "
              f"{len(x.nodes)} nodes, data@0x{x.data_start:X})")
        g = rec["geometry"]
        print(f"   surfaces({g['surface_count']}): {g['surfaces']}")
        print(f"   cluster_surface_indices_used={g['distinct_cluster_surfaces']}")
        print(f"   triangles={g['triangle_count']} "
              f"(header-match={g['triangle_count_matches_header']})  "
              f"skeleton_nodes={g['skeleton_node_count']}  "
              f"skinned_verts={g['bone_deform_vertices']}  "
              f"skinning={g['has_skinning']}")
        print(f"   clusters={g['clusters']}  decoded_verts={g['decoded_vertices']}"
              f"  decoded_tris={g['decoded_triangles']}"
              f"  uv_clusters={g['uv_clusters']}  decoded_uvs={g['decoded_uvs']}")
        print(f"   skinned_clusters={g['skinned_clusters']}  "
              f"paletted={g['paletted_bone_clusters']} "
              f"direct={g['direct_bone_clusters']}  "
              f"decoded_influences={g['decoded_influences']}  "
              f"max_influences_per_vertex={g['max_influences_per_vertex']}  "
              f"bad_clusters={g['bad_clusters']}")
        print(f"   reference_box_clusters={g['reference_box_clusters']}  "
              f"mismatch={g['reference_box_mismatch_clusters']}  "
              f"missing={g['reference_box_missing_clusters']}")
        print(f"   matrix_map_ids={g['matrix_map_ids']}  "
              f"shared_id_groups={g['matrix_map_shared_id_groups']}  "
              f"palette_conflicts={g['matrix_map_palette_conflict_groups']}")
        if with_matrix_map:
            for grp in rec.get("matrix_map_groups", []):
                print(f"   matrix_map_id={grp['matrix_map_id']}  "
                      f"clusters={grp['clusters']}  "
                      f"palettes={grp['palette_count']}  "
                      f"agree={grp['palettes_agree']}")
        if with_skeleton and skel is not None:
            print(f"   skeleton: nodes={len(skel.nodes)} "
                  f"used={len(skel.used_nodes())} "
                  f"padding={len(skel.unused_nodes())} "
                  f"indices={len(skel.node_indices)} tree={skel.is_tree()} "
                  f"child_table_ok={skel.child_ranges_tile()} "
                  f"max_rotation_slot={skel.max_rotation_slot()} "
                  f"max_movement_slot={skel.max_movement_slot()}")
            for nd in skel.nodes[:24]:
                tag = "" if nd.is_used() else "  [padding]"
                print(f"      node {nd.index:3d} parent={nd.parent:3d} "
                      f"children={nd.child_count} "
                      f"@{nd.child_start}")
                print(f"           t=({nd.translation[0]:8.3f},"
                      f"{nd.translation[1]:8.3f},{nd.translation[2]:8.3f}) "
                      f"rot={nd.rotation_slot} mov={nd.movement_slot}{tag}")
            print("      node order: "
                  + " ".join(str(i) for i in skel.node_indices[:32])
                  + (" ..." if len(skel.node_indices) > 32 else ""))
        if with_keys:
            print(f"   keys({g['keys_count']}):")
            for kv in rec.get("keys", []):
                print(f"      {kv['key']} = {kv['value']}")
        if with_mesh:
            for c in rec.get("clusters", []):
                fr = c["frames"][-1] if c["frames"] else {}
                print(f"   cluster {c['index']}: verts={fr.get('vertex_count')} "
                      f"ver={fr.get('version') and hex(fr['version'])} "
                      f"tris={c['triangle_count']} max_index={c['max_index']} "
                      f"in_range={c['index_in_range']} "
                      f"uvs={c['uv_count']} uv_match={c['uv_matches']} "
                      f"surface={c['surface_index']} flags={c['cluster_flags']} "
                      f"refbox_match={c.get('reference_box_matches_frame_bounds')} "
                      f"skin={c['skin'] and c['skin']['influence_count']} "
                      f"skin_match={c['skin_matches']} "
                      f"bone_space={c['bone_space']} max_bone={c['max_bone']} "
                      f"max_node={c['max_resolved_bone']}"
                      + (f"  ERR {c['error']}" if c["error"] else ""))
                if c.get("reference_box_min") is not None:
                    print("      reference box %s .. %s" % (
                        _fmt3(c["reference_box_min"]),
                        _fmt3(c["reference_box_max"])))
        if cluster is not None:
            for d in rec["cluster_detail"]:
                print(f"   cluster {cluster} frame ver=0x{d['version']:x} "
                      f"verts={d['vertex_count']} bounds=({d['bounds_min']} .. "
                      f"{d['bounds_max']})")
                for p in d["positions"][:8]:
                    print("      pos %8.3f %8.3f %8.3f" % tuple(p))
            for u in rec.get("uv_detail", []):
                print(f"   cluster {cluster} uv frame ver=0x{u['version']:x} "
                      f"uvs={u['uv_count']} bounds=({u['bounds_min']} .. "
                      f"{u['bounds_max']})")
                for uv in u["uvs"][:8]:
                    print("      uv  %8.4f %8.4f" % tuple(uv))
            for sk in rec.get("skin_detail", []):
                print(f"   cluster {cluster} skin verts={sk['vertex_count']} "
                      f"influences={sk['influence_count']} "
                      f"bone_space={sk['bone_space']} "
                      f"max_bone={sk['max_bone']} "
                      f"max_node={sk['max_resolved_bone']} "
                      f"matrix_map_id={sk['matrix_map_id']} "
                      f"matrix_bones={sk['matrix_bone_count']}")
                palette = sk.get("matrix_bones") or []
                if palette:
                    print("      palette(nodes)=%s"
                          % ", ".join(str(p) for p in palette))
                for i, v in enumerate(sk["vertices"][:4]):
                    inf = sk["influences"][v["influence_start"]:
                                           v["influence_start"]
                                           + v["influence_count"]]
                    print("      v%-4d bones=%s" % (
                        i, ", ".join("%d:%.3f" % (b, w) for b, w in inf)))
        if with_tree:
            for line in x.tree_lines():
                print("   " + line)
        if with_strings:
            print("   strings: " + ", ".join(rec["strings"][:40]))
    return rec


def inventory(root: str) -> list[dict]:
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for fn in sorted(files):
            if not fn.lower().endswith(".xmd"):
                continue
            p = os.path.join(dirpath, fn)
            try:
                x = Xmd(p)
                g = x.geometry_summary()
                out.append({"path": p, "size": len(x.data),
                            "nodes": len(x.nodes),
                            "triangles": g["triangle_count"],
                            "skeleton_nodes": g["skeleton_node_count"],
                            "skinned_verts": g["bone_deform_vertices"],
                            "skinned_clusters": g["skinned_clusters"],
                            "surfaces": g["surfaces"]})
            except XmdError as e:
                out.append({"path": p, "error": str(e)})
    return out


def check_files(paths: list[str]) -> int:
    """Validate container structure for each path. Returns process exit code."""
    rc = 0
    for p in paths:
        try:
            x = Xmd(p)
            problems = x.validate()
        except XmdError as e:
            print(f"FAIL {os.path.basename(p)}: {e}")
            rc = 1
            continue
        if problems:
            rc = 1
            print(f"FAIL {os.path.basename(p)}: {'; '.join(problems)}")
        else:
            print(f"ok   {os.path.basename(p)}: {len(x.nodes)} nodes, "
                  f"data 0x{x.data_start:X}..0x{len(x.data):X}")
    return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", help="a .xmd file")
    ap.add_argument("--inventory", metavar="DIR",
                    help="walk DIR for *.xmd and summarise all")
    ap.add_argument("--check", action="store_true",
                    help="validate container structure (exit 1 on problems); "
                         "use with a file or --inventory DIR")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strings", action="store_true")
    ap.add_argument("--tree", action="store_true")
    ap.add_argument("--mesh", action="store_true",
                    help="decode VERTEXFRAMES/TRIANGLES cluster pairs and "
                         "print per-cluster geometry")
    ap.add_argument("--cluster", type=int, metavar="N",
                    help="print the decoded frames (with positions) of "
                         "VERTEXFRAMES cluster N")
    ap.add_argument("--skeleton", action="store_true",
                    help="decode and print the SKELETON NODES/NODEINDICES tree")
    ap.add_argument("--keys", action="store_true",
                    help="decode and print the KEYS metadata map (not animation)")
    ap.add_argument("--matrix-map", action="store_true",
                    help="group paletted clusters by matrix_map_id (BONEMATRIXMAP)")
    args = ap.parse_args(argv)

    if args.check:
        targets = []
        if args.inventory:
            for dirpath, _dirs, files in os.walk(args.inventory):
                targets += [os.path.join(dirpath, f) for f in sorted(files)
                            if f.lower().endswith(".xmd")]
        elif args.path:
            targets = [args.path]
        else:
            ap.error("--check needs a .xmd path or --inventory DIR")
        if not targets:
            ap.error("no .xmd files found")
        return check_files(targets)

    if args.inventory:
        recs = inventory(args.inventory)
        if args.json:
            print(json.dumps(recs, indent=2))
        else:
            for r in recs:
                if "error" in r:
                    print(f"  ERR {r['path']}: {r['error']}")
                else:
                    print(f"{r['triangles']!s:>7} tri  {r['skeleton_nodes']!s:>4} bones  "
                          f"{r['size']:>9} B  {os.path.basename(r['path'])}")
        return 0

    if not args.path:
        ap.error("provide a .xmd path or --inventory DIR")
    rec = report_file(args.path, args.json, args.strings, args.tree,
                      with_mesh=args.mesh, cluster=args.cluster,
                      with_skeleton=args.skeleton, with_keys=args.keys,
                      with_matrix_map=args.matrix_map)
    if args.json:
        print(json.dumps(rec, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
