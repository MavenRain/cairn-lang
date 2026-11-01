/* The refusal codes of `langc explain CODE` (S13 Q-S13-7 (a) and
   Q-S13-10 (a)), ported from evolve-lang src/codes.h (8c5d8e1). */
#ifndef LANG_FRONT_CODES_H
#define LANG_FRONT_CODES_H

#include <stddef.h>

/* One refusal code of docs/host/README.md. The section is the README
   section that gives the code; the meaning is its README text verbatim,
   with its Markdown. */
typedef struct {
  const char *code;
  const char *section;
  const char *meaning;
} CodeRow;

/* The number of rows: the 17 refusal codes of docs/host/README.md. */
size_t code_count(void);
/* Row INDEX in the order of docs/host/README.md, or NULL when INDEX is not
   below code_count(). */
const CodeRow *code_at(size_t index);
/* The row of CODE, or NULL when no row has that code. */
const CodeRow *code_find(const char *code);

#endif
