#!/usr/bin/env python3
"""Self-contained tests for ``tools/gl_vocabulary_scan.py`` (GH #67, ADR-0481).

The scan is *pure* (bytes in, counts out), so it is tested on a synthetic blob
that mimics a module's import/name strings -- no game install, no binary needed.
It also pins the two decisive negatives the corrected GH #67 plan rests on: a
module that composes with the fixed-function stack is distinguished from one
that uses ARB vertex programs.

Run headlessly:  python tools/test_gl_vocabulary_scan.py
Exit code 0 = pass, 1 = fail.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gl_vocabulary_scan as scan  # noqa: E402  (path injected above)

_failures = 0


def check(condition, message):
    global _failures
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        _failures += 1


def _blob(*names):
    """A fake module body: each name as a null-terminated C string."""
    return b"".join(n.encode("ascii") + b"\x00" for n in names)


def _synthetic_pe(imports):
    """A minimal 32-bit PE image importing `imports` (list of names) from OPENGL32.dll.

    Enough of the format for `pe_imports`: DOS + COFF + optional header with one
    data directory (import), one .rdata section holding the descriptor, the ILT,
    the IAT and the hint/name table.
    """
    import struct

    hint_names = b""
    ilt_entries = []
    name_off = 0x1070
    for n in imports:
        ilt_entries.append(name_off)
        hint_names += struct.pack("<H", 0) + n.encode("ascii") + b"\x00"
        name_off += 2 + len(n) + 1
    ilt_entries.append(0)

    section_off = 0x200
    vaddr = 0x1000

    def off(rva):
        return section_off + (rva - vaddr)

    size = 0x400
    img = bytearray(b"\x00" * size)
    # DOS header + e_lfanew.
    img[0:2] = b"MZ"
    struct.pack_into("<I", img, 0x3C, 0x40)
    # PE signature + COFF header.
    img[0x40:0x44] = b"PE\0\0"
    coff = 0x44
    struct.pack_into("<HHIIIHH", img, coff, 0x14C, 1, 0, 0, 0, 0xE0, 0x0102)
    opt = coff + 20
    struct.pack_into("<H", img, opt, 0x10B)  # PE32
    ddir = opt + 96
    struct.pack_into("<II", img, ddir + 8, 0x1000, 40)  # import dir
    # Section header: .rdata at vaddr 0x1000 -> raw 0x200.
    sec = opt + 0xE0
    img[sec:sec + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", img, sec + 8, 0x400, vaddr, 0x200, section_off)
    # Import descriptor.
    struct.pack_into("<IIIII", img, off(0x1000), 0x1040, 0, 0, 0x1060, 0x1050)
    # ILT + IAT.
    for i, v in enumerate(ilt_entries):
        struct.pack_into("<I", img, off(0x1040) + i * 4, v)
        struct.pack_into("<I", img, off(0x1050) + i * 4, v)
    # DLL name.
    img[off(0x1060):off(0x1060) + 13] = b"OPENGL32.dll\0"
    # Hint/name table.
    img[off(0x1070):off(0x1070) + len(hint_names)] = hint_names
    return bytes(img)


def _check_pe_parser():
    pe = _synthetic_pe(["glLoadMatrixf", "glProgramStringARB"])
    imports = scan.pe_imports(pe)
    check(imports.get("OPENGL32.dll") == ["glLoadMatrixf", "glProgramStringARB"],
          "the PE import parser must read the import table")
    check(scan.open_gl_imports(pe) == ["glLoadMatrixf", "glProgramStringARB"],
          "open_gl_imports must match OPENGL32.dll case-insensitively")
    try:
        scan.pe_imports(b"not a PE")
        check(False, "a non-PE buffer must raise ValueError")
    except ValueError:
        pass


def main():
    # A name surrounded by a NUL is an import / GetProcAddress literal; a bare
    # byte match alone must not be counted as one.
    data = _blob("glDrawElements", "glProgramStringARB") + b"glMultMatrixfx"
    counts = scan.scan_names(data, ["glDrawElements", "glProgramStringARB", "glMultMatrixf"])
    check(counts["glDrawElements"]["cstr"] == 1, "a null-terminated name counts as a cstr")
    check(counts["glProgramStringARB"]["cstr"] == 1, "every null-terminated name is counted")
    check(counts["glMultMatrixf"]["bytes"] == 1 and counts["glMultMatrixf"]["cstr"] == 0,
          "an embedded name is a byte match but not a cstr")
    check("glRotatef" not in counts, "an absent name is not reported")

    # Repeated occurrences (an import plus a literal) are both counted.
    counts = scan.scan_names(_blob("glClear", "glClear"), ["glClear"])
    check(counts["glClear"] == {"bytes": 2, "cstr": 2}, "repeats are counted, not deduped")

    # The decisive shape: the live 2009 engine's vocabulary. A module that uses
    # ARB vertex programs and NOT the fixed-function composition calls must be
    # reported as such -- this is the measurement that corrected the plan.
    live_2009 = _blob(
        "glDrawElements", "glCallList", "glBegin", "glEnd",
        "glLoadMatrixf", "glLoadIdentity", "glPushMatrix", "glPopMatrix",
        "glTranslatef", "glFrustum", "glOrtho", "glMatrixMode", "glClear",
        "glVertexPointer", "glNormalPointer", "glColorPointer",
        "glEnableClientState", "glDisableClientState",
        "glProgramStringARB", "glBindProgramARB", "glGenProgramsARB",
        "glProgramEnvParameter4fvARB", "glVertexAttribPointerARB",
        "wglGetProcAddress",
    )
    groups = scan.group_presence(scan.scan_names(live_2009))
    check(groups["program"]["present"] ==
          ["glProgramStringARB", "glBindProgramARB", "glGenProgramsARB",
           "glProgramEnvParameter4fvARB"],
          "the ARB vertex-program entry points are detected")
    for absent in ("glMultMatrixf", "glMultMatrixd", "glLoadMatrixd",
                   "glRotatef", "glScalef", "glDepthRange"):
        check(absent in groups["fixed_function_matrix"]["absent"],
              f"{absent} must be reported absent for the vertex-program engine")
    check("glTexCoordPointer" in groups["attributes"]["absent"],
          "the generic-attribute path implies no glTexCoordPointer")

    # CLI: --module on a real file, JSON shape, and a missing file is a clean error.
    import json
    import tempfile
    with tempfile.TemporaryDirectory(prefix="riddickvr-glvocab-") as tmp:
        path = os.path.join(tmp, "RndrGL.dll")
        with open(path, "wb") as fh:
            fh.write(live_2009)
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = scan.main(["--module", path, "--json"])
        check(rc == 0, "scanning a readable module exits 0")
        report = json.loads(buf.getvalue())
        check(report["size"] == len(live_2009), "the report carries the module size")
        check(report["groups"]["program"]["present"], "the JSON report carries group presence")
        check(scan.main(["--module", os.path.join(tmp, "nope.dll")]) == 2,
              "an unreadable module is a clean non-zero exit")

    # The static import table is the authoritative "which GL calls does the module
    # bind to OPENGL32.dll" answer (the byte scan only sees name strings).
    _check_pe_parser()

    if _failures:
        print(f"gl vocabulary scan: {_failures} failure(s)", file=sys.stderr)
        return 1
    print("gl vocabulary scan: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
