# Rayforce → Rayforce Rename — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rename the entire project from Rayforce to Rayforce — all prefixes, file paths, build targets, docs, and user-facing strings.

**Architecture:** Purely mechanical text substitution and file moves. No logic changes. One atomic commit. The `ray_` prefix already exists on static Rayfall builtin functions in `eval.c` — these are internal and won't collide with the renamed public API.

**Tech Stack:** C17, CMake, sed for bulk replacement.

**Design doc:** `docs/plans/2026-03-28-rayforce-to-rayforce-rename-design.md`

**Build & test commands:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure
```

**IMPORTANT — sed ordering:** The replacements MUST be applied in this order to avoid double-renaming:
1. `RAYFORCE` → `RAYFORCE` (uppercase constants/macros first)
2. `Rayforce` → `Rayforce` (capitalized references)
3. `rayforce` → `rayforce` (lowercase references)
4. `RAY_` → `RAY_` (public prefix — constants, macros, type tags)
5. `ray_` → `ray_` (public prefix — functions, types)
6. `rayforce.h` → `rayforce.h` (header filename in includes)

**CRITICAL exclusions — do NOT rename:**
- `stdout`, `stdin`, `stderr`, `stdint.h`, `stdbool.h`, `stdlib.h` etc. (prefix is `std`, not `td`)
- `STDIN_FD`, `STDIN_FILENO`, `STDOUT_FILENO` (these are system defines)
- `munit.c` / `munit.h` (third-party test framework, untouched)
- Files under `build/` directory
- The `.git/` directory

---

## Task 1: Move header file and update include directory

**Files:**
- Move: `include/rayforce/rayforce.h` → `include/rayforce.h`
- Remove: `include/rayforce/` (empty directory after move)
- Modify: `CMakeLists.txt`

- [ ] Move the public header:
  ```bash
  mv include/rayforce/rayforce.h include/rayforce.h
  rmdir include/rayforce
  ```
- [ ] Build to confirm it breaks (expected — includes not updated yet):
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build 2>&1 | head -5
  ```
  Expected: compilation errors about missing `rayforce/rayforce.h`
- [ ] **Do NOT commit yet** — continue to Task 2

---

## Task 2: Bulk rename prefixes in all source files

**Files:** All `.c` and `.h` files under `src/`, `test/`, `bench/`, `examples/`, `fuzz/`, and `include/rayforce.h`

**IMPORTANT:** Use `sed` with word boundaries to avoid renaming `stdout`, `stdin`, etc. Apply in the exact order below. Each sed command operates on ALL matching files at once.

- [ ] Rename uppercase `RAYFORCE` → `RAYFORCE` in all C/H files:
  ```bash
  find src test bench examples fuzz include -name '*.c' -o -name '*.h' | \
    xargs sed -i 's/RAYFORCE/RAYFORCE/g'
  ```
- [ ] Rename capitalized `Rayforce` → `Rayforce` in all C/H files:
  ```bash
  find src test bench examples fuzz include -name '*.c' -o -name '*.h' | \
    xargs sed -i 's/Rayforce/Rayforce/g'
  ```
- [ ] Rename lowercase `rayforce` → `rayforce` in all C/H files:
  ```bash
  find src test bench examples fuzz include -name '*.c' -o -name '*.h' | \
    xargs sed -i 's/rayforce/rayforce/g'
  ```
- [ ] Rename `RAY_` → `RAY_` prefix in all C/H files:
  ```bash
  find src test bench examples fuzz include -name '*.c' -o -name '*.h' | \
    xargs sed -i 's/RAY_/RAY_/g'
  ```
- [ ] Rename `ray_` → `ray_` prefix in all C/H files:
  ```bash
  find src test bench examples fuzz include -name '*.c' -o -name '*.h' | \
    xargs sed -i 's/ray_/ray_/g'
  ```
- [ ] Fix include paths — `<rayforce/rayforce.h>` and `"rayforce/rayforce.h"` → `<rayforce.h>` and `"rayforce.h"`:
  ```bash
  find src test bench examples fuzz include -name '*.c' -o -name '*.h' | \
    xargs sed -i 's|<rayforce/rayforce\.h>|<rayforce.h>|g; s|"rayforce/rayforce\.h"|"rayforce.h"|g'
  ```
  Note: the earlier `rayforce` → `rayforce` sed already changed the strings, so we fix the double-rename here.
- [ ] **Do NOT commit yet** — continue to Task 3

---

## Task 3: Fix collateral damage from sed

After bulk sed, some things will be incorrectly renamed. Fix them.

**Files:** Various `.c` and `.h` files

- [ ] Fix `stray_out` / `stray_in` / `stray_err` — these were `stdout`/`stdin`/`stderr` with `td` → `ray` applied to the `std` prefix. **Actually `std` does not start with `ray_` so this should NOT happen.** Verify:
  ```bash
  grep -rn 'stray_\|sray_' src/ test/ bench/ examples/ fuzz/ include/ || echo "No collateral — clean"
  ```
- [ ] Fix `STRAY_` false hits (from `STDIN`, `STDOUT`):
  ```bash
  grep -rn 'STRAY_\|SRAY_' src/ test/ bench/ examples/ fuzz/ include/ || echo "No collateral — clean"
  ```
- [ ] Fix `__RAY_` prefix if any internal double-underscore defines exist:
  ```bash
  grep -rn '__RAY_' src/ include/ | head -5
  ```
  These should be fine — `__RAY_` becomes `__RAY_` which is correct.
- [ ] Check for broken `#include` paths — any still referencing `rayforce`:
  ```bash
  grep -rn 'rayforce' src/ test/ bench/ examples/ fuzz/ include/ --include='*.c' --include='*.h'
  ```
  Expected: no matches. If any remain, fix them.
- [ ] Check for `ray_` still present (should all be renamed):
  ```bash
  grep -rn '\bray_' src/ test/ bench/ examples/ fuzz/ include/ --include='*.c' --include='*.h' | grep -v 'munit' | head -10
  ```
  Expected: no matches outside munit files.
- [ ] Check for `RAY_` still present:
  ```bash
  grep -rn '\bRAY_' src/ test/ bench/ examples/ fuzz/ include/ --include='*.c' --include='*.h' | grep -v 'munit' | head -10
  ```
  Expected: no matches outside munit files.
- [ ] **Do NOT commit yet** — continue to Task 4

---

## Task 4: Update CMakeLists.txt

**Files:** Modify `CMakeLists.txt`

- [ ] Apply the same renames to CMakeLists.txt:
  ```bash
  sed -i 's/RAYFORCE/RAYFORCE/g; s/rayforce_static/rayforce_static/g; s/rayforce_repl/rayforce/g; s/test_rayforce/test_rayforce/g' CMakeLists.txt
  ```
- [ ] Manually update remaining references:
  - `project(rayforce` → `project(rayforce`
  - Shared library target: `add_library(rayforce SHARED` → `add_library(rayforce_lib SHARED` and `set_target_properties(rayforce_lib PROPERTIES OUTPUT_NAME rayforce)`
  - Static library output name: stays `OUTPUT_NAME rayforce`
  - Binary `rayforce` links to `rayforce_static`
  - Include dir: `PUBLIC include` stays (header is now at `include/rayforce.h`)
  - Remove the old `rayforce/` subdirectory from include paths if referenced
  - Benchmark/fuzz targets: update `rayforce` references
  - `rayforce.pc` config reference → `rayforce.pc`
- [ ] Update compile definitions: `RAYFORCE_VERSION`, `RAYFORCE_GIT_COMMIT`, `RAYFORCE_BUILD_DATE` (already renamed by sed from `RAYFORCE_*`)
- [ ] **Do NOT commit yet** — continue to Task 5

---

## Task 5: Update pkg-config template

**Files:** Rename and modify `rayforce.pc.in` → `rayforce.pc.in`

- [ ] Rename the file:
  ```bash
  mv rayforce.pc.in rayforce.pc.in
  ```
- [ ] Update contents:
  ```
  Name: rayforce
  Description: Pure C17 columnar vector engine with native graph processing
  Libs: -L${libdir} -lrayforce
  ```
- [ ] Update `CMakeLists.txt` reference from `rayforce.pc` to `rayforce.pc` (if configure_file is used)
- [ ] **Do NOT commit yet** — continue to Task 6

---

## Task 6: Update user-facing strings in REPL

**Files:** Modify `src/app/repl.c`, `src/app/term.c`

- [ ] These should already be renamed by Task 2 sed. Verify:
  ```bash
  grep -n 'rayforce\|Rayforce\|RAYFORCE' src/app/repl.c src/app/term.c
  ```
  Expected: no matches.
- [ ] Verify REPL banner says "Rayforce":
  ```bash
  grep -n 'Rayforce\|RAYFORCE_VERSION' src/app/repl.c | head -5
  ```
- [ ] Verify history file path is `~/.rayforce_history`:
  ```bash
  grep -n 'history' src/app/term.c | head -5
  ```
- [ ] **Do NOT commit yet** — continue to Task 7

---

## Task 7: Update CLAUDE.md

**Files:** Modify `CLAUDE.md`

- [ ] Replace all `rayforce` → `rayforce`, `Rayforce` → `Rayforce`, `ray_` → `ray_`, `RAY_` → `RAY_`, `rayforce.h` → `rayforce.h` in CLAUDE.md:
  ```bash
  sed -i 's/RAYFORCE/RAYFORCE/g; s/Rayforce/Rayforce/g; s/rayforce/rayforce/g; s/RAY_/RAY_/g; s/ray_/ray_/g; s/td\.h/rayforce.h/g' CLAUDE.md
  ```
- [ ] Verify build commands in CLAUDE.md are updated (binary name `rayforce` instead of `rayforce_repl`)
- [ ] **Do NOT commit yet** — continue to Task 8

---

## Task 8: Update README.md and docs

**Files:** Modify `README.md`, docs under `docs/plans/`

- [ ] Rename in README.md:
  ```bash
  sed -i 's/RAYFORCE/RAYFORCE/g; s/Rayforce/Rayforce/g; s/rayforce/rayforce/g; s/RAY_/RAY_/g; s/ray_/ray_/g; s/td\.h/rayforce.h/g' README.md
  ```
- [ ] Rename in active plan/design docs (not completed ones — those are historical):
  ```bash
  for f in docs/plans/2026-03-28-*.md docs/plans/2026-03-27-repl-design.md; do
    sed -i 's/RAYFORCE/RAYFORCE/g; s/Rayforce/Rayforce/g; s/rayforce/rayforce/g; s/RAY_/RAY_/g; s/ray_/ray_/g; s/td\.h/rayforce.h/g' "$f"
  done
  ```
- [ ] **Do NOT commit yet** — continue to Task 9

---

## Task 9: Rename benchmark file

**Files:** Rename `bench/bench_rayforce.c` → `bench/bench_rayforce.c`

- [ ] Rename:
  ```bash
  mv bench/bench_rayforce.c bench/bench_rayforce.c
  ```
- [ ] Apply sed renames inside the file (if not already done in Task 2 — check):
  ```bash
  grep -n 'rayforce\|ray_\|RAY_' bench/bench_rayforce.c | head -5
  ```
- [ ] Update CMakeLists.txt if it references `bench_rayforce.c` by name
- [ ] **Do NOT commit yet** — continue to Task 10

---

## Task 10: Build and test

- [ ] Clean build directory and rebuild:
  ```bash
  rm -rf build
  cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build 2>&1 | tail -20
  ```
  Expected: clean compilation, no errors.
- [ ] If build fails, fix remaining rename issues. Common problems:
  - Missed `ray_` in a file not covered by `find`
  - Header guard mismatch
  - CMake target name typo
- [ ] Run full test suite:
  ```bash
  cd build && ctest --output-on-failure
  ```
  Expected: all tests pass.
- [ ] Smoke test the REPL:
  ```bash
  echo '(+ 1 2)' | ./build/rayforce
  ```
  Expected: `3`
- [ ] Smoke test banner:
  ```bash
  echo ':q' | ./build/rayforce 2>&1 | head -3
  ```
  Expected: banner with "Rayforce" (not shown in piped mode — just verify no crash)
- [ ] **Do NOT commit yet** — continue to Task 11

---

## Task 11: Final verification and commit

- [ ] Full grep for any remaining `rayforce` or `ray_` references (excluding build/, .git/, completed plans, .ralphex/):
  ```bash
  grep -rn 'rayforce\|ray_\|RAY_' --include='*.c' --include='*.h' --include='*.txt' --include='*.in' \
    src/ test/ bench/ examples/ fuzz/ include/ CMakeLists.txt CLAUDE.md README.md *.pc.in 2>/dev/null | \
    grep -v 'munit\|\.git' | head -20
  ```
  Expected: no matches (or only false positives like `stdout`, `stdin` which don't start with `ray_`).
- [ ] Commit everything:
  ```bash
  git add -A
  git status
  git commit -m "refactor: rename rayforce/ray_ to rayforce/ray_

  Merge Rayforce and Rayforce under the Rayforce project name.
  Purely mechanical rename — no logic changes.

  - ray_ prefix → ray_ (functions, types)
  - RAY_ prefix → RAY_ (constants, macros)
  - include/rayforce/rayforce.h → include/rayforce.h
  - rayforce_repl binary → rayforce binary
  - librayforce → librayforce
  - ~/.rayforce_history → ~/.rayforce_history

  Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
  ```

---

## Dependency Graph

```
Task 1 (move header) → Task 2 (bulk sed) → Task 3 (fix collateral)
→ Task 4 (CMakeLists) → Task 5 (pkg-config) → Task 6 (verify REPL)
→ Task 7 (CLAUDE.md) → Task 8 (README/docs) → Task 9 (bench rename)
→ Task 10 (build + test) → Task 11 (verify + commit)
```

All tasks are strictly sequential — each depends on the previous.
