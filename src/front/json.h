/* The `langc check --json` writer (S12 A2, rulings Q-S12-1 to Q-S12-8). */
#ifndef LANG_FRONT_JSON_H
#define LANG_FRONT_JSON_H

#include "front/check.h"

/* Writes TEXT as a JSON string: `"`, `\\`, newline and tab get a short
   escape, other control bytes `\u00XX`, an invalid UTF-8 byte `\ufffd`,
   valid UTF-8 raw. The escapes of quorum-lang src/front/json.c. */
void json_string_n(const char *text, size_t len, FILE *out);
void json_string(const char *text, FILE *out);
/* Writes the one-line object of `langc check --json` and a newline:
   `{"diagnostics":[...],"progress":{...}}` in this key order. DIAG gives
   the one diagnostic when DIAG->set, else the list is empty; its fix is
   null, or the edit of diag_fix. PATH is the span file, as given. */
void json_check(FILE *out, const char *path, const Diag *diag, const CheckProgress *progress);

#endif
