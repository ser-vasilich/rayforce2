# Kill RAY_CHAR — Unify All Strings Under RAY_STR

## Motivation

RAY_CHAR serves three confusing roles: single char atom, byte buffer for long string atoms, and vector type. The new RAY_STR vector (16-byte `ray_str_t` with inline/pool storage) makes RAY_CHAR obsolete. Removing it simplifies the type system and closes a gap in the type constant numbering.

## New Type Layout

```
1=BOOL  2=U8  3=I16  4=I32  5=I64  6=F32  7=F64  8=DATE  9=TIME  10=TIMESTAMP  11=GUID  12=SYM  13=STR
```

Changes from current: RAY_CHAR removed, I16..GUID shift down by 1, F32/F64 swapped to ascending size order. Contiguous 1..13 with no gaps.

## String Atom Model

`-RAY_STR` atom = single string. SSO for ≤6 bytes (inline in slen/sdata[7]). Long strings (≥7 bytes) store backing buffer in obj pointer (typed RAY_U8).

Char literal `'a'` parses to `ray_str("a", 1)` — 1-byte SSO, same 32-byte footprint as old RAY_CHAR atom.

`(first "hello")` returns `ray_str("h", 1)` instead of `ray_char('h')`.

## Migration

- **rayforce.h**: renumber constants, remove RAY_CHAR, remove c8 field
- **atom.c**: delete ray_char(), fix ray_str() long path (RAY_CHAR→RAY_U8)
- **parse.c**: char literals emit ray_str() instead of ray_char()
- **eval.c**: all -RAY_CHAR → -RAY_STR with string logic
- **format.c**: remove fmt_char(), merge into string display
- **exec.c**: remove RAY_CHAR dispatch
- **tests**: update type assertions, char→string
