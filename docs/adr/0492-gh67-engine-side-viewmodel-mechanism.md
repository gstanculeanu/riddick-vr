# ADR-0492: The engine's first-person viewmodel is an item *property* of `CRPG_Object_Inventory`, not a GL pass (GH #67 step 1, engine-side identification)

- Status: Accepted (scheduled autoloop wake 90, GH #67)
- Date: 2026-10-18
- Related: GH #67 (this), GH #63 / GH #66 (the suppression boundary), GH #68
  (iteration vehicle), ADR-0435 / ADR-0438 (`noviewmodel`, the RPG item flag),
  ADR-0482/0483 (submitted-vertex capture), ADR-0484 (the planar candidate is not
  the viewmodel), ADR-0487/0488 (window openers + the projection near plane),
  ADR-0489/0490 (per-interval sampling; the near-field candidate falsified),
  ADR-0491 (the immediate-mode census is empty on the level)

## Context

Nine corrections of GH #67 step 1 have attacked the first-person viewmodel
*through GL*, and every route is now measured dead on the live target:

| correction | route | measured |
|---|---|---|
| ADR-0482/0483 | sample the submitted client vertices, in/out of the FP window | the window's largest in-window draw is a **plane** |
| ADR-0484 | grade the candidate's shape (volumetric?) | planar ⇒ not the arms |
| ADR-0487 | two window openers (depth-clear vs near-drop) | neither isolates the pass |
| ADR-0488 | extract the engine's z-flipped near plane | works, but the opener still is not the arms |
| ADR-0490 | window-independent near-field candidate | the level submits **no** draw within ±16 units |
| ADR-0491 | census the immediate-mode mechanism | the window holds **0** `glBegin` blocks |

ADR-0490/0491 concluded the remaining route is **engine-side identification of the
viewmodel pass** (GH #66 approach 2), i.e. ask the engine what the viewmodel *is*
rather than guessing a GL signature. This ADR records that answer for the live
(2009 *Assault on Dark Athena*) build.

## Decision

Record the engine-side mechanism (no code change; REA artifact only) and re-target
GH #67 step 1 at it.

### What is measured (read-only raw scan; `docs/rea/reference/GameClasses2009_ViewmodelProperties.txt`)

Subject: `Content\GameClasses_Win32_x86.dll`, 9,016,560 B,
`sha256 d7e81d7d3726d8f75e320c76e237b5fae69a0bea2f64ef729c53e3c1ddd72b79` — the
**2009** build (the deep-REA corpus in the same directory is all the 2004 build).

1. **The RPG item-flag name table is the 2009 build's own copy**, bit-ordered at VA
   `0x1082d728`: `wallcollision` (bit 0), **`noviewmodel` (bit 1)**,
   `playerlaserbeam` (2), `__equipped__` (3), `rpgmodelflags` (4), `aiming` (5),
   `playerflashlight` (6), `forcewalk` (7) … `nomerge` (31). This reproduces
   ADR-0435/ADR-0438's `noviewmodel = 1 << 1` on the target build and confirms the
   table is a per-bit *name list* (each name is reached only through the array).
2. **`VIEWMODEL`, `ATTACHPOINT`, `ATTACHROTTRACK` are item-class property names**
   (`.rdata` VAs `0x1075a144` / `0x1075a150` / `0x1075a15c`), declared immediately
   before the class-name string `CRPG_Object_Inventory` (`0x1075a16c`, referenced
   once from the class-registry `.data` at `0x1082d9ec`).
3. Each of the three names has **exactly one** code cross-reference, and all three
   are in the same code neighbourhood (`0x10265a92` .. `0x10265b9d`):
   - `FUN_10265ab0` looks up `ATTACHROTTRACK` and `ATTACHPOINT` **by name** (call to
     `0x10283bba`) and, when found, reads a **bool** value (vtable slot `0x1b8`)
     into its own bytes `+0x362` / `+0x35a` — the item's attach configuration;
   - `FUN_10265a70` pushes the `VIEWMODEL` string into a name-keyed call
     (`0x100162e0`). It is a leaf `ret 0Ch` with no `test eax,eax`, so whether it
     reads or *sets* the property is not resolved without a decompile.
4. `FirstPerson` / `ThirdPerson` are adjacent entries in a `.data` view-mode name
   table (VA `0x1083ff68`/`0x1083ff6c` → `0x1078be28`/`0x1078be1c`), near
   `CWObject_InputEntity` / `CWObject_Matinee`.

### Consequence for GH #67 step 1

The engine's first-person viewmodel is **not a GL-state-defined pass**. It is an
**item property** (`VIEWMODEL`) plus an **attach configuration**
(`ATTACHPOINT` / `ATTACHROTTRACK`) on `CRPG_Object_Inventory`. That is consistent
with every prior measurement: the object/view transform is applied engine-side to
the submitted vertices (ADR-0481/0482), so the arms reach GL as an ordinary
`glDrawElements` of an already-transformed, item-owned model — indistinguishable
from world geometry by any GL signature the hook can see.

Therefore the next step is **not another geometry heuristic**. It is either

- find the **consumer** of `VIEWMODEL` (the code that instantiates the item's
  viewmodel model in the model pipeline — MXR `CXR_ModelInstance`), or
- drive the engine's own first-person camera/attach state (GH #67 step 2/3).

## Consequences

- The geometric branch of GH #67 step 1 is now closed by measurement *and* by the
  engine's own data model: no GL-threshold correction can find the arms, because
  the arms are an item model.
- The 2009 module (9.0 MB) **exceeds** the provider's single-request analysis
  budget: `rea function ... --procedure 0x10265a92` returned
  `provider_timeout` (`analyze_function`, `timeout_ms 330000`, "Ghidra startup
  deadline elapsed") and `rea decompile ... 0x10265a92` returned
  `execution_failure` (`procedure_pseudo_code`) — two attempts, both killed by the
  same budget. The deep-REA corpus is the 2004 build
  (2.2 MB); `RndrGL.dll` (939 KB) decompiles fine. The 2009 code path therefore
  needs a persistent analysis snapshot across wakes, or a smaller module — this is
  a measured infrastructure limit, not a broken installation (`rea doctor`
  healthy).
- No behaviour changed: observation/REA only, no hook code, no default, no deploy,
  no install write.

## Validation

- The REA artifact `docs/rea/reference/GameClasses2009_ViewmodelProperties.txt`
  carries the subject hash, the section map, the full 32-entry flag table with VAs,
  the disassembly of the two functions, the reproducer script, the timeout record,
  and an explicit "not claimed" section.
- `tools/verify_reported_fixes.py` gains a GH #67 invariant pinning that artifact:
  it must exist and must record the module hash, the `noviewmodel` bit table, the
  three property-name VAs, the single code xrefs (`0x10265a92`, `0x10265b7a`,
  `0x10265b9d`), and the provider timeout — so the finding cannot silently rot.
- No live run was needed or used for this ADR (the claim is a shipped-artifact
  fact, read-only from the install).
