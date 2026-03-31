/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#include "err.h"

static const char* err_strings[] = {
    [RAY_OK]          = "ok",
    [RAY_ERR_OOM]     = "out of memory",
    [RAY_ERR_TYPE]    = "type error",
    [RAY_ERR_RANGE]   = "range error",
    [RAY_ERR_LENGTH]  = "length mismatch",
    [RAY_ERR_RANK]    = "rank error",
    [RAY_ERR_DOMAIN]  = "domain error",
    [RAY_ERR_NYI]     = "not yet implemented",
    [RAY_ERR_IO]      = "I/O error",
    [RAY_ERR_SCHEMA]  = "schema error",
    [RAY_ERR_CORRUPT] = "corrupt data",
    [RAY_ERR_CANCEL]  = "query cancelled",
    [RAY_ERR_PARSE]   = "parse error",
    [RAY_ERR_NAME]    = "name error",
};

#define ERR_STRING_COUNT (sizeof(err_strings) / sizeof(err_strings[0]))

const char* ray_err_str(ray_err_t e) {
    if ((unsigned)e >= ERR_STRING_COUNT) return "unknown error";
    return err_strings[e];
}
