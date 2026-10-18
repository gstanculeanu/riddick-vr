# ADR-0496: the item-model destination `vtable[0x68]` is a per-class virtual, and on `CRPG_Object_Item` a second *property loader* — not the model-instance creation (GH #67 step 1)

- Status: Accepted (scheduled autoloop wake 95, GH #67)
- Date: 2026-10-18
- Related: GH #67 (this), GH #80 (the state question this hands off to), GH #66/#63
  (the engine's own FP arms), ADR-0494 (whose "Next 1a" this closes), ADR-0495
  (which closed "Next 1b"), ADR-0493, ADR-0492, ADR-0481/0482

## Context

ADR-0494 left two named routes for GH #67 step 1:

> (1a) the object's property-setter target (destination vtable slot `0x68`) and the
> model-instance creation it feeds — a *different* name than `VIEWMODEL` leads
> there; (1b) chase the `FirstPerson`/`ThirdPerson` view-mode table …

ADR-0495 closed (1b) (the block is a `CWObject_Matinee` script key). This ADR
measures (1a) and closes it.

## Decision

Record the measured identity of the destination `vtable[0x68]` in the item-model
helper and correct ADR-0494's (1a) reading. No game code, no hook code, no default,
no deploy, no live run — one read-only REA decompile plus a local static
vtable/RTTI census and disassembly of the same file bytes.

### Measured (read-only; `docs/rea/reference/GameClasses2009_ModelSetterSlot68.txt`)

Subject `Content\GameClasses_Win32_x86.dll`, 9,016,560 B,
`sha256 d7e81d7d3726…`, `confidence = observed`, `authority = shipped-artifact`;
`rea decompile 0x10252d70` exit 0 in 380 s
(`ev_45c80b840286f193cd0f146d8987b7ccbd0f694060a2ad78b50071a45018eb75`).

1. **The destination is a different object from the registry, and the call is one
   per-class virtual dispatch with one value argument.** The disassembly of
   `FUN_1026d790` resolves the two stack arguments: `arg5` is the registry handed
   to `CRegistry::FindChild` (`0x10283bba`), `arg6` is the destination whose
   `vtable[0x68]` receives the found child's value (`mov esi,[esp+2Ch]; mov
   edi,[esi]; … mov edx,[edi+68h]; push eax; mov ecx,esi; call edx`).
2. **Slot `0x68` is a per-class virtual, not one global function.** An image-wide
   MSVC RTTI vtable census finds **1,319 vtables / 874 distinct slot-0x68
   targets** (582 into `.text`; 737 into adjacent `.rdata` because the vtable is
   shorter than 27 slots — those are not claimed). Within `CRPG_Object*` the
   target differs per class (`CRPG_Object` `0x1025df20`, `_Weapon` `0x1024c320`,
   `_Inventory` `0x1024ebb0`, `_Char` `0x1024cdd0`, `_Ammo` `0x10273d60`, …).
3. **On the item class it is a second property loader.** `CRPG_Object_Item`'s
   slot `0x68` is `FUN_10252d70`, which calls the slot-8 item model-property
   loader `FUN_10260940()` and then loads **SPAWN**, **PROJECTILE**,
   **SPAWNEFFECT**, **SUMMON_PROJECTILECLASS**, **SUMMON_PROJECTILECLASS_PLAYER**,
   **GIVEONPICKUP**, **MELEE_HITSOUND** (the six literals Ghidra left unnamed were
   recovered by local disassembly). Its shape is a loader, with `ret 0xC`
   (three stack arguments).

### Consequence for GH #67 step 1

- **(1a) as stated is not supported.** The class whose slot-8 loader *is* the item
  model loader (`CRPG_Object_Item`) has a *loader* at slot `0x68`, not a
  model-instance creator; the destination in `FUN_1026d790` is a different object
  whose class is not identified here.
- With ADR-0495 (1b) and this ADR (1a), **every named engine-side route of
  ADR-0494 is exhausted**: the geometric routes, the name tables, the `VIEWMODEL`
  property, and now the destination slot are all measured negatives.
- The remaining route is the **state question** (ADR-0491/0492): whether the engine
  draws its first-person arms *at all* on the sampled **passive/unarmed** level
  start. Until that is answered, an empty first-person window is not evidence
  about the arms. It is a harness problem — a scripted run that reaches an
  armed/fighting state — not another static hunt (GH #80).

## Consequences

- The loop must not spend further wakes treating "the destination `vtable[0x68]`"
  as the model-instance creation; the model reference is *stored* there, and the
  consumer (the model *instance* creation) is elsewhere.
- The pinning invariant is extended with this artifact (hash, `0x10252d70`,
  `FUN_10260940`, the seven recovered literals, `GIVEONPICKUP`, plus the
  per-class census singleton targets) and this ADR, so the correction cannot
  silently rot.
- Observation/REA only: no hook code, no INI default changed, no deploy, no
  install write, no live run.

## Validation

- `docs/rea/reference/GameClasses2009_ModelSetterSlot68.txt` carries the subject
  hash, the request with wall clock and evidence id, the two disassemblies, the
  decompilation, the census, the `CRPG_Object*` table, and an explicit
  "not claimed" section.
- `tools/verify_reported_fixes.py` GH #67 invariant extended (the checks above and
  this ADR).
- No C++ changed → no `build-all`/CTest; the offline guards
  (`verify_reported_fixes`, `scenario_tests.py --check`, `verify_hook_init`,
  `validate_addresses`, `verify_test_artifacts`, `xrsim --selftest`,
  `tools/test_*.py`) are the validation.
