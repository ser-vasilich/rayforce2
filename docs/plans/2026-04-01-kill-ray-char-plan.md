# Kill RAY_CHAR Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove RAY_CHAR type, unify all strings under RAY_STR, renumber type constants to close gaps.

**Architecture:** Three phases — (1) renumber type constants and remove RAY_CHAR from headers/types, (2) migrate all code that creates or matches RAY_CHAR, (3) update tests and formatting.

**Tech Stack:** Pure C17, munit test framework.

---

### Task 1: Renumber type constants and remove RAY_CHAR

**Files:**
- Modify: `include/rayforce.h`
- Modify: `src/ops/ops.h` (RAY_SEL)
- Modify: `src/core/types.c` (ray_type_sizes)
- Modify: `src/core/types.h` (RAY_TYPE_COUNT)

**Step 1: Update type constants in rayforce.h**

Replace the type constants block (lines 58-75) with:

```c
#define RAY_LIST       0
#define RAY_BOOL       1
#define RAY_U8         2
#define RAY_I16        3
#define RAY_I32        4
#define RAY_I64        5
#define RAY_F32        6
#define RAY_F64        7
#define RAY_DATE       8
#define RAY_TIME       9
#define RAY_TIMESTAMP 10
#define RAY_GUID      11
#define RAY_SYM       12   /* Dictionary-encoded string column (adaptive width) */
#define RAY_STR       13   /* Variable-length string column (inline + pool) */
```

Remove `RAY_CHAR` entirely. Remove the `c8` field from the ray_t union (line ~133: `char c8;`). Also remove the `ray_char()` declaration (line ~184).

**Step 2: Update RAY_SEL in ops.h**

Change `#define RAY_SEL 15` to `#define RAY_SEL 14`.

**Step 3: Update ray_type_sizes in types.c**

```c
const uint8_t ray_type_sizes[RAY_TYPE_COUNT] = {
    /* [RAY_LIST]      =  0 */ 8,
    /* [RAY_BOOL]      =  1 */ 1,
    /* [RAY_U8]        =  2 */ 1,
    /* [RAY_I16]       =  3 */ 2,
    /* [RAY_I32]       =  4 */ 4,
    /* [RAY_I64]       =  5 */ 8,
    /* [RAY_F32]       =  6 */ 4,
    /* [RAY_F64]       =  7 */ 8,
    /* [RAY_DATE]      =  8 */ 4,
    /* [RAY_TIME]      =  9 */ 4,
    /* [RAY_TIMESTAMP] = 10 */ 8,
    /* [RAY_GUID]      = 11 */ 16,
    /* [RAY_SYM]       = 12 */ 8,
    /* [RAY_STR]       = 13 */ 16,
    /* [RAY_SEL]       = 14 */ 0,
};
```

**Step 4: Update RAY_TYPE_COUNT in types.h**

Change `#define RAY_TYPE_COUNT 16` to `#define RAY_TYPE_COUNT 15`.

**Step 5: Build**

Run: `make clean && make 2>&1 | head -50`
Expected: compilation errors from code still referencing RAY_CHAR, ray_char, c8 field. That's expected — they get fixed in later tasks.

**Step 6: Commit**

```bash
git add include/rayforce.h src/ops/ops.h src/core/types.c src/core/types.h
git commit -m "refactor: renumber type constants, remove RAY_CHAR"
```

---

### Task 2: Remove ray_char() and fix ray_str() long path

**Files:**
- Modify: `src/vec/atom.c`

**Step 1: Delete ray_char() function**

Remove the `ray_char()` function (lines 50-56):
```c
// DELETE THIS:
ray_t* ray_char(char val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_CHAR;
    v->c8 = val;
    return v;
}
```

**Step 2: Fix ray_str() long string path**

In `ray_str()` (line ~110), change the long string backing type from RAY_CHAR to RAY_U8:

```c
    /* Old: chars->type = RAY_CHAR; */
    chars->type = RAY_U8;
```

**Step 3: Commit**

```bash
git add src/vec/atom.c
git commit -m "refactor: delete ray_char(), fix ray_str() backing to RAY_U8"
```

---

### Task 3: Migrate parse.c — char literals become string atoms

**Files:**
- Modify: `src/lang/parse.c`

**Step 1: Update char literal parsing**

In `parse_symbol()` (around line 402-454), change two `ray_char(ch)` calls to `ray_str(&ch, 1)`:

Line ~446 (escaped char literal):
```c
/* Old: return ray_char(ch); */
return ray_str(&ch, 1);
```

Line ~453 (simple char literal):
```c
/* Old: return ray_char(ch); */
return ray_str(&ch, 1);
```

**Step 2: Commit**

```bash
git add src/lang/parse.c
git commit -m "refactor: char literals parse as ray_str atoms"
```

---

### Task 4: Migrate eval.c — replace all RAY_CHAR references

This is the largest task (~16 `-RAY_CHAR` checks, ~16 `->c8` field accesses).

**Files:**
- Modify: `src/lang/eval.c`

**Step 1: Mechanical replacement of -RAY_CHAR checks**

Every `x->type == -RAY_CHAR` becomes `x->type == -RAY_STR`. But since -RAY_STR checks already exist, you need to merge the char logic into the existing string logic.

Key patterns to find and fix:

**a) Numeric coercion (char → double):**
```c
/* Old: if (x->type == -RAY_CHAR) return (double)(unsigned char)x->c8; */
/* This was "treat char as its byte value". For strings, coerce first byte: */
/* Remove this case entirely — string-to-number coercion is already handled elsewhere
   or this becomes: parse the string as a number */
```

**b) first/last returning char atom:**
```c
/* Old: return ray_char(ptr[0]); */
return ray_str(&ptr[0], 1);  /* or */ return ray_str(ptr, 1);

/* Old: return ray_char(ptr[len-1]); */
{ char c = ptr[len-1]; return ray_str(&c, 1); }
```

**c) c8 field access — all `->c8` references:**
These read a single byte from a char atom. Since char atoms are now string atoms, use `ray_str_ptr(x)[0]` or the SSO field `x->sdata[0]` directly (since single chars are always SSO).

```c
/* Old: x->c8 */
/* New: x->sdata[0]  (for -RAY_STR atoms, guaranteed SSO for 1-byte strings) */
```

**d) String-char comparisons:**
Remove special char-vs-string comparison branches — both sides are now -RAY_STR, so string comparison handles it uniformly.

**e) Type inference / vector creation from char atoms:**
Remove `case -RAY_CHAR:` branches that create RAY_CHAR vectors. Single-char string atoms going into a vector should create RAY_STR vectors.

**Step 2: Search and fix**

Run: `grep -n 'RAY_CHAR\|->c8[^a-z0-9_]' src/lang/eval.c` and fix each site.

The key principle: everywhere that handled `-RAY_CHAR` as a special case either:
1. Gets merged into the existing `-RAY_STR` handling (most cases)
2. Uses `x->sdata[0]` instead of `x->c8` for single-byte access
3. Gets deleted because string-string comparison already covers it

**Step 3: Build check**

`make clean && make 2>&1 | grep error` — eval.c should compile clean.

**Step 4: Commit**

```bash
git add src/lang/eval.c
git commit -m "refactor: replace all RAY_CHAR references in eval.c with RAY_STR"
```

---

### Task 5: Migrate format.c — remove char formatting

**Files:**
- Modify: `src/app/format.c`

**Step 1: Remove RAY_CHAR from ray_type_name()**

Delete the `case RAY_CHAR: return "c8";` line.

**Step 2: Remove fmt_char() function**

Delete the `fmt_char()` function (lines ~174-186) and its call sites. Single-char strings now format the same as any string: `"a"`.

**Step 3: Remove RAY_CHAR vector formatting case**

In vector element formatting, remove the `case RAY_CHAR:` that called `fmt_char()`.

**Step 4: Remove RAY_CHAR atom formatting case**

In atom formatting, remove `case RAY_CHAR:` that called `fmt_char()`. Atom strings are already handled by the `-RAY_STR` case.

**Step 5: Build and commit**

```bash
make clean && make
git add src/app/format.c
git commit -m "refactor: remove RAY_CHAR from formatting, char displays as string"
```

---

### Task 6: Migrate remaining source files

**Files:**
- Any remaining files with RAY_CHAR references

**Step 1: Find remaining references**

```bash
grep -rn 'RAY_CHAR\|ray_char' src/ include/ --include='*.c' --include='*.h'
```

Expected files: possibly `exec.c`, `csv.c`, `vec.c`, `str.c`. Fix each:
- Remove `RAY_CHAR` from switch/case type dispatches
- Remove any RAY_CHAR vector creation code
- Fix any `ray_char()` calls to `ray_str(&ch, 1)`

**Step 2: Build**

```bash
make clean && make
```

**Step 3: Commit**

```bash
git add -A
git commit -m "refactor: remove all remaining RAY_CHAR references from source"
```

---

### Task 7: Update tests

**Files:**
- Modify: `test/test_atom.c`
- Modify: `test/test_types.c`
- Modify: `test/test_lang.c`
- Modify: `test/test_format.c`
- Any other test files with RAY_CHAR

**Step 1: Fix test_atom.c**

Replace `test_atom_char` with a test that verifies `ray_str("a", 1)` creates a proper SSO string atom:

```c
static MunitResult test_atom_single_char_str(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* v = ray_str("Z", 1);
    munit_assert_int(v->type, ==, -RAY_STR);
    munit_assert_int(v->slen, ==, 1);
    munit_assert_int(v->sdata[0], ==, 'Z');
    const char* p = ray_str_ptr(v);
    munit_assert_int(p[0], ==, 'Z');
    munit_assert_int(ray_str_len(v), ==, 1);
    ray_release(v);
    return MUNIT_OK;
}
```

Update the test suite array entry.

**Step 2: Fix test_types.c**

Update `test_type_sizes_known_types` — remove RAY_CHAR assertion, update any size checks that changed. Verify new layout:
```c
munit_assert_uint(ray_type_sizes[RAY_I16], ==, 2);
munit_assert_uint(ray_type_sizes[RAY_F32], ==, 4);
munit_assert_uint(ray_type_sizes[RAY_F64], ==, 8);
munit_assert_uint(ray_type_sizes[RAY_STR], ==, 16);
```

**Step 3: Fix test_lang.c**

Find any tests that check for char atoms (type == -RAY_CHAR) and update to check for -RAY_STR. Look for:
- `ASSERT_EQ("'a'", ...)` — char literal tests
- Any `-RAY_CHAR` type assertions
- Tests using `ray_char()`

**Step 4: Fix test_format.c**

Update any tests that expect `c8` type name or char formatting like `'a'`.

**Step 5: Find and fix remaining test files**

```bash
grep -rn 'RAY_CHAR\|ray_char\|->c8' test/ --include='*.c'
```

**Step 6: Build and run all tests**

```bash
make clean && make && make test
```

All 563+ tests should pass.

**Step 7: Commit**

```bash
git add test/
git commit -m "test: update tests for RAY_CHAR removal"
```

---

### Task 8: Final verification

**Step 1: Grep for any remaining references**

```bash
grep -rn 'RAY_CHAR\|ray_char\|->c8[^a-z]' src/ test/ examples/ include/ --include='*.c' --include='*.h'
```

Should return nothing (or only comments).

**Step 2: Full test run**

```bash
make clean && make && make test
```

**Step 3: REPL smoke test**

```bash
echo "'a'" | ./rayforce         # should show "a" (string, not char)
echo "(first \"hello\")" | ./rayforce  # should show "h"
echo "(+ 1 \"s\")" | ./rayforce       # should show type error
echo "(== 'a' \"a\")" | ./rayforce    # should show true
```

**Step 4: Commit any remaining fixes**

```bash
git add -A
git commit -m "chore: final cleanup for RAY_CHAR removal"
```
