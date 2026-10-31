/* The canonical printer (`langc fmt`, rulings Q-S11-1 to Q-S11-6). */
#ifndef LANG_FRONT_FMT_H
#define LANG_FRONT_FMT_H

#include "front/ast.h"
#include "front/lexer.h"

/* A text that grows in the arena. After a failed growth OOM is 1 and later
   appends do nothing. */
typedef struct {
  Arena *arena;
  char *data;
  size_t len;
  size_t cap;
  int oom;
} FmtText;

void fmt_text_init(FmtText *out, Arena *arena);
/* Appends the canonical text of declaration I. An `and` sort (block_first
   != I) prints the `and` head. No newline after the last line. */
void fmt_decl(FmtText *out, const Decl *decls, size_t i);
/* Appends region I of the canonical text: a blank line when I > 0 and
   declaration I is not an `and` sort, the comments from *NEXT_COMMENT up to
   the last line of declaration I (each on its own line), the declaration
   and a newline. Moves *NEXT_COMMENT past them. */
void fmt_region(FmtText *out, const Decl *decls, size_t i, const TokenList *toks, size_t *next_comment);
/* Appends the comments from NEXT_COMMENT on, after a blank line when
   DECL_COUNT > 0. */
void fmt_tail(FmtText *out, size_t decl_count, const TokenList *toks, size_t next_comment);
/* The canonical text of a whole source. Returns 1, or 0 when the arena is
   full. */
int fmt_program(FmtText *out, const Decl *decls, size_t count, const TokenList *toks);
/* Checks the source size and reparses OUT before a command emits it. A
   canonical spelling can use more bytes or parser depth than its input. */
int fmt_validate(const FmtText *out, const char *source, Diag *diag);

#endif
