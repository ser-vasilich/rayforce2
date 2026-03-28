# Rayforce → Rayforce Rename — Design

**Goal:** Rename the project from Rayforce to Rayforce, merging the two vector engine codebases under a single name.

## Scope

### Prefix Rename (mechanical, every source file)

| Before | After |
|--------|-------|
| `ray_` (functions, types) | `ray_` |
| `RAY_` (constants, macros) | `RAY_` |
| `ray_t` (core block type) | `ray_t` |

### File & Directory Rename

| Before | After |
|--------|-------|
| `include/rayforce/rayforce.h` | `include/rayforce.h` |
| `include/rayforce/` (directory) | removed |

### Build System (CMakeLists.txt)

| Before | After |
|--------|-------|
| `project(rayforce)` | `project(rayforce)` |
| `rayforce_static` / `librayforce.a` | `rayforce_static` / `librayforce.a` |
| `rayforce` (shared) / `librayforce.so` | `rayforce_lib` / `librayforce.so` |
| `rayforce_repl` (binary) | `rayforce` (full binary: engine + REPL + terminal) |
| `test_rayforce` | `test_rayforce` |
| `rayforce.pc` | `rayforce.pc` |
| `RAYFORCE_*` cmake vars | `RAYFORCE_*` |

### User-Facing Strings

| Before | After |
|--------|-------|
| `~/.rayforce_history` | `~/.rayforce_history` |
| `Rayforce` in REPL banner | `Rayforce` |
| `RAYFORCE_VERSION` etc. | `RAYFORCE_VERSION` |

### Internal Headers

All `#include "rayforce/rayforce.h"` → `#include "rayforce.h"`
All `#include <rayforce/rayforce.h>` → `#include <rayforce.h>`
All header guards: `RAY_*_H` → `RAY_*_H`

### Source Comments & Docs

- CLAUDE.md references
- Plan docs mentioning Rayforce
- License headers (keep author, change project name)
- Test files

## Approach

One atomic commit. No logic changes — purely mechanical text substitution and file moves.

### Execution Order

1. Rename `include/rayforce/rayforce.h` → `include/rayforce.h`
2. Remove `include/rayforce/` directory
3. Sed-replace all prefixes across all `.c`, `.h`, `.md` files
4. Rename CMake targets and variables
5. Update CLAUDE.md
6. Build (`cmake -B build && cmake --build build`)
7. Run tests (`ctest --output-on-failure`)
8. Single commit

## Non-Goals

- No logic changes
- No API redesign
- No merging Rayforce runtime code (separate effort)
