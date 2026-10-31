/* Arena allocation and the diagnostic sink. SHARED by the tcc kits. */
#ifndef LANG_FRONT_BASE_H
#define LANG_FRONT_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct ArenaChunk ArenaChunk;

/* Every compiler object lives in one arena. The arena frees all at once. */
typedef struct {
  ArenaChunk *head;
  size_t used_bytes;
  size_t limit_bytes;
} Arena;

void arena_init(Arena *arena, size_t limit_bytes);
/* Returns zeroed memory, or NULL when the limit is reached. */
void *arena_alloc(Arena *arena, size_t size);
char *arena_strndup(Arena *arena, const char *text, size_t len);
void arena_release(Arena *arena);

/* A source range, 1-based. END_COL is the column after the last byte, so an
   empty range has END_COL == COL. LINE 0 means no source position (S12). */
typedef struct {
  int line;
  int col;
  int end_line;
  int end_col;
} Span;

Span span_make(int line, int col, int end_line, int end_col);

/* The first failure wins. Later failures do not overwrite it. */
typedef struct {
  int set;
  const char *code;
  char def[64];
  char msg[512];
  Span span; /* the range of the failure (S12) */
  int fix_set;  /* the failure has a fix: replace fix_span by fix (S12) */
  int fix_next; /* diag_fix ran: the next failure takes the fix */
  Span fix_span;
  char fix[256];
} Diag;

void diag_init(Diag *diag);
/* Records the failure and returns 0, so a caller can write
   `return diag_fail(...)`. DEF is NULL when no definition applies. */
int diag_fail(Diag *diag, const char *code, const char *def, const char *fmt, ...);
/* As diag_fail, and records SPAN with the failure (S12). */
int diag_fail_at(Diag *diag, Span span, const char *code, const char *def, const char *fmt, ...);
/* Gives the next failure a fix: replace SPAN by TEXT ("" deletes). Does
   nothing when a failure is set or TEXT is NULL. Call it just before the
   failure (S12). */
void diag_fix(Diag *diag, Span span, const char *text);
/* Prints `langc: CODE: DEF: message`. */
void diag_print(const Diag *diag, FILE *out);
/* The byte size of the first UTF-8 scalar, or 0 for an invalid sequence
   (quorum-lang base.c; S12 json). */
size_t utf8_char_size(const char *text, size_t len);

#endif
