/* Grammar:
     decl    := 'def' 'rec'? NAME ':' term ':=' term
              | 'axiom' NAME ':' term
              | 'family' NAME group* ':=' ctor ('|' ctor)*
              | 'event' NAME group*
     ctor    := NAME group* ('[' cells ']')?
     group   := '(' NAME+ ':' term ')'
     term    := 'fun' (NAME | group)+ '=>' term
              | group ('->' | '*') term
              | join ('->' term)?
     join    := meet ('\/' meet)*
     meet    := papp ('/\' papp)*
     papp    := app ('@' app)*
     app     := atom atom*
     atom    := NAME | NAT | WORD | 'Type' NAT | 'Sigma' group atom | '(' term ')'
              | '~' atom */
#include <string.h>

#include "front/parser.h"

#define PARSE_MAX_DEPTH 1000

typedef struct {
  const Token *toks;
  size_t count;
  size_t pos;
  Arena *arena;
  Diag *diag;
  const char *source;
  const char *def;
  Origin origin;
  int depth;
} Parser;

/* One binder group: `(x y : A)` gives two names that share one type. */
typedef struct {
  const char **names;
  size_t count;
  const Term *type;
  const Token *tok;
} Binders;

static const Term *parse_term(Parser *p);
static const Term *parse_atom(Parser *p);

static const Token *peek_at(const Parser *p, size_t ahead) {
  size_t i = p->pos + ahead;
  return &p->toks[i < p->count ? i : p->count - 1];
}

static const Token *cur(const Parser *p) {
  return peek_at(p, 0);
}

static int at(const Parser *p, TokKind kind) {
  return cur(p)->kind == kind;
}

static void bump(Parser *p) {
  p->pos += p->pos + 1 < p->count ? 1u : 0u;
}

static int eat(Parser *p, TokKind kind) {
  if (!at(p, kind)) return 0;
  bump(p);
  return 1;
}

/* The range of TOK (S12). */
static Span tok_span(const Token *tok) {
  return span_make(tok->line, tok->col, tok->line, tok->col + (int)tok->len);
}

/* Record the range of the tokens of the type of D: type_start before
   parse_term, type_end after it (S12). */
static void type_start(const Parser *p, Decl *d) {
  d->type_line = p->toks[p->pos].line;
  d->type_col = p->toks[p->pos].col;
}

static void type_end(const Parser *p, Decl *d) {
  const Token *last = &p->toks[p->pos - 1];
  d->type_end_line = last->line;
  d->type_end_col = last->col + (int)last->len;
}

static int oom(Parser *p) {
  return diag_fail(p->diag, "OOM", p->def, "%s: the arena limit is reached", p->source);
}

static int fail_at(Parser *p, const Token *tok, const char *what) {
  return diag_fail_at(p->diag, tok_span(tok), "PARSE_EXPECT", p->def, "%s:%d:%d: expected %s, found %s",
                      p->source, tok->line, tok->col, what, tok_kind_name(tok->kind));
}

static int expect(Parser *p, TokKind kind, const char *what) {
  return eat(p, kind) ? 1 : fail_at(p, cur(p), what);
}

static int enter(Parser *p) {
  p->depth++;
  if (p->depth <= PARSE_MAX_DEPTH) return 1;
  return diag_fail_at(p->diag, tok_span(cur(p)), "PARSE_DEPTH", p->def, "%s:%d:%d: terms nest deeper than %d levels",
                      p->source, cur(p)->line, cur(p)->col, PARSE_MAX_DEPTH);
}

static const char *token_text(Parser *p, const Token *tok) {
  char *text = arena_strndup(p->arena, tok->text, tok->len);
  if (text == NULL) oom(p);
  return text;
}

static const char *take_name(Parser *p, const char *what) {
  if (!at(p, TOK_IDENT)) {
    fail_at(p, cur(p), what);
    return NULL;
  }
  const Token *tok = cur(p);
  bump(p);
  return token_text(p, tok);
}

static Term *make_term(Parser *p, TermKind kind, int line, int col) {
  Term *term = arena_alloc(p->arena, sizeof *term);
  if (term == NULL) {
    oom(p);
    return NULL;
  }
  term->kind = kind;
  term->line = line;
  term->col = col;
  return term;
}

/* Grows an arena array by doubling. The old block stays in the arena. */
static void *grow(Parser *p, void *items, size_t count, size_t *cap, size_t size) {
  if (count < *cap) return items;
  size_t next = *cap == 0 ? 4 : *cap * 2;
  void *fresh = arena_alloc(p->arena, next * size);
  if (fresh == NULL) {
    oom(p);
    return NULL;
  }
  if (count > 0) memcpy(fresh, items, count * size);
  *cap = next;
  return fresh;
}

static int starts_atom(TokKind kind) {
  switch (kind) {
    case TOK_IDENT:
    case TOK_NAT:
    case TOK_WORD:
    case TOK_LPAREN:
    case TOK_KW_TYPE:
    case TOK_KW_SIGMA:
    case TOK_TILDE:
      return 1;
    case TOK_EOF:
    case TOK_RPAREN:
    case TOK_COLON:
    case TOK_DEFINE:
    case TOK_ARROW:
    case TOK_FATARROW:
    case TOK_STAR:
    case TOK_BAR:
    case TOK_AT:
    case TOK_MEET:
    case TOK_JOIN:
    case TOK_LBRACKET:
    case TOK_RBRACKET:
    case TOK_COMMA:
    case TOK_KW_DEF:
    case TOK_KW_REC:
    case TOK_KW_FUN:
    case TOK_KW_FAMILY:
    case TOK_KW_AXIOM:
    case TOK_KW_STATE:
    case TOK_KW_EVENT:
    case TOK_KW_AND:
    case TOK_KW_COFAMILY:
    case TOK_KW_SEALED:
    case TOK_KW_LAW:
      return 0;
  }
  return 0;
}

/* `(` NAME+ `:` starts a binder group. `(f x)` does not. */
static int at_binder_group(const Parser *p) {
  if (!at(p, TOK_LPAREN) || peek_at(p, 1)->kind != TOK_IDENT) return 0;
  size_t ahead = 2;
  while (peek_at(p, ahead)->kind == TOK_IDENT) ahead++;
  return peek_at(p, ahead)->kind == TOK_COLON;
}

static int parse_group(Parser *p, Binders *out) {
  out->tok = cur(p);
  if (!expect(p, TOK_LPAREN, "'('")) return 0;
  size_t start = p->pos;
  while (at(p, TOK_IDENT)) bump(p);
  out->count = p->pos - start;
  if (out->count == 0) return fail_at(p, cur(p), "a binder name");
  out->names = arena_alloc(p->arena, out->count * sizeof *out->names);
  if (out->names == NULL) return oom(p);
  for (size_t i = 0; i < out->count; i++) {
    out->names[i] = token_text(p, &p->toks[start + i]);
    if (out->names[i] == NULL) return 0;
  }
  if (!expect(p, TOK_COLON, "':'")) return 0;
  out->type = parse_term(p);
  return out->type != NULL && expect(p, TOK_RPAREN, "')'");
}

/* A bare lambda binder `x` has no type. The checker infers it. */
static int parse_bare(Parser *p, Binders *out) {
  out->tok = cur(p);
  out->names = arena_alloc(p->arena, sizeof *out->names);
  if (out->names == NULL) return oom(p);
  out->names[0] = take_name(p, "a binder");
  out->count = 1;
  out->type = NULL;
  return out->names[0] != NULL;
}

/* Builds the nested binders of B around BODY, the last name innermost. */
static const Term *wrap(Parser *p, TermKind kind, const Binders *b, const Term *body) {
  const Term *acc = body;
  for (size_t i = b->count; i > 0 && acc != NULL; i--) {
    Term *node = make_term(p, kind, b->tok->line, b->tok->col);
    if (node == NULL) return NULL;
    node->name = b->names[i - 1];
    node->left = b->type;
    node->right = acc;
    acc = node;
  }
  return acc;
}

static const Term *parse_join(Parser *p);

/* One branch `k -> t` of a system. The key is at the join level, so `->`
   ends it. */
static const Term *parse_branch(Parser *p) {
  const Token *tok = cur(p);
  const Term *key = parse_join(p);
  const Term *body = key != NULL && expect(p, TOK_ARROW, "'->' after a key") ? parse_term(p) : NULL;
  Term *branch = body == NULL ? NULL : make_term(p, TERM_BRANCH, tok->line, tok->col);
  if (branch == NULL) return NULL;
  branch->left = key;
  branch->right = body;
  return branch;
}

/* The cells of a system after its first branch: `, k -> t ... ]`. */
static const Term *parse_cells(Parser *p, const Token *tok) {
  const Term *branch = NULL;
  const Term *rest = NULL;
  Term *cell;
  if (at(p, TOK_RBRACKET)) {
    bump(p);
  } else {
    branch = parse_branch(p);
    if (branch == NULL) return NULL;
    if (eat(p, TOK_COMMA)) {
      if (at(p, TOK_RBRACKET)) {
        fail_at(p, cur(p), "a branch after ','");
        return NULL;
      }
      if (!enter(p)) return NULL;
      rest = parse_cells(p, cur(p));
      p->depth--;
      if (rest == NULL) return NULL;
    } else if (!expect(p, TOK_RBRACKET, "',' or ']'")) {
      return NULL;
    }
  }
  cell = make_term(p, TERM_SYSTEM, tok->line, tok->col);
  if (cell == NULL) return NULL;
  cell->left = branch;
  cell->right = rest;
  return cell;
}

/* `[ k1 -> t1, ... ]`, as the body of a `fun` or as the boundary of a
   constructor. */
static const Term *parse_system(Parser *p) {
  const Token *tok = cur(p);
  bump(p);
  return parse_cells(p, tok);
}

static const Term *parse_lambda_tail(Parser *p) {
  if (eat(p, TOK_FATARROW)) return at(p, TOK_LBRACKET) ? parse_system(p) : parse_term(p);
  Binders b;
  int ok = at(p, TOK_IDENT) ? parse_bare(p, &b) : parse_group(p, &b);
  if (!ok || !enter(p)) return NULL;
  const Term *body = parse_lambda_tail(p);
  p->depth--;
  return body == NULL ? NULL : wrap(p, TERM_LAM, &b, body);
}

static const Term *parse_fun(Parser *p) {
  bump(p);
  if (at(p, TOK_FATARROW)) {
    fail_at(p, cur(p), "a binder after 'fun'");
    return NULL;
  }
  return parse_lambda_tail(p);
}

static const Term *parse_binder_type(Parser *p) {
  Binders b;
  if (!parse_group(p, &b)) return NULL;
  int sigma = at(p, TOK_STAR);
  if (!sigma && !at(p, TOK_ARROW)) {
    fail_at(p, cur(p), "'->' or '*' after a binder");
    return NULL;
  }
  bump(p);
  const Term *body = parse_term(p);
  return body == NULL ? NULL : wrap(p, sigma ? TERM_SIGMA : TERM_PI, &b, body);
}

static const Term *parse_universe(Parser *p) {
  const Token *tok = cur(p);
  bump(p);
  if (!at(p, TOK_NAT)) {
    fail_at(p, cur(p), "a universe level after 'Type'");
    return NULL;
  }
  Term *term = make_term(p, TERM_UNIV, tok->line, tok->col);
  if (term == NULL) return NULL;
  term->nat = cur(p)->nat;
  bump(p);
  return term;
}

static const Term *parse_sigma_tail(Parser *p) {
  if (!at_binder_group(p)) {
    fail_at(p, cur(p), "a binder (x : A) after 'Sigma'");
    return NULL;
  }
  Binders b;
  if (!parse_group(p, &b)) return NULL;
  const Term *body = parse_atom(p);
  return body == NULL ? NULL : wrap(p, TERM_SIGMA, &b, body);
}

/* `Sigma (x : A) Sigma (y : B) C` nests without parentheses, so it counts
   toward the depth limit. */
static const Term *parse_sigma(Parser *p) {
  bump(p);
  const Term *term = enter(p) ? parse_sigma_tail(p) : NULL;
  p->depth--;
  return term;
}

static const Term *parse_paren(Parser *p) {
  bump(p);
  const Term *inner = parse_term(p);
  return inner != NULL && expect(p, TOK_RPAREN, "')'") ? inner : NULL;
}

static const Term *parse_leaf(Parser *p, TermKind kind) {
  const Token *tok = cur(p);
  Term *term = make_term(p, kind, tok->line, tok->col);
  if (term == NULL) return NULL;
  term->nat = tok->nat;
  term->word = kind == TERM_WORD ? &tok->word : NULL;
  term->name = kind == TERM_VAR ? token_text(p, tok) : NULL;
  bump(p);
  return kind == TERM_VAR && term->name == NULL ? NULL : term;
}

/* `~r` binds tighter than application: `~(i /\ j)`. */
static const Term *parse_neg(Parser *p) {
  const Token *tok = cur(p);
  bump(p);
  const Term *inner = enter(p) ? parse_atom(p) : NULL;
  p->depth--;
  Term *term = inner == NULL ? NULL : make_term(p, TERM_INEG, tok->line, tok->col);
  if (term == NULL) return NULL;
  term->left = inner;
  return term;
}

static const Term *parse_atom(Parser *p) {
  const Token *tok = cur(p);
  switch (tok->kind) {
    case TOK_IDENT: return parse_leaf(p, TERM_VAR);
    case TOK_NAT: return parse_leaf(p, TERM_NAT);
    case TOK_WORD: return parse_leaf(p, TERM_WORD);
    case TOK_LPAREN: return parse_paren(p);
    case TOK_KW_TYPE: return parse_universe(p);
    case TOK_KW_SIGMA: return parse_sigma(p);
    case TOK_TILDE: return parse_neg(p);
    case TOK_EOF:
    case TOK_RPAREN:
    case TOK_COLON:
    case TOK_DEFINE:
    case TOK_ARROW:
    case TOK_FATARROW:
    case TOK_STAR:
    case TOK_BAR:
    case TOK_AT:
    case TOK_MEET:
    case TOK_JOIN:
    case TOK_LBRACKET:
    case TOK_RBRACKET:
    case TOK_COMMA:
    case TOK_KW_DEF:
    case TOK_KW_REC:
    case TOK_KW_FUN:
    case TOK_KW_FAMILY:
    case TOK_KW_AXIOM:
    case TOK_KW_STATE:
    case TOK_KW_EVENT:
    case TOK_KW_AND:
    case TOK_KW_COFAMILY:
    case TOK_KW_SEALED:
    case TOK_KW_LAW:
      break;
  }
  fail_at(p, tok, "a term");
  return NULL;
}

static const Term *parse_app(Parser *p) {
  const Term *head = parse_atom(p);
  while (head != NULL && starts_atom(cur(p)->kind)) {
    const Term *arg = parse_atom(p);
    Term *app = arg == NULL ? NULL : make_term(p, TERM_APP, head->line, head->col);
    if (app == NULL) return NULL;
    app->left = head;
    app->right = arg;
    head = app;
  }
  return head;
}

/* A left-associative chain `x OP y OP z` of the operands that SUB parses. */
static const Term *parse_chain(Parser *p, TokKind op, TermKind kind, const Term *(*sub)(Parser *)) {
  const Term *head = sub(p);
  while (head != NULL && at(p, op)) {
    const Token *tok = cur(p);
    bump(p);
    const Term *rhs = sub(p);
    Term *node = rhs == NULL ? NULL : make_term(p, kind, tok->line, tok->col);
    if (node == NULL) return NULL;
    node->left = head;
    node->right = rhs;
    head = node;
  }
  return head;
}

/* `p @ r` is looser than application: `f x @ i` is `(f x) @ i`. */
static const Term *parse_papp(Parser *p) {
  return parse_chain(p, TOK_AT, TERM_PAPP, parse_app);
}

static const Term *parse_meet(Parser *p) {
  return parse_chain(p, TOK_MEET, TERM_IMEET, parse_papp);
}

/* `/\` binds tighter than `\/`. */
static const Term *parse_join(Parser *p) {
  return parse_chain(p, TOK_JOIN, TERM_IJOIN, parse_meet);
}

static const Term *parse_term_body(Parser *p) {
  if (at(p, TOK_KW_FUN)) return parse_fun(p);
  if (at_binder_group(p)) return parse_binder_type(p);
  const Term *head = parse_join(p);
  if (head == NULL || !at(p, TOK_ARROW)) return head;
  const Token *arrow = cur(p);
  bump(p);
  const Term *body = parse_term(p);
  Term *pi = body == NULL ? NULL : make_term(p, TERM_PI, arrow->line, arrow->col);
  if (pi == NULL) return NULL;
  pi->name = NULL;
  pi->left = head;
  pi->right = body;
  return pi;
}

static const Term *parse_term(Parser *p) {
  const Term *term = enter(p) ? parse_term_body(p) : NULL;
  p->depth--;
  return term;
}

/* `history (` marks the fields of the next group in the state form (slice
   K3a). It is not a keyword: `history` stays a legal name. */
static int at_history(const Parser *p) {
  const Token *tok = cur(p);
  return tok->kind == TOK_IDENT && tok->len == 7u && memcmp(tok->text, "history", 7u) == 0
      && peek_at(p, 1)->kind == TOK_LPAREN;
}

/* MARKS is 1 for the constructor fields of the state form. */
static int parse_fields(Parser *p, int marks, const Field **out, size_t *out_count) {
  Field *items = NULL;
  size_t count = 0;
  size_t cap = 0;
  while (at_binder_group(p) || (marks && at_history(p))) {
    Binders b;
    int history = marks && at_history(p);
    if (history) bump(p);
    if (!parse_group(p, &b)) return 0;
    for (size_t i = 0; i < b.count; i++) {
      items = grow(p, items, count, &cap, sizeof *items);
      if (items == NULL) return 0;
      items[count].name = b.names[i];
      items[count].type = b.type;
      items[count].line = b.tok->line;
      items[count].col = b.tok->col;
      items[count].history = history;
      count++;
    }
  }
  *out = items;
  *out_count = count;
  return 1;
}

static int parse_def(Parser *p, Decl *d) {
  bump(p);
  d->kind = DECL_DEF;
  d->is_rec = eat(p, TOK_KW_REC);
  d->name = take_name(p, "a definition name");
  if (d->name == NULL) return 0;
  p->def = d->name;
  if (!expect(p, TOK_COLON, "':'")) return 0;
  type_start(p, d);
  d->type = parse_term(p);
  type_end(p, d);
  if (d->type == NULL || !expect(p, TOK_DEFINE, "':='")) return 0;
  d->body = parse_term(p);
  return d->body != NULL;
}

static int parse_axiom(Parser *p, Decl *d) {
  bump(p);
  d->kind = DECL_AXIOM;
  d->name = take_name(p, "an axiom name");
  if (d->name == NULL) return 0;
  p->def = d->name;
  if (!expect(p, TOK_COLON, "':'")) return 0;
  type_start(p, d);
  d->type = parse_term(p);
  type_end(p, d);
  return d->type != NULL;
}

/* A cofamily has the form of a family (S4): its one constructor is the
   corecursor and its fields are the observations. */
static int parse_family(Parser *p, Decl *d) {
  d->kind = at(p, TOK_KW_COFAMILY) ? DECL_COFAMILY : DECL_FAMILY;
  bump(p);
  d->name = take_name(p, "a family name");
  if (d->name == NULL) return 0;
  p->def = d->name;
  if (!parse_fields(p, 0, &d->params, &d->param_count)) return 0;
  d->index = eat(p, TOK_COLON) ? parse_term(p) : NULL;
  if ((d->index == NULL && p->diag->set) || !expect(p, TOK_DEFINE, "':='")) return 0;
  Ctor *ctors = NULL;
  size_t count = 0;
  size_t cap = 0;
  do {
    ctors = grow(p, ctors, count, &cap, sizeof *ctors);
    if (ctors == NULL) return 0;
    Ctor *ctor = &ctors[count];
    count++;
    ctor->line = cur(p)->line;
    ctor->col = cur(p)->col;
    ctor->name = take_name(p, "a constructor name");
    if (ctor->name == NULL || !parse_fields(p, d->is_state, &ctor->fields, &ctor->field_count)) return 0;
    ctor->result = eat(p, TOK_COLON) ? parse_term(p) : NULL;
    if (ctor->result == NULL && p->diag->set) return 0;
    ctor->boundary = at(p, TOK_LBRACKET) ? parse_system(p) : NULL;
    if (ctor->boundary == NULL && p->diag->set) return 0;
  } while (eat(p, TOK_BAR));
  d->ctors = ctors;
  d->ctor_count = count;
  return 1;
}

/* `event NAME group*` (slice K4a1): a family declaration with one
   constructor, the event. The checker adds the constructor to Out. */
static int parse_event(Parser *p, Decl *d) {
  size_t cap = 0;
  Ctor *ctor;
  bump(p);
  d->kind = DECL_FAMILY;
  d->is_rec = 0;
  d->type = NULL;
  d->body = NULL;
  d->params = NULL;
  d->param_count = 0;
  d->index = NULL;
  d->name = take_name(p, "an event name");
  if (d->name == NULL) return 0;
  p->def = d->name;
  ctor = grow(p, NULL, 0, &cap, sizeof *ctor);
  if (ctor == NULL) return 0;
  ctor->name = d->name;
  ctor->line = d->line;
  ctor->col = d->col;
  ctor->result = NULL;
  ctor->boundary = NULL;
  d->ctors = ctor;
  d->ctor_count = 1u;
  return parse_fields(p, 0, &ctor->fields, &ctor->field_count);
}

/* `and NAME params [: index] := ctor | ...` (S6): the next sort of the
   block that FIRST starts. Only a `family` declaration starts a block; a
   `cofamily` is parsed with its block so that the checker refuses it (S4). */
static int at_and(const Parser *p, const Decl *first) {
  return at(p, TOK_KW_AND) && (first->kind == DECL_FAMILY || first->kind == DECL_COFAMILY) && !first->is_state && !first->is_event;
}

static int parse_and(Parser *p, Decl *d) {
  const Token *tok = cur(p);
  p->def = NULL;
  d->origin = p->origin;
  d->line = tok->line;
  d->col = tok->col;
  d->is_state = 0;
  d->is_event = 0;
  d->is_sealed = 0;
  d->is_law = 0;
  return parse_family(p, d);
}

static int parse_decl(Parser *p, Decl *d) {
  const Token *tok = cur(p);
  p->def = NULL;
  d->origin = p->origin;
  d->line = tok->line;
  d->col = tok->col;
  d->is_state = tok->kind == TOK_KW_STATE;
  d->is_event = tok->kind == TOK_KW_EVENT;
  d->is_sealed = 0;
  d->is_law = 0;
  switch (tok->kind) {
    /* `sealed family` and `law def` (S5): domain-only modifiers; the
       checker refuses them in a program (REFUSE_DATA). */
    case TOK_KW_SEALED:
      bump(p);
      d->is_sealed = 1;
      return at(p, TOK_KW_FAMILY) ? parse_family(p, d) : fail_at(p, cur(p), "'family' after 'sealed'");
    case TOK_KW_LAW:
      bump(p);
      d->is_law = 1;
      return at(p, TOK_KW_DEF) ? parse_def(p, d) : fail_at(p, cur(p), "'def' after 'law'");
    case TOK_KW_DEF: return parse_def(p, d);
    case TOK_KW_AXIOM: return parse_axiom(p, d);
    case TOK_KW_FAMILY:
    case TOK_KW_STATE:
    case TOK_KW_COFAMILY: return parse_family(p, d);
    case TOK_KW_EVENT: return parse_event(p, d);
    case TOK_EOF:
    case TOK_IDENT:
    case TOK_NAT:
    case TOK_WORD:
    case TOK_LPAREN:
    case TOK_RPAREN:
    case TOK_COLON:
    case TOK_DEFINE:
    case TOK_ARROW:
    case TOK_FATARROW:
    case TOK_STAR:
    case TOK_BAR:
    case TOK_AT:
    case TOK_TILDE:
    case TOK_MEET:
    case TOK_JOIN:
    case TOK_LBRACKET:
    case TOK_RBRACKET:
    case TOK_COMMA:
    case TOK_KW_REC:
    case TOK_KW_FUN:
    case TOK_KW_TYPE:
    case TOK_KW_SIGMA:
    case TOK_KW_AND:
      break;
  }
  return fail_at(p, tok, "'def', 'axiom', 'family', 'cofamily', 'state', 'event', 'sealed' or 'law'");
}

int parse_source(Arena *arena, const char *source, Origin origin, const TokenList *toks, DeclList *list, Diag *diag) {
  Parser p = {
    .toks = toks->items, .count = toks->count, .pos = 0, .arena = arena, .diag = diag,
    .source = source, .def = NULL, .origin = origin, .depth = 0,
  };
  Decl *items = list->items;
  size_t count = list->count;
  size_t cap = list->cap;
  while (!at(&p, TOK_EOF)) {
    size_t first = count;
    items = grow(&p, items, count, &cap, sizeof *items);
    if (items == NULL || !parse_decl(&p, &items[count])) return 0;
    items[count].end_line = p.toks[p.pos - 1].line;
    items[count].end_col = p.toks[p.pos - 1].col + (int)p.toks[p.pos - 1].len;
    count++;
    while (at_and(&p, &items[first])) {
      items = grow(&p, items, count, &cap, sizeof *items);
      if (items == NULL || !parse_and(&p, &items[count])) return 0;
      items[count].end_line = p.toks[p.pos - 1].line;
      items[count].end_col = p.toks[p.pos - 1].col + (int)p.toks[p.pos - 1].len;
      count++;
    }
    for (size_t k = first; k < count; k++) {
      items[k].block_first = first;
      items[k].block_count = count - first;
    }
  }
  list->items = items;
  list->count = count;
  list->cap = cap;
  return 1;
}
