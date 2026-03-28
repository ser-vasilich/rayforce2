#ifndef RAY_PARSE_H
#define RAY_PARSE_H

#include <rayforce.h>

/* Parse a Rayfall source string into a ray_t object tree.
 * Returns a single expression, or a list of expressions if the
 * source contains multiple top-level forms. */
ray_t* ray_parse(const char* source);

#endif /* RAY_PARSE_H */
