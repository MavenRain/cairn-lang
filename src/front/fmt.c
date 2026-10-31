/* The canonical printer (`langc fmt`): the charter-lang shape with the cairn
   decl forms. Rulings: Q-S11-1 layout, Q-S11-3 `Sigma (x : A) B`, Q-S11-4
   one name in each group and one `fun` for consecutive lambdas, Q-S11-5
   comments above the decl, Q-S11-6 one spelling for each word. */
#include "front/fmt.h"

#include <stdio.h>
#include <string.h>

#include "front/front.h"
#include "front/u256.h"

/* A decl whose flat text is longer breaks (Q-S11-1). */
#define FMT_WIDTH 100u

/* The precedence levels, low to high, as the parser reads them: an operand
   below the level that its place needs gets parentheses. */
typedef enum {
  LEVEL_TERM,
  LEVEL_JOIN,
  LEVEL_MEET,
  LEVEL_PAPP,
  LEVEL_APP,
  LEVEL_ATOM
} FmtLevel;

void fmt_text_init(FmtText *out, Arena *arena) {
  out->arena = arena;
  out->data = NULL;
  out->len = 0;
  out->cap = 0;
  out->oom = 0;
}

static void put_bytes(FmtText *out, const char *bytes, size_t n) {
  if (out->oom || n == 0) return;
  if (out->len + n > out->cap) {
    size_t cap = out->cap == 0 ? 256u : out->cap;
    char *data;
    while (cap < out->len + n) cap *= 2u;
    data = arena_alloc(out->arena, cap);
    if (data == NULL) {
      out->oom = 1;
      return;
    }
    if (out->len > 0) memcpy(data, out->data, out->len);
    out->data = data;
    out->cap = cap;
  }
  memcpy(out->data + out->len, bytes, n);
  out->len += n;
}

static void put_str(FmtText *out, const char *text) {
  put_bytes(out, text, strlen(text));
}

static void put_nat(FmtText *out, uint64_t n) {
  char digits[24];
  size_t at = sizeof digits;
  do {
    digits[--at] = (char)('0' + n % 10u);
    n /= 10u;
  } while (n != 0);
  put_bytes(out, digits + at, sizeof digits - at);
}

/* A U256 below 2^64 is decimal, else lower-case hex with no leading zero;
   then `u`. An Addr is `0x` and 40 lower-case hex digits. */
static void put_word(FmtText *out, uint64_t sort, const U256 *w) {
  char text[96];
  size_t top = 3;
  size_t at;
  if (sort == WORD_ADDR) {
    word_text(WORD_ADDR, w, text, sizeof text);
    put_str(out, text);
    return;
  }
  while (top > 0 && w->limb[top] == 0) top--;
  if (top == 0) {
    put_nat(out, w->limb[0]);
  } else {
    at = (size_t)snprintf(text, sizeof text, "0x%llx", (unsigned long long)w->limb[top]);
    for (size_t k = top; k > 0; k--) {
      at += (size_t)snprintf(text + at, sizeof text - at, "%016llx", (unsigned long long)w->limb[k - 1]);
    }
    put_bytes(out, text, at);
  }
  put_str(out, "u");
}

/* Type n and Sigma are parenthesized as an argument, so they sit at the
   application level; `~` binds tighter than application. A system and a
   branch are never parenthesized. */
static FmtLevel term_level(const Term *t) {
  switch (t->kind) {
    case TERM_LAM:
    case TERM_PI:
      return LEVEL_TERM;
    case TERM_IJOIN: return LEVEL_JOIN;
    case TERM_IMEET: return LEVEL_MEET;
    case TERM_PAPP: return LEVEL_PAPP;
    case TERM_APP:
    case TERM_SIGMA:
    case TERM_UNIV:
      return LEVEL_APP;
    case TERM_VAR:
    case TERM_NAT:
    case TERM_WORD:
    case TERM_INEG:
    case TERM_SYSTEM:
    case TERM_BRANCH:
      break;
  }
  return LEVEL_ATOM;
}

static void put_term(FmtText *out, const Term *t, FmtLevel level);

/* `(x : A)`: one name in each group (Q-S11-4). */
static void put_group(FmtText *out, const char *name, const Term *type) {
  put_str(out, "(");
  put_str(out, name);
  put_str(out, " : ");
  put_term(out, type, LEVEL_TERM);
  put_str(out, ")");
}

/* All leading lambdas go into one binder head; a NULL type is a bare name. */
static void put_lambda(FmtText *out, const Term *t) {
  const Term *body = t;
  put_str(out, "fun");
  while (body->kind == TERM_LAM) {
    put_str(out, " ");
    if (body->left == NULL) {
      put_str(out, body->name);
    } else {
      put_group(out, body->name, body->left);
    }
    body = body->right;
  }
  put_str(out, " => ");
  put_term(out, body, LEVEL_TERM);
}

/* `[ k1 -> t1, k2 -> t2 ]`; the one cell of `[ ]` has no branch. */
static void put_system(FmtText *out, const Term *t) {
  put_str(out, "[");
  for (const Term *cell = t; cell != NULL; cell = cell->right) {
    if (cell->left != NULL) {
      put_str(out, cell == t ? " " : ", ");
      put_term(out, cell->left, LEVEL_ATOM);
    }
  }
  put_str(out, " ]");
}

/* A left-associative operator: the left operand at the level of the
   operator, the right operand one level up. */
static void put_binary(FmtText *out, const Term *t, const char *op, FmtLevel left, FmtLevel right) {
  put_term(out, t->left, left);
  put_str(out, op);
  put_term(out, t->right, right);
}

static void put_term(FmtText *out, const Term *t, FmtLevel level) {
  int paren = term_level(t) < level;
  if (paren) put_str(out, "(");
  switch (t->kind) {
    case TERM_VAR: put_str(out, t->name); break;
    case TERM_NAT: put_nat(out, t->nat); break;
    case TERM_WORD: put_word(out, t->nat, t->word); break;
    case TERM_APP: put_binary(out, t, " ", LEVEL_APP, LEVEL_ATOM); break;
    case TERM_LAM: put_lambda(out, t); break;
    case TERM_PI:
      if (t->name == NULL) {
        put_term(out, t->left, LEVEL_JOIN);
      } else {
        put_group(out, t->name, t->left);
      }
      put_str(out, " -> ");
      put_term(out, t->right, LEVEL_TERM);
      break;
    case TERM_SIGMA:
      put_str(out, "Sigma ");
      put_group(out, t->name, t->left);
      put_str(out, " ");
      put_term(out, t->right, LEVEL_ATOM);
      break;
    case TERM_UNIV:
      put_str(out, "Type ");
      put_nat(out, t->nat);
      break;
    case TERM_PAPP: put_binary(out, t, " @ ", LEVEL_PAPP, LEVEL_APP); break;
    case TERM_INEG:
      put_str(out, "~");
      put_term(out, t->left, LEVEL_ATOM);
      break;
    case TERM_IMEET: put_binary(out, t, " /\\ ", LEVEL_MEET, LEVEL_PAPP); break;
    case TERM_IJOIN: put_binary(out, t, " \\/ ", LEVEL_JOIN, LEVEL_MEET); break;
    case TERM_SYSTEM: put_system(out, t); break;
    case TERM_BRANCH: put_binary(out, t, " -> ", LEVEL_JOIN, LEVEL_TERM); break;
  }
  if (paren) put_str(out, ")");
}

/* The groups of a parameter or field list; a state field after the history
   marker prints `history (x : A)`. */
static void put_fields(FmtText *out, const Field *fields, size_t count) {
  for (size_t k = 0; k < count; k++) {
    put_str(out, fields[k].history ? " history " : " ");
    put_group(out, fields[k].name, fields[k].type);
  }
}

static void put_ctor(FmtText *out, const Ctor *c) {
  put_str(out, c->name);
  put_fields(out, c->fields, c->field_count);
  if (c->result != NULL) {
    put_str(out, " : ");
    put_term(out, c->result, LEVEL_TERM);
  }
  if (c->boundary != NULL) {
    put_str(out, " ");
    put_system(out, c->boundary);
  }
}

static const char *family_head(const Decl *d) {
  if (d->is_event) return "event ";
  if (d->is_state) return "state ";
  return d->is_sealed ? "sealed family " : "family ";
}

/* The keywords and the name. A sort after the first of its block is an
   `and` sort. */
static void put_head(FmtText *out, const Decl *decls, size_t i) {
  const Decl *d = &decls[i];
  if (d->block_first != i) {
    put_str(out, "and ");
  } else {
    switch (d->kind) {
      case DECL_DEF:
        put_str(out, d->is_law ? "law def " : "def ");
        if (d->is_rec) put_str(out, "rec ");
        break;
      case DECL_AXIOM: put_str(out, "axiom "); break;
      case DECL_FAMILY: put_str(out, family_head(d)); break;
      case DECL_COFAMILY: put_str(out, "cofamily "); break;
    }
  }
  put_str(out, d->name);
}

/* BROKEN: a def body goes on line 2 at a 2-space indent; an axiom never
   breaks. */
static void put_def(FmtText *out, const Decl *decls, size_t i, int broken) {
  const Decl *d = &decls[i];
  put_head(out, decls, i);
  put_str(out, " : ");
  put_term(out, d->type, LEVEL_TERM);
  if (d->kind == DECL_AXIOM) return;
  put_str(out, broken ? " :=\n  " : " := ");
  put_term(out, d->body, LEVEL_TERM);
}

/* BROKEN: each constructor after the first goes on its own line `  | c`;
   an event never breaks. */
static void put_family(FmtText *out, const Decl *decls, size_t i, int broken) {
  const Decl *d = &decls[i];
  put_head(out, decls, i);
  if (d->is_event) {
    if (d->ctor_count > 0) put_fields(out, d->ctors[0].fields, d->ctors[0].field_count);
    return;
  }
  put_fields(out, d->params, d->param_count);
  if (d->index != NULL) {
    put_str(out, " : ");
    put_term(out, d->index, LEVEL_TERM);
  }
  put_str(out, " :=");
  for (size_t k = 0; k < d->ctor_count; k++) {
    put_str(out, k == 0 ? " " : broken ? "\n  | " : " | ");
    put_ctor(out, &d->ctors[k]);
  }
}

static void put_decl(FmtText *out, const Decl *decls, size_t i, int broken) {
  switch (decls[i].kind) {
    case DECL_DEF:
    case DECL_AXIOM:
      put_def(out, decls, i, broken);
      break;
    case DECL_FAMILY:
    case DECL_COFAMILY:
      put_family(out, decls, i, broken);
      break;
  }
}

void fmt_decl(FmtText *out, const Decl *decls, size_t i) {
  FmtText flat;
  fmt_text_init(&flat, out->arena);
  put_decl(&flat, decls, i, 0);
  out->oom |= flat.oom;
  if (flat.len <= FMT_WIDTH) {
    put_bytes(out, flat.data, flat.len);
  } else {
    put_decl(out, decls, i, 1);
  }
}

static void put_comment(FmtText *out, const Comment *c) {
  put_bytes(out, c->text, c->len);
  put_str(out, "\n");
}

void fmt_region(FmtText *out, const Decl *decls, size_t i, const TokenList *toks, size_t *next_comment) {
  if (i > 0 && decls[i].block_first == i) put_str(out, "\n");
  while (*next_comment < toks->comment_count && toks->comments[*next_comment].line <= decls[i].end_line) {
    put_comment(out, &toks->comments[*next_comment]);
    (*next_comment)++;
  }
  fmt_decl(out, decls, i);
  put_str(out, "\n");
}

void fmt_tail(FmtText *out, size_t decl_count, const TokenList *toks, size_t next_comment) {
  if (decl_count > 0 && next_comment < toks->comment_count) put_str(out, "\n");
  for (size_t k = next_comment; k < toks->comment_count; k++) put_comment(out, &toks->comments[k]);
}

int fmt_program(FmtText *out, const Decl *decls, size_t count, const TokenList *toks) {
  size_t next_comment = 0;
  for (size_t i = 0; i < count; i++) fmt_region(out, decls, i, toks, &next_comment);
  fmt_tail(out, count, toks, next_comment);
  return !out->oom;
}

int fmt_validate(const FmtText *out, const char *source, Diag *diag) {
  if (out->len > SOURCE_MAX_BYTES)
    return diag_fail(diag, "FMT_SIZE", NULL, "%s: canonical text is larger than %zu bytes", source, SOURCE_MAX_BYTES);
  TokenList toks = {0};
  DeclList decls = {0};
  Diag parsed;
  diag_init(&parsed);
  const char *text = out->data == NULL ? "" : out->data;
  if (!lex_source(out->arena, "canonical text", text, out->len, &toks, &parsed)
      || !parse_source(out->arena, "canonical text", ORIGIN_PROGRAM, &toks, &decls, &parsed))
    return diag_fail(diag, parsed.code, parsed.def, "%s: cannot emit canonical text: %s", source, parsed.msg);
  return 1;
}
