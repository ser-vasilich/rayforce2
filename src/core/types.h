/*
 *   Copyright (c) 2024-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_TYPES_H
#define RAY_TYPES_H

/*
 * types.h — Internal types header.
 *
 * The canonical type definitions (ray_t, type constants, attribute flags)
 * live in <rayforce.h> (the public header).
 * Internal .c files can include either rayforce.h directly or types.h.
 */
#include <rayforce.h>

/* --------------------------------------------------------------------------
 * Type classification helpers (operate on positive type tags)
 * -------------------------------------------------------------------------- */
/* Numeric: BOOL, U8, CHAR, I16, I32, I64, F64 */
#define RAY_IS_NUMERIC(t) ((t) >= RAY_BOOL && (t) <= RAY_F64)

/* Integer: BOOL, U8, CHAR, I16, I32, I64 */
#define RAY_IS_INTEGER(t) ((t) >= RAY_BOOL && (t) <= RAY_I64)

/* Float: F64 only */
#define RAY_IS_FLOAT(t)   ((t) == RAY_F64)

#endif /* RAY_TYPES_H */
