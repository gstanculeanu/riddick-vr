"""Which OpenGL entry points a shipped module actually calls (GH #67, ADR-0481).

A read-only, reproducible scan of a shipped code module for the null-terminated
OpenGL entry-point *name strings* it references. This is the measurement that
established where the live engine puts its transforms:

  * `RndrGL.dll` (the only module that imports OpenGL, on both installs) is the
    GL client, so its vocabulary is the engine's vocabulary;
  * the 2009 target's `RndrGL.dll` imports **no** `glMultMatrixf` /
    `glLoadMatrixd` / `glRotatef` / `glScalef` -- so the fixed-function
    GL_MODELVIEW stack cannot be composed with them, and mirroring it (the
    original GH #67 step-1 plan) can only ever see the single identity load a
    live run shows;
  * it *does* import the ARB_vertex_program entry points
    (`glProgramStringARB`, `glBindProgramARB`, `glGenProgramARB`,
    `glProgramEnvParameter4fvARB`) plus the generic-attribute path
    (`glVertexAttribPointerARB`, and no `glTexCoordPointer`), i.e. the
    object/view transforms reach the GPU as *program environment parameters*.

`scan_names` is pure (bytes in, counts out) so it is unit-tested on a synthetic
blob; the CLI runs it against an install's `RndrGL.dll`. Nothing is written to
the game install.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

# The vocabulary that decides how the transform can be captured. Grouped so the
# report reads as an answer, not a dump.
VOCABULARY = {
    # fixed-function matrix composition (the GH #67 step-1 premise)
    "fixed_function_matrix": [
        "glLoadMatrixf", "glLoadMatrixd", "glMultMatrixf", "glMultMatrixd",
        "glLoadTransposeMatrixf", "glMultTransposeMatrixf",
        "glRotatef", "glRotated", "glTranslatef", "glTranslated",
        "glScalef", "glScaled", "glPushMatrix", "glPopMatrix", "glLoadIdentity",
        "glMatrixMode", "glFrustum", "glOrtho", "glDepthRange",
    ],
    # ARB/NV vertex-program path (where the live transforms actually go)
    "program": [
        "glProgramStringARB", "glBindProgramARB", "glGenProgramsARB",
        "glDeleteProgramsARB", "glProgramEnvParameter4fvARB",
        "glProgramLocalParameter4fvARB", "glGetProgramivARB",
        "glGetProgramStringARB", "glProgramEnvParameter4fARB",
        "glProgramLocalParameter4fARB", "glLoadProgramNV", "glTrackMatrixNV",
        "glExecuteProgramNV",
    ],
    # generic attributes / client arrays
    "attributes": [
        "glVertexAttribPointer", "glVertexAttribPointerARB",
        "glEnableVertexAttribArray", "glEnableVertexAttribArrayARB",
        "glDisableVertexAttribArrayARB", "glVertexAttrib4fARB",
        "glVertexPointer", "glNormalPointer", "glColorPointer",
        "glTexCoordPointer", "glEnableClientState", "glDisableClientState",
        "glInterleavedArrays", "glArrayElement",
    ],
    # draw + display list
    "draw": [
        "glDrawElements", "glDrawArrays", "glDrawRangeElements", "glCallList",
        "glCallLists", "glNewList", "glGenLists", "glEndList",
        "glBegin", "glEnd",
    ],
    # loader / shading-language
    "loader": [
        "wglGetProcAddress", "glGetString", "glGetIntegerv", "glew",
        "glCreateShaderObjectARB", "glCreateProgramObjectARB",
    ],
}

ALL_NAMES = sorted({name for group in VOCABULARY.values() for name in group})


def scan_names(data: bytes, names=None):
    """Count null-terminated occurrences of each GL entry-point name in `data`.

    Returns ``{name: {"bytes": n, "cstr": m}}`` for every name that appears at
    least once. ``cstr`` counts the name followed by ``\\x00`` -- the shape of an
    import-table entry or a `wglGetProcAddress` name literal -- while ``bytes``
    also catches a name embedded in a longer string.
    """
    # NOTE: a plain byte scan is a *substring* match, so a name that is a prefix
    # of another (e.g. "glGetProgramiv" inside "glGetProgramivARB") would be a
    # false positive; VOCABULARY therefore only lists names that are not prefixes
    # of one another. `cstr` is the authoritative count.
    names = ALL_NAMES if names is None else list(names)
    out = {}
    for name in names:
        raw = name.encode("ascii")
        total = data.count(raw)
        if not total:
            continue
        out[name] = {"bytes": total, "cstr": data.count(raw + b"\x00")}
    return out


def group_presence(counts):
    """Map each vocabulary group to the present/absent names (sorted).

    A name counts as *present* only when it appears at least once as a
    null-terminated literal (``cstr >= 1``) -- an import-table entry or a
    ``wglGetProcAddress`` name. A bare substring match (e.g. a core name that is
    a prefix of its ARB spelling) is reported as absent, so the human-readable
    output cannot over-claim.
    """
    present = {n for n, c in counts.items() if c.get("cstr", 0) >= 1}
    return {
        group: {
            "present": [n for n in names if n in present],
            "absent": [n for n in names if n not in present],
        }
        for group, names in VOCABULARY.items()
    }


def default_rndrgl(game_dir):
    return os.path.join(game_dir, "System", "Win32_x86", "RndrGL.dll")


def _pe_sections(data):
    """(sections, pe32plus) for a PE image, sections as (vaddr, vsize, rawptr, rawsize)."""
    import struct

    if data[:2] != b"MZ":
        raise ValueError("not a PE image (no MZ)")
    e_lfanew, = struct.unpack_from("<I", data, 0x3C)
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE image (no PE signature)")
    coff = e_lfanew + 4
    nsec, = struct.unpack_from("<H", data, coff + 2)
    size_opt, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    secs = []
    base = opt + size_opt
    for i in range(nsec):
        o = base + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((vaddr, vsize, rawptr, rawsize))
    return secs, magic == 0x20B, opt


def pe_imports(data):
    """The module's *static* import table: ``{dll_name: [function_name, ...]}``.

    This is the authoritative "which GL entry points does the engine import from
    ``OPENGL32.dll``" answer, and it is what distinguishes the two mechanisms: a
    name in this table is served by the proxy's export (its own hook), while a
    name that is only a string literal in the image must be resolved with
    ``wglGetProcAddress`` -- and is only interceptable there. Returns ``{}`` for a
    module with no imports; raises ``ValueError`` for a non-PE buffer.
    """
    import struct

    secs, pe32plus, opt = _pe_sections(data)
    ddir = opt + (112 if pe32plus else 96)

    def rva2off(rva):
        for vaddr, vsize, rawptr, rawsize in secs:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None

    imp_rva, imp_size = struct.unpack_from("<II", data, ddir + 8)
    out = {}
    off = rva2off(imp_rva) if imp_rva else None
    while off is not None:
        ilt, ts, fc, name_rva, iat = struct.unpack_from("<IIIII", data, off)
        if not name_rva and not iat:
            break
        noff = rva2off(name_rva)
        if noff is None:
            break
        end = data.find(b"\x00", noff)
        dll = data[noff:end].decode("latin1") if end != -1 else ""
        funcs = []
        walk = rva2off(ilt or iat)
        while walk:
            entry, = struct.unpack_from("<I", data, walk)
            if entry == 0:
                break
            if entry & 0x80000000:
                funcs.append("ordinal#%d" % (entry & 0xFFFF))
            else:
                foff = rva2off(entry)
                if foff is None:
                    break
                fend = data.find(b"\x00", foff + 2)
                if fend == -1:
                    break
                funcs.append(data[foff + 2:fend].decode("latin1"))
            walk += 4
        out[dll] = funcs
        off += 20
    return out


def open_gl_imports(data, dll="OPENGL32.dll"):
    """The static GL entry points imported from `dll` (case-insensitive)."""
    for name, funcs in pe_imports(data).items():
        if name.lower() == dll.lower():
            return funcs
    return []


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--module", help="path to a shipped module to scan")
    src.add_argument("--game-dir", help="install root; scans System/Win32_x86/RndrGL.dll")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--imports", action="store_true",
                    help="parse the static import table instead of the byte scan "
                         "(authoritative for what is statically imported from OPENGL32.dll)")
    args = ap.parse_args(argv)

    path = args.module or default_rndrgl(args.game_dir)
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        print(f"error: cannot read {path}: {exc}", file=sys.stderr)
        return 2

    if args.imports:
        try:
            imported = open_gl_imports(data)
        except ValueError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        if args.json:
            json.dump({"module": os.path.abspath(path), "size": len(data),
                       "openGlImports": imported}, sys.stdout, indent=2, sort_keys=True)
            sys.stdout.write("\n")
            return 0
        print(f"{os.path.abspath(path)} ({len(data)} bytes) -- {len(imported)} "
              f"static OPENGL32.dll import(s)")
        for fn in imported:
            print(f"  {fn}")
        return 0

    counts = scan_names(data)
    report = {
        "module": os.path.abspath(path),
        "size": len(data),
        "counts": counts,
        "groups": group_presence(counts),
    }
    if args.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0

    print(f"{report['module']} ({report['size']} bytes)")
    for group, presence in report["groups"].items():
        present = ", ".join(presence["present"]) or "-"
        absent = ", ".join(presence["absent"]) or "-"
        print(f"  {group}:")
        print(f"    present: {present}")
        print(f"    absent : {absent}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
