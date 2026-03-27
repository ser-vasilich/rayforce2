#ifndef TD_PARSE_H
#define TD_PARSE_H

#include <teide/td.h>

/* Parse a Rayfall source string into a td_t object tree.
 * Returns a single expression, or a list of expressions if the
 * source contains multiple top-level forms. */
td_t* td_parse(const char* source);

#endif /* TD_PARSE_H */
