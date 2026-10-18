# ADR-0494: The `VIEWMODEL` property is the **collectible class's own model-load override** (a READ), not an inventory/weapon viewmodel (GH #67 step 1)

- Status: Accepted (scheduled autoloop wake 93, GH #67)
- Date: 2026-10-18
- Related: GH #67 (this), GH #66 (per-draw interception, approach 2), GH #63/#64
  (the engine's own FP arms), ADR-0492 (the engine-side identification this
  corrects), ADR-0493 (the decompilation of `FUN_10265a70` this answers),
  ADR-0435/0438 (`noviewmodel`), ADR-0481/0482 (the transform is engine-side),
  ADR-0491

## Context

ADR-0492 identified the engine-side mechanism from the live (2009) `GameClasses`
module by raw byte scan: the first-person viewmodel is an item *property*
`VIEWMODEL` with an attach configuration `ATTACHPOINT`/`ATTACHROTTRACK`, its sole
code xref is the `push "VIEWMODEL"` inside `FUN_10265a70`, and the strings sit
immediately before the class-name string `CRPG_Object_Inventory`. ADR-0493 then
decompiled `FUN_10265a70` (`{ FUN_10260940(); CStr::CStr("VIEWMODEL");
FUN_1026d790(); }`) and left two questions open, naming them in its "not claimed"
section:

> Whether `FUN_10265a70` reads or *sets* `VIEWMODEL` (its callees … and its callers
> are not decompiled). … Whether the viewmodel model is created from this property
> (still an inference from the name; the consumer is not located).

This ADR spends autoloop wake 93 on exactly those two questions.

## Decision

Record the measured answer and correct ADR-0492's attribution. No game code, no
hook code, no default, no deploy, no live run — REA plus a static-table read of
the same file bytes.

### Measured (read-only; `docs/rea/reference/GameClasses2009_ViewmodelConsumer.txt`)

Subject `Content\GameClasses_Win32_x86.dll`, 9,016,560 B,
`sha256 d7e81d7d3726…`, provider Ghidra 12.1.4, `confidence = observed`,
`authority = shipped-artifact`. Three requests, `exit 0`, 390 s / 378 s / 369 s.

1. **The site is reached only as a vtable slot.** `rea xrefs 0x10265a70` returns
   exactly one incoming reference, `0x10759ed4`, a `.rdata` data location; an
   image-wide 4-byte scan finds the same single occurrence. No function calls
   `FUN_10265a70`.
2. **`FUN_1026d790` is the "copy the named child of the definition into the
   object" helper** — `name = arg->vtable[0x18]()` (the temporary `CStr`),
   `CRegistry::FindChild(registry, name)`, then the found child's value
   (`child->vtable[0x1b4]()` → `temp->vtable[0x14](1)`) handed to the destination's
   `vtable[0x68]`. **Direction: definition registry → object.**
3. **`FUN_10260940` is the shared item model-property loader**, and it uses the
   *same* helper for its own keys: `MODEL`, `MODEL0..MODEL3`, `MODEL_DUMMY`,
   `PHYSMODEL`, `ATTACHMODEL`, `SOUND_*` (13 keys), `ICONSURFACE`/`AMMOICONSURFACE`/
   `HUDSURFACE`/`ACTIVATIONSURFACE`/`EXTRASURFACE`, `ANIM_HOLD`/`ANIM_ATTACK`/
   `ANIM_BLOCK`/`ANIM_RELOAD`/`ANIM_RELOAD_CROUCH`/`ANIM_EQUIP`/`ANIM_NOAMMOHOLD`/
   `ANIM_NOAMMO`, `ANIMGRAPH2`, `EFFECTLIST`, `PICKUPMSG`. `FUN_10265a70` is
   therefore `FUN_10260940()` **plus** a `VIEWMODEL` read — a loader override.
4. **The owning class is `CRPG_Object_Collectible`, not `CRPG_Object_Inventory`.**
   The slot's vtable starts at `0x10759eb4` (slot index **8**); its `-4` RTTI
   CompleteObjectLocator is `0x107ab9d4`, whose TypeDescriptor `0x1082d888` is
   `.?AVCRPG_Object_Collectible@@`. Slot **12** of the same vtable is
   `FUN_10265ab0`, the `ATTACHPOINT`/`ATTACHROTTRACK` reader — i.e. ADR-0492's
   "one neighbourhood" is one class's adjacent methods.
5. **Class registry (`.data` 0x1082d838…0x1082dda0)** records
   `{name, factory, parent}`: `CRPG_Object` (parent `CReferenceCount`),
   `CRPG_Object_Item` (parent `CRPG_Object`), `CRPG_Object_Collectible` (parent
   `CRPG_Object_Item`), `CRPG_Object_Inventory` (parent `CRPG_Object` — a *sibling*
   branch), `CRPG_Object_Gun`/`AngelusChain` (parent `CRPG_Object_Weapon`),
   `CRPG_Object_Melee` (parent `CRPG_Object_Item`), `CRPG_Object_Ulaks` (parent
   `Melee`), `CRPG_Object_RocketMelee` (parent `CRPG_Object_Summon`),
   `CRPG_Object_CreatureAttack`/`Ammo` (parent `CRPG_Object_Item`),
   `CRPG_Object_Char` (parent `CRPG_Object`).
6. **Slot 8 is found in exactly four vtables** (an image-wide scan of the
   loader's address): `CRPG_Object_Item`, `CRPG_Object_CreatureAttack`,
   `CRPG_Object_Telephone_Receiver` (all = the base loader `FUN_10260940`) and
   `CRPG_Object_Collectible` (= the `VIEWMODEL`-reading override `FUN_10265a70`).
   **No weapon-class vtable holds either function.**

### Consequence for GH #67 step 1

- The read-vs-set question is **closed on the READ side**: `VIEWMODEL` is a
  *definition key* read into the object at model-load time, through the same code
  path as `MODEL`/`MODEL0..3`/`ATTACHMODEL`.
- The attribution is corrected: the class is `CRPG_Object_Collectible`; the
  `CRPG_Object_Inventory` proximity in `.rdata` is string-pooling order.
- *"The engine's first-person arms are the item property `VIEWMODEL`"* is **not
  supported**: no weapon class reads that name, and the only reader is the
  collectible class's model-load override. The engine-side search for the FP arms
  must move off this property (the item model pipeline is still the right *place*
  to look: the engine names models by definition keys, and `MODEL`/`MODEL_DUMMY`/
  `ATTACHMODEL` route through the object's property setters, not through GL state).

## Consequences

- ADR-0492 §2/§5 get CORRECTED banners pointing here (its raw-scan facts — the
  property strings, their single code xref, the `noviewmodel` flag table — stand;
  the *class attribution* and the "the arms are this property" reading do not).
  The pinning invariant is not weakened: the old artifact must keep recording the
  raw-scan facts.
- The loop must not spend further wakes trying to isolate the FP arms through item
  properties named `VIEWMODEL`; the next engine-side step is the object's
  property-setter target (destination vtable slot `0x68`) and the model-instance
  creation it feeds — a *different* name than `VIEWMODEL` leads there.
- Observation/REA only: no hook code, no INI default changed, no deploy, no install
  write, no live run.

## Validation

- `docs/rea/reference/GameClasses2009_ViewmodelConsumer.txt` carries the subject
  hash, the three requests with wall clocks and evidence ids, both decompilations
  **verbatim**, the vtable/RTTI read, the class-registry hierarchy, the vtable
  ownership scan, the local reproducer, and an explicit "not claimed" section.
- `tools/verify_reported_fixes.py` gains a GH #67 invariant pinning the new
  artifact (module hash, `CRPG_Object_Collectible`, `0x10759eb4`,
  `.?AVCRPG_Object_Collectible@@`, `0x10759ed4`, `FUN_1026d790`, `FUN_10260940`,
  `ATTACHMODEL`, `PICKUPMSG`, `0x1075aa0c`, `0x1078957c`) and this ADR.
- No C++ changed → no `build-all`/CTest; the offline guards
  (`verify_reported_fixes`, `scenario_tests.py --check`, `verify_hook_init`,
  `validate_addresses`, `verify_test_artifacts`, `xrsim --selftest`,
  `tools/test_*.py`) are the validation.
