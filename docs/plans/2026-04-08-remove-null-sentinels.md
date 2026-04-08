# Remove Null Sentinels Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove all null sentinel logic (0Nl, 0Ni, 0Nd, etc.) from the codebase. Rayforce2 uses null bitmaps on vectors — sentinel values are a legacy from Rayforce1 that confuse the code and cause bugs (like the recent `abs` overflow).

**Architecture:** Null in RF2 has two forms only:
1. `RAY_NULL_OBJ` — the singleton null object (type=RAY_NULL). Returned by void builtins (println, show). Tested with `RAY_IS_NULL(p)`.
2. Null bitmap on vectors — per-element null flags. The executor skips null elements via the bitmap, not by checking values.

Sentinel values (`INT64_MIN` for 0Nl, `INT32_MIN` for 0Ni, `NaN` for 0Nf, etc.) served as in-band null markers in RF1 where vectors didn't have null bitmaps. In RF2 they're redundant and harmful.

---

## Task 1: Remove `is_null_atom` and all callers

**Files:**
- `src/lang/eval_internal.h` — delete `is_null_atom` function (line 115-129)
- `src/ops/arith.c` — 25 call sites
- `src/ops/cmp.c` — 18 call sites
- `src/ops/agg.c` — 5 call sites
- `src/ops/collection.c` — 3 call sites
- `src/lang/eval.c` — 3 call sites
- `src/ops/query.c` — 2 call sites
- `src/ops/builtins.c` — 1 call site

**Strategy per call site:**

Most `is_null_atom(x)` checks in arithmetic/comparison builtins follow this pattern:
```c
if (is_null_atom(x)) { ray_retain(x); return x; }  // propagate null
```

These exist because RF1 vectors didn't have null bitmaps — builtins had to check each element for sentinel values. In RF2, the null bitmap + executor handle this. The atomic mapper (`RAY_FN_ATOMIC`) skips null elements before calling the builtin. **Remove these checks entirely.**

For `RAY_NULL_OBJ` checks (the singleton, not sentinels), replace with `RAY_IS_NULL(x)`.

**Step-by-step:**
1. Delete `is_null_atom` from `eval_internal.h`
2. Build — get all compilation errors (58 sites)
3. For each error:
   - If the check propagates a null sentinel through arithmetic → remove the check
   - If the check guards against `RAY_NULL_OBJ` → replace with `RAY_IS_NULL(x)`
   - If the check is in aggregation (sum/count/avg etc.) where nulls affect the result → the null bitmap is checked in the morsel loop, remove the per-element check
4. Build and test

## Task 2: Remove sentinel parsing from parser

**Files:**
- `src/lang/parse.c` (lines 220-226, 411, 460)

The parser creates sentinel atoms for `0Nl`, `0Ni`, `0Nd`, `0Nt`, `0Np`, `0Nf` syntax. These should either:
- Return `RAY_NULL_OBJ` (the universal null)
- Or be removed entirely (syntax no longer recognized)

**Decision needed:** Should `0Nl` syntax still be recognized? If yes, it should return `RAY_NULL_OBJ`. If no, remove the parsing rules. The kdb+ convention is to keep the syntax but have a single null representation.

**Recommended:** Keep the syntax, all sentinel literals parse to `RAY_NULL_OBJ`. The type-specific null syntax (`0Ni` vs `0Nl`) becomes redundant but harmless.

**Step-by-step:**
1. Replace all sentinel literal cases in parse.c with `return RAY_NULL_OBJ; ray_retain(RAY_NULL_OBJ);`
2. Remove empty symbol sentinel (line 460): empty symbol `\`` should be an error, not null

## Task 3: Remove sentinel display from format

**Files:**
- `src/lang/format.c` (lines 182, 187, 386, 389)

Currently, `INT32_MIN` displays as `0Ni` and `INT64_MIN` as `0Nl`. These are valid integer values — they should display as numbers, not null markers.

**Step-by-step:**
1. Remove the `INT32_MIN → 0Ni` and `INT64_MIN → 0Nl` special cases
2. Remove the `case RAY_I32: fmt_puts(b, "0Ni")` and `case RAY_I64: fmt_puts(b, "0Nl")` in the null format section

## Task 4: Remove sentinel constants from CLAUDE.md

**Files:**
- `CLAUDE.md` — update the Null section to remove sentinel references

**Step-by-step:**
1. Update the Null documentation to describe only `RAY_NULL_OBJ` and null bitmaps
2. Remove mentions of `is_null_atom`, `0Nl/0Ni/0Nd/0Nt/0Np/0Nf`, "sentinel nulls propagate through arithmetic"

## Task 5: Update documentation

**Files:**
- `website/docs/data-types.html` — if it documents null sentinels, update

## Task 6: Tests

**Step-by-step:**
1. Run full test suite — fix any failures from removed sentinel logic
2. Verify: `(+ 1 null)` returns null (RAY_NULL_OBJ) or error, not a sentinel
3. Verify: `(sum [1 2 3])` with null bitmap element produces correct result
4. Verify: `0Nl` syntax parses to null
5. Verify: `INT64_MIN` prints as the number, not `0Nl`

---

## Summary of changes

| What | Where | Action |
|------|-------|--------|
| `is_null_atom` function | eval_internal.h | Delete |
| 58 call sites | arith.c, cmp.c, agg.c, collection.c, eval.c, query.c, builtins.c | Remove or replace with RAY_IS_NULL |
| Sentinel parsing (0Nl etc.) | parse.c | Return RAY_NULL_OBJ |
| Sentinel display | format.c | Remove INT32_MIN/INT64_MIN special cases |
| Documentation | CLAUDE.md, website | Update |
