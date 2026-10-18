# ADR-0495: The `FirstPerson`/`ThirdPerson` name block is a **CWObject_Matinee** script key, not the player's FP body/arms (GH #67 step 1)

- Status: Accepted (scheduled autoloop wake 94, GH #67)
- Date: 2026-10-18
- Related: GH #67 (this), GH #66/#63/#64 (the engine's own FP arms), GH #80 (the
  simulator state question), ADR-0492 (sec 4, corrected here), ADR-0493,
  ADR-0494 (whose "Next 1b" this closes), ADR-0481/0482

## Context

ADR-0492 recorded two `.data` entries as a "view-mode / camera-mode name list"
for an entity: VA `0x1083ff68 -> 0x1078be28 "FirstPerson"` and
VA `0x1083ff6c -> 0x1078be1c "ThirdPerson"`, and left their consumer unlocated.
ADR-0492 then attributed the `VIEWMODEL` property cluster to
`CRPG_Object_Inventory`; ADR-0493 decompiled `FUN_10265a70`; ADR-0494 corrected
the class to `CRPG_Object_Collectible` and named two candidate "Next" steps for
#67 step 1:

> (1a) the object's property-setter target (destination vtable slot `0x68`) and
> the model-instance creation it feeds; (1b) chase the `FirstPerson`/`ThirdPerson`
> view-mode table (`0x1083ff68`/`0x1083ff6c`) to the camera/body-attach code --
> that is where the FP body/arms decision lives.

This ADR measures (1b) read-only and closes it.

## Decision

Record the measured identity of the `FirstPerson`/`ThirdPerson` block and correct
ADR-0492 sec 4's reading. No game code, no hook code, no default, no deploy, no
live run -- a static PE/byte scan plus an RTTI read of the same file bytes.

### Measured (read-only; `docs/rea/reference/GameClasses2009_ViewmodeConsumer.txt`)

Subject `Content\GameClasses_Win32_x86.dll`, 9,016,560 B,
`sha256 d7e81d7d3726�?�`, `confidence = observed`, `authority = shipped-artifact`,
**no** REA/Ghidra request (wall < 1 s).

1. **The block is a matinee key list.** `0x1083ff68 -> "FirstPerson"`,
   `0x1083ff6c -> "ThirdPerson"`, `0x1083ff70 -> "KeepEndFade"`,
   `0x1083ff74 -> "NoSkip"`, terminated by two zero dwords, followed by
   `0x1083ff80 -> "CWObject_Matinee"`. `KeepEndFade`/`NoSkip` are matinee playback
   options, so the run of strings is a **script/camera key list**, not a global
   view-mode enum.
2. **The block is referenced by exactly one instruction.**
   `0x105ec068  push 0x1083ff68`. An image-wide 4-byte scan finds no other code
   reference to `0x1083ff68`, **zero** to `0x1083ff6c`, and the strings
   `0x1078be28`/`0x1078be1c` themselves have **zero** code references (one data
   reference each: the block entries). So the block is reachable only through that
   one `push`.
3. **The site is a keyed dispatch.** Just before the push the code does
   `cmp edi, 0x4d68028a` / `cmp edi, 0x6b2e3218` / `jne ...`, then
   `push 0x1083ff68; lea ecx,[esp+0x20]; call 0x10026c30; mov [esi+0x3b8], eax`.
   The `FirstPerson`/`ThirdPerson` block is the argument for one key of a 32-bit
   keyed dispatcher.
4. **The containing function is a `CWObject_Matinee` virtual method.** The nearest
   `55 8B EC` prologue is `0x105ebf40`, the containing function. Its address
   occurs once in the image, as `.rdata` dword `0x1078c120`, i.e. **slot 15** of
   the vtable at `0x1078c0e4` (`(0x1078c120-0x1078c0e4)/4 = 15`; every
   neighbouring entry is a `.text` pointer). That vtable's `-4`
   CompleteObjectLocator `0x107b97e0` has `pTypeDescriptor 0x10840238 =
   ".?AVCWObject_Matinee@@"`; the same `.rdata` neighbourhood carries
   `CWO_Matinee_ClientData` and `.\WObj_Misc\WObj_Matinee.cpp`.

### Consequence for GH #67 step 1

- ADR-0494's candidate **(1b) is falsified**: the `FirstPerson`/`ThirdPerson`
  block is a **`CWObject_Matinee`** (cutscene object) key, and its only consumer is
  a single matinee method. There is no route from it to the player's first-person
  body/arms.
- ADR-0492 sec 4 is **corrected**: the two entries are *matinee camera-mode script
  keys* next to the matinee options `KeepEndFade`/`NoSkip`, not an engine
  view-mode table that drives the FP vs TP player camera.
- The remaining #67 step-1 candidates are ADR-0494's **(1a)** (the object
  property-setter target `vtable[0x68]` and the model-instance creation it feeds)
  and the **state question** (whether the arms are drawn on the sampled passive
  level start -- GH #80 / wake 89). One candidate is removed.

## Consequences

- The loop must not spend further wakes chasing the `FirstPerson`/`ThirdPerson`
  block as the FP-body decision.
- The pinning invariant is extended with this artifact (hash, the block VAs, the
  single `push` site `0x105ec068`, the function `0x105ebf40`, vtable `0x1078c0e4`,
  slot 15, RTTI `.?AVCWObject_Matinee@@`, `WObj_Matinee.cpp`) and this ADR, so
  the correction cannot silently rot.
- Observation/REA only: no hook code, no INI default changed, no deploy, no
  install write, no live run.

## Validation

- `docs/rea/reference/GameClasses2009_ViewmodeConsumer.txt` carries the subject
  hash, the reproducer, the verbatim block, the sole code reference with its
  disassembly, the vtable/RTTI read, the hash-probe negative, and an explicit
  "not claimed" section.
- `tools/verify_reported_fixes.py` GH #67 invariant extended (the checks above and
  this ADR).
- No C++ changed -> no `build-all`/CTest; the offline guards
  (`verify_reported_fixes`, `scenario_tests.py --check`, `verify_hook_init`,
  `validate_addresses`, `verify_test_artifacts`, `xrsim --selftest`,
  `tools/test_*.py`) are the validation.
