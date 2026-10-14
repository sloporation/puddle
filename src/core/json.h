#ifndef PUDDLE_JSON_H
#define PUDDLE_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A slice of the input. Strings keep their quotes; nothing is copied. */
typedef struct {
    const char *p;
    size_t len;
} puddle_span;

/* 1 if [json, json+len) is exactly one valid JSON object (nesting limited to 64), else 0. */
int puddle_json_valid_object(const char *json, size_t len);

/* Look up a top-level member of a JSON object and return the raw bytes of its value, exactly as
 * written (this is what event ids hash). Validates the whole object while scanning.
 *   0  found
 *   1  not found
 *  -1  malformed JSON, or the key appears more than once (ambiguous, so rejected) */
int puddle_json_member(const char *json, size_t len, const char *key, puddle_span *val);

/* Value helpers. 0 on success, -1 if the value is the wrong kind, out of range, or too long. */
int puddle_json_int64(puddle_span v, int64_t *out);
int puddle_json_string(puddle_span v, char *out, size_t cap); /* unescaped UTF-8, NUL-terminated */

#ifdef __cplusplus
}
#endif
#endif
