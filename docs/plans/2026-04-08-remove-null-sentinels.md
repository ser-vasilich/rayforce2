# Remove Null Sentinels Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove all null sentinel logic (0Nl, 0Ni, 0Nd, etc.) from the codebase. Rayforce2 uses null bitmaps on vectors — sentinel values are a legacy from Rayforce1 that confuse the code and cause bugs (like the recent `abs` overflow).

**Architecture:** Null in RF2 has three forms:
1. `RAY_NULL_OBJ` — the singleton void null (type=RAY_NULL). Returned by void builtins (println, show). Tested with `RAY_IS_NULL(p)`.
2. **Typed null atoms** — e.g., `0Ni` is an I32 atom with `nullmap[0] bit 0` set. The value field is undefined. The type is preserved. Uses the same null bitmap machinery as vectors — just the first bit.
3. **Null bitmap on vectors** — per-element null flags in the 16-byte inline nullmap (or ext_nullmap for large vectors). The executor skips null elements via the bitmap.

Sentinel values (`INT64_MIN` for 0Nl, `INT32_MIN` for 0Ni, `NaN` for 0Nf, etc.) are replaced by the null bit. The value field is no longer meaningful for null atoms — only the type and null bit matter.

---

## Task 1: Remove `is_null_atom` and all callers — DONE

- [x] Deleted `is_null_atom` from `eval_internal.h`
- [x] Added `RAY_ATOM_IS_NULL` macro and `ray_typed_null` constructor in `rayforce.h` and `atom.c`
- [x] Replaced all `is_null_atom` calls with `RAY_ATOM_IS_NULL` across arith.c, cmp.c, agg.c, collection.c, eval.c, query.c, builtins.c
- [x] Updated `null_for_promoted` to use `ray_typed_null` instead of sentinel values
- [x] Updated `store_typed_elem` to propagate null bits to vectors
- [x] Fixed slab fast path in heap.c to zero nullmap on reuse
- [x] Updated all sentinel null returns (INT64_MIN, INT32_MIN, NaN) in agg.c, collection.c, builtins.c, query.c
- [x] Updated atomic mapper fast path to use null bitmap instead of sentinel checks

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

**Replace with null-bit check:**
```c
if (x->nullmap[0] & 1) { ray_retain(x); return x; }  // typed null atom
```

This preserves null propagation through scalar arithmetic (e.g., `(+ 1 0Ni)` → null I32) while using the bitmap instead of sentinel values. The atomic mapper (`RAY_FN_ATOMIC`) handles vector-level nulls separately.

For `RAY_NULL_OBJ` checks (the void null), keep using `RAY_IS_NULL(x)`.

**Add helper macro** in eval_internal.h:
```c
#define RAY_ATOM_IS_NULL(x) ((x)->nullmap[0] & 1)
```

**Step-by-step:**
1. Delete `is_null_atom` from `eval_internal.h`
2. Build — get all compilation errors (58 sites)
3. For each error:
   - If the check propagates a null sentinel through arithmetic → remove the check
   - If the check guards against `RAY_NULL_OBJ` → replace with `RAY_IS_NULL(x)`
   - If the check is in aggregation (sum/count/avg etc.) where nulls affect the result → the null bitmap is checked in the morsel loop, remove the per-element check
4. Build and test

## Task 2: Remove sentinel parsing from parser — DONE

- [x] All typed null literals (0Nl, 0Ni, etc.) now use `ray_typed_null` with null bit
- [x] Empty symbol backtick uses `ray_typed_null(-RAY_SYM)`
- [x] Vector literal construction propagates null bits from atoms to vector bitmap

**Files:**
- `src/lang/parse.c` (lines 220-226, 411, 460)

The parser creates sentinel atoms for `0Nl`, `0Ni`, `0Nd`, `0Nt`, `0Np`, `0Nf` syntax. These should either:
- Return `RAY_NULL_OBJ` (the universal null)
- Or be removed entirely (syntax no longer recognized)

**Decision needed:** Should `0Nl` syntax still be recognized? If yes, it should return `RAY_NULL_OBJ`. If no, remove the parsing rules. The kdb+ convention is to keep the syntax but have a single null representation.

**Approach:** Keep all syntax. Each typed null literal creates an atom of the correct type with `nullmap[0] bit 0` set. The value field is zeroed (not a sentinel).

**Step-by-step:**
1. Replace each sentinel case in parse.c:
   - `0Nl` → `ray_i64(0)` with `nullmap[0] |= 1`
   - `0Ni` → `ray_i32(0)` with `nullmap[0] |= 1`
   - `0Nd` → `ray_date(0)` with `nullmap[0] |= 1`
   - `0Nt` → `ray_time(0)` with `nullmap[0] |= 1`
   - `0Np` → `ray_timestamp(0)` with `nullmap[0] |= 1`
   - `0Nf` → `ray_f64(0)` with `nullmap[0] |= 1` (NOT NaN — use the null bit)
2. Add a helper: `ray_t* ray_typed_null(int8_t type)` that creates a zeroed atom with null bit set
3. Empty symbol `\`` → typed null SYM atom (null bit set)

## Task 3: Remove sentinel display from format — DONE

- [x] Atom formatter checks `RAY_ATOM_IS_NULL` to display typed nulls (0Ni, 0Nl, etc.)
- [x] Removed sentinel value checks (INT32_MIN, INT64_MIN, NaN) from low-level formatters
- [x] Vector element formatter already uses `ray_vec_is_null` (unchanged)

**Files:**
- `src/lang/format.c` (lines 182, 187, 386, 389)

Display checks the null bit instead of sentinel values.

**Step-by-step:**
1. Replace `INT32_MIN → 0Ni` check with `RAY_ATOM_IS_NULL(x) → 0Ni` (check the bit, not the value)
2. Same for I64 → `0Nl`, F64 → `0Nf`, etc.
3. `INT64_MIN` and `INT32_MIN` now display as normal numbers (they're no longer special)

## Task 4: Remove sentinel constants from CLAUDE.md — DONE

- [x] Updated Null section to describe three null forms: RAY_NULL_OBJ, typed null atoms (null bit), null bitmaps
- [x] Removed mentions of `is_null_atom` and sentinel values
- [x] Documented `RAY_ATOM_IS_NULL` macro and `ray_typed_null` constructor
- [x] Updated eval_internal.h file description to reference `RAY_ATOM_IS_NULL` instead of `is_null_atom`

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
