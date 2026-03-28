#ifndef RAY_FORMAT_H
#define RAY_FORMAT_H

#include <rayforce.h>
#include <stdio.h>

#define FMT_TABLE_MAX_WIDTH   10
#define FMT_TABLE_MAX_HEIGHT  20
#define FMT_LIST_MAX_HEIGHT   50
#define FMT_DEFAULT_ROW_WIDTH 80
#define FMT_DEFAULT_PRECISION  2

/* Format a ray_t value into a new ray_t string (RAY_STR atom).
 * mode: 0 = compact, 1 = full (REPL), 2 = show (no limits) */
ray_t* ray_fmt(ray_t* obj, int mode);

/* Format and write to FILE* */
void ray_fmt_print(FILE* fp, ray_t* obj, int mode);

/* Display settings */
void ray_fmt_set_precision(int digits);
void ray_fmt_set_width(int cols);

/* Type name string (e.g. RAY_I64 -> "i64") */
const char* ray_type_name(int8_t type);

#endif /* RAY_FORMAT_H */
