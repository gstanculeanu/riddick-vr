# ADR-0493: The 2009 `GameClasses` module *does* decompile — the 330 s budget is a configurable deadline, `--snapshot` is not a cache, and `0x100162e0` is `CStr::CStr` (GH #67 step 1)

- Status: Accepted (scheduled autoloop wake 92, GH #67)
- Date: 2026-10-18
- Related: GH #67 (this), GH #66 (per-draw interception, approach 2), ADR-0492
  (the engine-side item-property identification this corrects), ADR-0435/0438
  (`noviewmodel`), ADR-0481/0482 (the transform is engine-side), ADR-0491

## Context

ADR-0492 closed the *geometric* branch of GH #67 step 1 and identified the
engine-side mechanism (the viewmodel is an item property `VIEWMODEL` +
`ATTACHPOINT`/`ATTACHROTTRACK` on `CRPG_Object_Inventory`), but its open questions
were blocked on one recorded limit:

> The 9.0 MB module **exceeds** the provider's single-request analysis budget:
> `rea function … --procedure 0x10265a92` → `provider_timeout`
> (`analyze_function`, `timeout_ms 330000`, "Ghidra startup deadline elapsed") …
> the 2009 code path therefore needs a persistent analysis snapshot across wakes,
> or a smaller module.

That limit was carried as a hard constraint into ADR-0492's Next. This ADR tests
it directly (autoloop wake 92) and corrects two of its parts.

## Decision

Record the measured REA behaviour on the 2009 module and correct ADR-0492
accordingly. No game code, no hook code, no default, no deploy — REA only.

### Measured (read-only; `docs/rea/reference/GameClasses2009_ViewmodelDecompiled.txt`)

Subject `Content\GameClasses_Win32_x86.dll`, 9,016,560 B,
`sha256 d7e81d7d3726…`, provider Ghidra 12.1.4, `confidence = observed`,
`authority = shipped-artifact`.

1. **The deadline is configurable, and the module analyses.** With
   `REA_GHIDRA_STARTUP_TIMEOUT_MS = 1500000` (the default is `330_000`), two
   `rea decompile` requests both returned **exit 0** (382 s and 380 s wall).
   *"The 2009 module does not fit the provider's analysis budget"* (ADR-0492 §5)
   is corrected to: *the **default** 330 s deadline is too short; the module
   decompiles with a raised deadline at ≈6.4 min per procedure.*
2. **`--snapshot` is not a cross-call analysis cache for the CLI Ghidra
   provider.** The snapshot path was passed on both calls; the second call still
   took 380 s (a fresh import + full auto-analysis). The provider's pinned
   analysis profile (`import_mode = "ephemeral-source-immutable"`) and its own
   stated limitations agree: the imported Program/temp project are ephemeral and
   "the next provider query imports the original artifact again".
   *The "persist a Ghidra analysis snapshot across wakes" route (ADR-0492 §Next 2)
   is therefore measured **not** to reduce CLI cost.*
3. **`FUN_10265a70` decompiled** (the sole `VIEWMODEL` xref, VA `0x10265a70`):
   `{ FUN_10260940(); CStr::CStr("VIEWMODEL"); FUN_1026d790(); }`. The raw-scan
   "`call 0x100162e0` = name-keyed helper" reading is corrected: that call is a
   **temporary `CStr` construction** from the literal (`sub esp,8; mov ecx,esp;
   push "VIEWMODEL"`), and the literal is passed *by name* to `FUN_1026d790`.
4. **`FUN_100162e0` decompiled** = `CStr::CStr(const char*)`: it installs
   `CStr::vftable` in `[this]`, zeroes `[this+4]`, computes the length inline and
   calls vtable slot `0x68`. It is the engine's **string class constructor**, not a
   property lookup.

### Consequence for GH #67 step 1

The engine-side route is now *practically* open: the 2009 `GameClasses` (and by
the same method MXR/`MSystem`) can be decompiled one procedure per ≈6.4 min
request by raising `REA_GHIDRA_STARTUP_TIMEOUT_MS`. The remaining question at the
`VIEWMODEL` site is *not* resolved by this ADR (its callees `FUN_10260940`/
`FUN_1026d790` and its callers are not decompiled), so read-vs-set is still open —
but the blocker is now understood as a *budget* to spend, not a wall.

## Consequences

- ADR-0492 §3b is corrected in place (the "name-keyed helper" at `0x100162e0` is
  the `CStr` constructor) and §5 is corrected in place (the 330 s limit is the
  *default deadline*, not the module's limit).
- A per-procedure cost of ≈6.4 min is now a known, bounded unit of work — one
  decompile per wake is a reasonable autoloop budget.
- The `--snapshot` flag is documented as **not** reducing CLI Ghidra cost, so no
  future wake should spend a unit on "warm the snapshot to make later calls fast".
  True cross-wake reuse would need a persistent `analyzeHeadless` project + a
  postScript (not implemented; a tooling change, not an evidence claim).
- Observation/REA only: no hook code, no default changed, no deploy, no install
  write, no live run.

## Validation

- `docs/rea/reference/GameClasses2009_ViewmodelDecompiled.txt` carries the subject
  hash, the exact commands, the wall clocks, both decompilations verbatim, the
  snapshot measurement, and an explicit "not claimed" section.
- ADR-0492 and `GameClasses2009_ViewmodelProperties.txt` get CORRECTED banners
  pointing at this ADR (the pinning invariant is not weakened: the old artifact
  must keep recording the raw-scan facts and the timeout *record*).
- `tools/verify_reported_fixes.py` gains a GH #67 invariant pinning the new
  artifact (module hash, `FUN_10265a70`, `CStr::vftable`, `REA_GHIDRA_STARTUP_TIMEOUT_MS`,
  `1500000`, `ephemeral-source-immutable`) and this ADR.
- No C++ changed → no `build-all`/CTest; the offline guards (`verify_reported_fixes`,
  `scenario_tests.py --check`, `verify_hook_init`, `validate_addresses`,
  `verify_test_artifacts`, `xrsim --selftest`, `tools/test_*.py`) are the validation.
