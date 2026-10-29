/* The lowering from checked values to the first-order IR. SHARED by the tcc
   kits.

   Each entry is evaluated on variables for its parameters. The normal form
   is the residual program, and this file writes it as IR statements. Each
   value becomes an Operand: a word and a trap flag (see src/ir.h). An
   eliminator on a heap value loads the tag and the fields of the block,
   behind a guard on the trap flag. A fold over a list or a linear family
   walks the spine into a reversed chain, then folds the chain from its last
   node. The chain is a list: a block [1, word, flag, rest, 0]. */
#include "front/lower.h"

#include <stdio.h>
#include <string.h>

#include "front/check.h"

#define IR_PRINT_DEPTH_MAX 4000u

/* A value at run time: its word and its trap flag. Each part is a constant
   or a local. */
typedef struct {
  const IrExpr *w;
  const IrExpr *f;
} Operand;

/* The two locals of a result. */
typedef struct {
  uint32_t w;
  uint32_t f;
} Slot;

typedef struct Buf Buf;

/* The operand of a value lowered earlier: one lowering per Value pointer
   (flag 51). A block finds the entries of its own memo and of the blocks it
   is placed in (flag 52), so a local set in a sibling arm is never read. */
typedef struct {
  const Value *key;
  Operand op;
} Memo;

struct Buf {
  const IrStmt **items;
  size_t count;
  size_t cap;
  const Buf *parent; /* the block this one is placed in, or NULL */
  Memo *memo;        /* open addressing on the Value pointer */
  size_t memo_count;
  size_t memo_cap; /* 0 or a power of two */
};

typedef struct {
  Machine *m;
  uint32_t local_count;
  Operand *levels; /* the operand of each de Bruijn level */
  uint32_t level_count;
  uint32_t level_cap;
} Low;

static int oom(Low *l) {
  return diag_fail(l->m->diag, "OOM", l->m->def, "out of memory");
}

static int internal(Low *l, const char *what) {
  return diag_fail(l->m->diag, "INTERNAL", l->m->def, "the lowering found %s", what);
}

static IrExpr *expr_alloc(Low *l, IrExprKind kind) {
  IrExpr *e = arena_alloc(l->m->arena, sizeof *e);
  if (e == NULL)
    oom(l);
  else
    e->kind = kind;
  return e;
}

static const IrExpr *ex_const(Low *l, uint64_t value) {
  IrExpr *e = expr_alloc(l, IR_EXPR_CONST);
  if (e != NULL)
    e->value = value;
  return e;
}

static const IrExpr *ex_local(Low *l, uint32_t local) {
  IrExpr *e = expr_alloc(l, IR_EXPR_LOCAL);
  if (e != NULL)
    e->local = local;
  return e;
}

static const IrExpr *ex_bin(Low *l, IrOp op, const IrExpr *a, const IrExpr *b) {
  IrExpr *e = a == NULL || b == NULL ? NULL : expr_alloc(l, IR_EXPR_BINARY);
  if (e == NULL)
    return NULL;
  e->op = op;
  e->left = a;
  e->right = b;
  return e;
}

/* Word WORD of the block at ADDR. */
static const IrExpr *ex_load(Low *l, const IrExpr *addr, uint32_t word) {
  IrExpr *e = addr == NULL ? NULL : expr_alloc(l, IR_EXPR_LOAD);
  if (e == NULL)
    return NULL;
  e->left = addr;
  e->field = word;
  return e;
}

static int is_const(const IrExpr *e, uint64_t value) {
  return e != NULL && e->kind == IR_EXPR_CONST && e->value == value;
}

/* The or of two flags. A constant flag folds away. */
static const IrExpr *ex_or(Low *l, const IrExpr *a, const IrExpr *b) {
  if (a == NULL || b == NULL)
    return NULL;
  if (is_const(a, 0) || is_const(b, 1))
    return b;
  if (is_const(b, 0) || is_const(a, 1))
    return a;
  return ex_bin(l, IR_OP_OR, a, b);
}

static Operand operand(const IrExpr *w, const IrExpr *f) {
  Operand o;
  o.w = w;
  o.f = f;
  return o;
}

static int give(Low *l, Operand *out, uint64_t w, uint64_t f) {
  *out = operand(ex_const(l, w), ex_const(l, f));
  return out->w != NULL && out->f != NULL;
}

static Operand trap_value(Low *l) {
  return operand(ex_const(l, 0), ex_const(l, 1));
}

static uint32_t local_new(Low *l) {
  return l->local_count++;
}

static Slot slot_new(Low *l) {
  Slot s;
  s.w = local_new(l);
  s.f = local_new(l);
  return s;
}

static Operand slot_get(Low *l, Slot s) {
  return operand(ex_local(l, s.w), ex_local(l, s.f));
}

/* A new block, placed in PARENT (NULL for the body of an entry). */
static Buf buf_under(const Buf *parent) {
  Buf b;
  memset(&b, 0, sizeof b);
  b.parent = parent;
  return b;
}

/* The slot of KEY in MEMO (CAP entries): its entry, or the free slot
   where it goes. */
static size_t memo_slot(const Memo *memo, size_t cap, const Value *key) {
  size_t i = (((size_t)(uintptr_t)key) >> 4) * 2654435761u & (cap - 1u);
  while (memo[i].key != NULL && memo[i].key != key)
    i = (i + 1u) & (cap - 1u);
  return i;
}

/* 1 and OUT when V was lowered into B or into a block B is placed in. */
static int memo_find(const Buf *b, const Value *v, Operand *out) {
  size_t i;
  for (; b != NULL; b = b->parent) {
    if (b->memo_cap == 0)
      continue;
    i = memo_slot(b->memo, b->memo_cap, v);
    if (b->memo[i].key == v) {
      *out = b->memo[i].op;
      return 1;
    }
  }
  return 0;
}

static int memo_grow(Low *l, Buf *b) {
  size_t cap = b->memo_cap == 0 ? 16u : b->memo_cap * 2u;
  Memo *grown = arena_alloc(l->m->arena, cap * sizeof *grown);
  size_t i;
  if (grown == NULL)
    return oom(l);
  memset(grown, 0, cap * sizeof *grown);
  for (i = 0; i < b->memo_cap; i++)
    if (b->memo[i].key != NULL)
      grown[memo_slot(grown, cap, b->memo[i].key)] = b->memo[i];
  b->memo = grown;
  b->memo_cap = cap;
  return 1;
}

/* Records V as O in the memo of B. The table stays at most half full. */
static int memo_put(Low *l, Buf *b, const Value *v, Operand o) {
  size_t i;
  if (2u * (b->memo_count + 1u) > b->memo_cap && !memo_grow(l, b))
    return 0;
  i = memo_slot(b->memo, b->memo_cap, v);
  if (b->memo[i].key == NULL)
    b->memo_count++;
  b->memo[i].key = v;
  b->memo[i].op = o;
  return 1;
}

/* A value with a lowering worth one memo entry: an operation or a stuck
   operation (flag 54). A number, a type, a variable and a closure are
   constants, a level or a diagnostic. */
static int memo_kind(const Value *v) {
  return v->kind == VAL_OP || v->kind == VAL_STUCK;
}

static int buf_grow(Low *l, Buf *b) {
  size_t cap = b->cap == 0 ? 8u : b->cap * 2u;
  const IrStmt **grown = arena_alloc(l->m->arena, cap * sizeof *grown);
  size_t i;
  if (grown == NULL)
    return oom(l);
  for (i = 0; i < b->count; i++)
    grown[i] = b->items[i];
  b->items = grown;
  b->cap = cap;
  return 1;
}

static int emit(Low *l, Buf *b, const IrStmt *s) {
  if (s == NULL || (b->count == b->cap && !buf_grow(l, b)))
    return 0;
  b->items[b->count++] = s;
  return 1;
}

static int splice(Low *l, Buf *b, const Buf *in) {
  size_t i;
  for (i = 0; i < in->count; i++)
    if (!emit(l, b, in->items[i]))
      return 0;
  return 1;
}

static IrBlock block_of(const Buf *b) {
  IrBlock out;
  out.items = b->items;
  out.count = b->count;
  return out;
}

static IrStmt *stmt_new(Low *l, IrStmtKind kind) {
  IrStmt *s = arena_alloc(l->m->arena, sizeof *s);
  if (s == NULL)
    oom(l);
  else
    s->kind = kind;
  return s;
}

/* A statement that reads the expression E. */
static IrStmt *stmt_expr(Low *l, IrStmtKind kind, const IrExpr *e) {
  IrStmt *s = e == NULL ? NULL : stmt_new(l, kind);
  if (s != NULL)
    s->expr = e;
  return s;
}

static int set(Low *l, Buf *b, uint32_t local, const IrExpr *e) {
  IrStmt *s = stmt_expr(l, IR_STMT_SET, e);
  if (s != NULL)
    s->local = local;
  return emit(l, b, s);
}

/* DST := SRC. A flag that reads the word local of DST goes through a
   temporary, so a loop-carried update stays correct. */
static int store(Low *l, Buf *b, Slot dst, Operand src) {
  uint32_t t;
  if (src.w == NULL || src.f == NULL)
    return 0;
  if (src.f->kind == IR_EXPR_LOCAL && src.f->local == dst.w) {
    t = local_new(l);
    return set(l, b, t, src.f) && set(l, b, dst.w, src.w) && set(l, b, dst.f, ex_local(l, t));
  }
  return set(l, b, dst.w, src.w) && set(l, b, dst.f, src.f);
}

/* E as a constant or a local. */
static const IrExpr *atom(Low *l, Buf *b, const IrExpr *e) {
  uint32_t t;
  if (e == NULL || e->kind == IR_EXPR_CONST || e->kind == IR_EXPR_LOCAL)
    return e;
  t = local_new(l);
  return set(l, b, t, e) ? ex_local(l, t) : NULL;
}

static int atoms(Low *l, Buf *b, const IrExpr *w, const IrExpr *f, Operand *out) {
  out->w = atom(l, b, w);
  out->f = atom(l, b, f);
  return out->w != NULL && out->f != NULL;
}

static int st_if(Low *l, Buf *b, const IrExpr *c, const Buf *yes, const Buf *no) {
  IrStmt *s = stmt_expr(l, IR_STMT_IF, c);
  if (s == NULL)
    return 0;
  s->body = block_of(yes);
  s->otherwise = block_of(no);
  return emit(l, b, s);
}

/* IF F {YES} ELSE {NO}. A constant F keeps one block. */
static int if_flag(Low *l, Buf *b, const IrExpr *f, const Buf *yes, const Buf *no) {
  if (f == NULL)
    return 0;
  if (is_const(f, 0) || is_const(f, 1))
    return splice(l, b, is_const(f, 1) ? yes : no);
  return st_if(l, b, f, yes, no);
}

/* Runs IN when the trap flag F is 0. When F is 1, RES gets the trap. */
static int guard(Low *l, Buf *b, const IrExpr *f, Slot res, const Buf *in) {
  Buf trap = buf_under(b);
  return store(l, &trap, res, trap_value(l)) && if_flag(l, b, f, &trap, in);
}

static int st_switch(Low *l, Buf *b, const IrExpr *e, const IrBlock *arms, size_t n) {
  IrStmt *s = stmt_expr(l, IR_STMT_SWITCH, e);
  if (s == NULL)
    return 0;
  s->arms = arms;
  s->arm_count = n;
  return emit(l, b, s);
}

/* REPEAT COUNT {BODY}, with a new counter local. */
static int st_repeat(Low *l, Buf *b, const IrExpr *count, const Buf *body) {
  IrStmt *s = stmt_expr(l, IR_STMT_REPEAT, count);
  if (s == NULL)
    return 0;
  s->counter = local_new(l);
  s->body = block_of(body);
  return emit(l, b, s);
}

/* WHILE COUNT, LOCAL {BODY}, with a new counter local. */
static int st_while(Low *l, Buf *b, const IrExpr *count, uint32_t local, const Buf *body) {
  IrStmt *s = stmt_expr(l, IR_STMT_WHILE, count);
  if (s == NULL)
    return 0;
  s->counter = local_new(l);
  s->local = local;
  s->body = block_of(body);
  return emit(l, b, s);
}

/* A new block [TAG, the word and the flag of each of the N operands]. */
static int block_ops(Low *l, Buf *b, uint64_t tag, const Operand *ops, uint32_t n, Operand *out) {
  const IrExpr **fields = arena_alloc(l->m->arena, (2u * (size_t)n + 1u) * sizeof *fields);
  IrStmt *s = stmt_new(l, IR_STMT_ALLOC);
  uint32_t i;
  if (fields == NULL || s == NULL)
    return fields == NULL ? oom(l) : 0;
  fields[0] = ex_const(l, tag);
  for (i = 0; i < n; i++) {
    fields[1u + 2u * i] = ops[i].w;
    fields[2u + 2u * i] = ops[i].f;
  }
  s->local = local_new(l);
  s->fields = fields;
  s->field_count = 2u * (size_t)n + 1u;
  *out = operand(ex_local(l, s->local), ex_const(l, 0));
  return fields[0] != NULL && emit(l, b, s);
}

/* The operand of field K of the block at ADDR. */
static Operand field_op(Low *l, const IrExpr *addr, uint32_t k) {
  return operand(ex_load(l, addr, 1u + 2u * k), ex_load(l, addr, 2u + 2u * k));
}

/* Field K of the block at ADDR, copied into two new locals. */
static int field_at(Low *l, Buf *b, const IrExpr *addr, uint32_t k, Operand *out) {
  Slot s = slot_new(l);
  *out = slot_get(l, s);
  return store(l, b, s, field_op(l, addr, k));
}

static int levels_grow(Low *l) {
  uint32_t cap = l->level_cap == 0 ? 64u : l->level_cap * 2u;
  Operand *grown = arena_alloc(l->m->arena, (size_t)cap * sizeof *grown);
  uint32_t i;
  if (grown == NULL)
    return oom(l);
  for (i = 0; i < l->level_count; i++)
    grown[i] = l->levels[i];
  l->levels = grown;
  l->level_cap = cap;
  return 1;
}

/* A new variable that stands for the operand O. Levels never repeat. */
static const Value *fresh(Low *l, Operand o) {
  if (o.w == NULL || o.f == NULL || (l->level_count == l->level_cap && !levels_grow(l)))
    return NULL;
  l->levels[l->level_count] = o;
  return val_var(l->m, l->level_count++);
}

static const char *op_word(IrOp op) {
  switch (op) {
  case IR_OP_ADD:
    return "add";
  case IR_OP_SUB:
    return "sub";
  case IR_OP_MUL:
    return "mul";
  case IR_OP_EQ:
    return "eq";
  case IR_OP_LE:
    return "le";
  case IR_OP_ADD_CARRY:
    return "add_carry";
  case IR_OP_MUL_CARRY:
    return "mul_carry";
  case IR_OP_OR:
    return "or";
  case IR_OP_LT:
    return "lt";
  case IR_OP_DIV:
    return "div";
  case IR_OP_MOD:
    return "mod";
  }
  return "?";
}

static const char *call_word(IrCall call) {
  switch (call) {
  case IR_CALL_U256_ADD:
    return "u256_add";
  case IR_CALL_U256_SUB:
    return "u256_sub";
  case IR_CALL_U256_LE:
    return "u256_le";
  case IR_CALL_U256_EQ:
    return "u256_eq";
  case IR_CALL_TO_U256:
    return "to_u256";
  case IR_CALL_SHA256:
    return "sha256";
  case IR_CALL_FE_ADD:
    return "fe_add";
  case IR_CALL_FE_SUB:
    return "fe_sub";
  case IR_CALL_FE_MUL:
    return "fe_mul";
  case IR_CALL_FE_INV:
    return "fe_inv";
  case IR_CALL_EC_ADD:
    return "ec_add";
  case IR_CALL_EC_MUL:
    return "ec_mul";
  case IR_CALL_SCHNORR_OK:
    return "schnorr_ok";
  }
  return "?";
}

static void expr_print(const IrExpr *e, FILE *out, unsigned depth) {
  size_t i;
  if (depth > IR_PRINT_DEPTH_MAX) {
    fputs("...", out);
    return;
  }
  switch (e->kind) {
  case IR_EXPR_CONST:
    fprintf(out, "%llu", (unsigned long long)e->value);
    return;
  case IR_EXPR_LOCAL:
    fprintf(out, "l%u", (unsigned)e->local);
    return;
  case IR_EXPR_BINARY:
    fprintf(out, "%s(", op_word(e->op));
    expr_print(e->left, out, depth + 1u);
    fputs(", ", out);
    expr_print(e->right, out, depth + 1u);
    fputs(")", out);
    return;
  case IR_EXPR_LOAD:
    fputs("load(", out);
    expr_print(e->left, out, depth + 1u);
    fprintf(out, ", %u)", (unsigned)e->field);
    return;
  case IR_EXPR_CALL:
    fprintf(out, "call %s(", call_word(e->call));
    for (i = 0; i < e->arg_count; i++) {
      fputs(i == 0 ? "" : ", ", out);
      expr_print(e->args[i], out, depth + 1u);
    }
    fputs(")", out);
    return;
  }
}

static void block_print(const IrBlock *b, FILE *out, unsigned depth);

static void indent(FILE *out, unsigned depth) {
  fprintf(out, "%*s", (int)(2u * depth), "");
}

static void close_print(FILE *out, unsigned depth) {
  indent(out, depth);
  fputs("}\n", out);
}

static void stmt_print(const IrStmt *s, FILE *out, unsigned depth) {
  size_t i;
  indent(out, depth);
  switch (s->kind) {
  case IR_STMT_SET:
    fprintf(out, "l%u := ", (unsigned)s->local);
    expr_print(s->expr, out, 0);
    fputs("\n", out);
    return;
  case IR_STMT_ALLOC:
    fprintf(out, "l%u := alloc [", (unsigned)s->local);
    for (i = 0; i < s->field_count; i++) {
      fputs(i == 0 ? "" : ", ", out);
      expr_print(s->fields[i], out, 0);
    }
    fputs("]\n", out);
    return;
  case IR_STMT_IF:
    fputs("if ", out);
    expr_print(s->expr, out, 0);
    fputs(" {\n", out);
    block_print(&s->body, out, depth + 1u);
    indent(out, depth);
    fputs("} else {\n", out);
    block_print(&s->otherwise, out, depth + 1u);
    close_print(out, depth);
    return;
  case IR_STMT_SWITCH:
    fputs("switch ", out);
    expr_print(s->expr, out, 0);
    fputs(" {\n", out);
    for (i = 0; i < s->arm_count; i++) {
      indent(out, depth);
      fprintf(out, "arm %zu:\n", i);
      block_print(&s->arms[i], out, depth + 1u);
    }
    close_print(out, depth);
    return;
  case IR_STMT_REPEAT:
    fprintf(out, "repeat l%u := ", (unsigned)s->counter);
    expr_print(s->expr, out, 0);
    fputs(" {\n", out);
    block_print(&s->body, out, depth + 1u);
    close_print(out, depth);
    return;
  case IR_STMT_WHILE:
    fprintf(out, "while l%u := ", (unsigned)s->counter);
    expr_print(s->expr, out, 0);
    fprintf(out, ", l%u {\n", (unsigned)s->local);
    block_print(&s->body, out, depth + 1u);
    close_print(out, depth);
    return;
  case IR_STMT_TRAP:
    fputs("trap\n", out);
    return;
  case IR_STMT_RETURN:
    fputs("return ", out);
    expr_print(s->expr, out, 0);
    fputs("\n", out);
    return;
  }
}

static void block_print(const IrBlock *b, FILE *out, unsigned depth) {
  size_t i;
  if (depth > IR_PRINT_DEPTH_MAX) {
    fputs("...\n", out);
    return;
  }
  for (i = 0; i < b->count; i++)
    stmt_print(b->items[i], out, depth);
}

static const char *scalar_word(IrScalar s) {
  switch (s) {
  case IR_SCALAR_NAT:
    return "nat";
  case IR_SCALAR_FLAG:
    return "flag";
  case IR_SCALAR_U256:
    return "u256";
  }
  return "?";
}

void ir_print(const IrProgram *prog, FILE *out) {
  size_t i;
  size_t k;
  for (i = 0; i < prog->func_count; i++) {
    const IrFunc *fn = &prog->funcs[i];
    fprintf(out, "func %s (", fn->name);
    for (k = 0; k < fn->param_count; k++)
      fprintf(out, "%s%s", k == 0 ? "" : ", ", scalar_word(fn->params[k]));
    fprintf(out, ") -> %s, %u locals\n", scalar_word(fn->result), (unsigned)fn->local_count);
    block_print(&fn->body, out, 1u);
  }
}

static int lower(Low *l, Buf *b, const Value *v, Operand *out);

static int closure(Low *l) {
  return diag_fail(l->m->diag, "REFUSE_RUNTIME_CLOSURE", l->m->def, "a function value must exist at run time (HOST-LIMIT)");
}

/* 1 when the evaluator gave a value, else 0 after a diagnostic. */
static int got(Low *l, const Value *v) {
  return v != NULL ? 1 : (l->m->diag->set ? 0 : internal(l, "a missing value"));
}

/* RES := the lowering of V. */
static int lower_into(Low *l, Buf *b, const Value *v, Slot res) {
  Operand o;
  return lower(l, b, v, &o) && store(l, b, res, o);
}

/* RES := FN applied to N new variables, one for each operand of ARGS. */
static int apply_into(Low *l, Buf *b, const Value *fn, const Operand *args, uint32_t n, Slot res) {
  const Value *v = fn;
  const Value *x;
  uint32_t i;
  for (i = 0; i < n && v != NULL; i++) {
    x = fresh(l, args[i]);
    v = x == NULL ? NULL : apply_value(l->m, v, x);
  }
  return got(l, v) && lower_into(l, b, v, res);
}

/* A new block [TAG, the lowering of each of the N values of ARGS]. */
static int ctor_block(Low *l, Buf *b, uint64_t tag, const Value *const *args, uint32_t n, Operand *out) {
  Operand *ops = arena_alloc(l->m->arena, ((size_t)n + 1u) * sizeof *ops);
  uint32_t i;
  if (ops == NULL)
    return oom(l);
  for (i = 0; i < n; i++)
    if (!lower(l, b, args[i], &ops[i]))
      return 0;
  return block_ops(l, b, tag, ops, n, out);
}

/* A constructor with fewer arguments than its arity is a function. */
static int ctor_full(Low *l, Buf *b, const Value *v, uint32_t skip, uint32_t n, uint64_t tag, Operand *out) {
  return v->argc < skip + n ? closure(l) : ctor_block(l, b, tag, v->args + skip, n, out);
}

/* The number of recursive fields of C. */
static uint32_t rec_count(const CtorInfo *c) {
  uint32_t n = 0;
  uint32_t k;
  for (k = 0; k < c->field_count; k++)
    n += c->fields[k].recursive ? 1u : 0u;
  return n;
}

/* 1 when X is a point constructor value without a recursive field (a
   leaf, as fmEta and fmUnit). */
static int static_leaf(const Low *l, const Value *x) {
  return x != NULL && (x->kind == VAL_OP || x->kind == VAL_STUCK) && x->op == OP_CTOR && l->m->ctors[x->inst].interval_count == 0u &&
         rec_count(&l->m->ctors[x->inst]) == 0u;
}

/* A binary node (cairn-lang S8, Q-S8-6, Q-S8-10): a point constructor with
   the 2 recursive fields L < R lowers when one side is a static leaf. A
   static leaf at L gives the constructor tag (the walk goes to R); a static
   leaf at R gives the pseudo tag ctor_count (the walk goes to L). Else the
   value is a tree at run time. */
static int lower_ctor(Low *l, Buf *b, const Value *v, Operand *out) {
  const CtorInfo *c = &l->m->ctors[v->inst];
  const FamilyInfo *fam = &l->m->families[c->family];
  const Value *const *f = v->args + fam->param_count;
  uint32_t side[2] = {0u, 0u};
  uint32_t n = 0;
  uint32_t k;
  if (c->interval_count != 0u || rec_count(c) != 2u || v->argc < fam->param_count + c->field_count)
    return ctor_full(l, b, v, fam->param_count, c->field_count, v->inst - fam->first_ctor, out);
  for (k = 0; k < c->field_count; k++)
    if (c->fields[k].recursive)
      side[n++] = k;
  return static_leaf(l, f[side[0]])   ? ctor_block(l, b, v->inst - fam->first_ctor, f, c->field_count, out)
         : static_leaf(l, f[side[1]]) ? ctor_block(l, b, fam->ctor_count, f, c->field_count, out)
                                      : diag_fail(l->m->diag, "REFUSE_RUNTIME_TREE", l->m->def,
                                                  "a run-time %s without a static leaf side is a tree of %s (HOST-LIMIT)", c->name, fam->name);
}

/* An operation with no form on the target (cairn-lang S8, Q-S8-1, Q-S8-5). */
static int refuse(Low *l, const char *what) {
  return diag_fail(l->m->diag, "REFUSE_LOWER", l->m->def, "%s does not lower (HOST-LIMIT)", what);
}

/* An operation on a secret key or a witness (the zk key, sign, witness, cast
   and prove operations) has no form on any target: a module is public
   (cairn-lang S9, Q-S9-3, PLAN O6). */
static int secret_op(Low *l) {
  return diag_fail(l->m->diag, "REFUSE_TARGET_SECRET", l->m->def, "the operation touches a secret key or a witness (O6)");
}

/* The prelude function CALL applied to the N words of ARGS. */
static const IrExpr *ex_call(Low *l, IrCall call, const IrExpr *const *args, size_t n) {
  IrExpr *e = expr_alloc(l, IR_EXPR_CALL);
  if (e == NULL)
    return NULL;
  e->call = call;
  e->args = args;
  e->arg_count = n;
  return e;
}

/* The constant U256 (or Addr) X: a new block of its 4 limbs, the low limb
   first (src/ir.h). */
static int word_block(Low *l, Buf *b, const U256 *x, Operand *out) {
  const IrExpr **fields = arena_alloc(l->m->arena, 4u * sizeof *fields);
  IrStmt *s = stmt_new(l, IR_STMT_ALLOC);
  uint32_t k;
  if (fields == NULL)
    return oom(l);
  for (k = 0; k < 4u; k++)
    fields[k] = ex_const(l, x->limb[k]);
  if (s == NULL || fields[0] == NULL || fields[1] == NULL || fields[2] == NULL || fields[3] == NULL)
    return 0;
  s->local = local_new(l);
  s->fields = fields;
  s->field_count = 4u;
  *out = operand(ex_local(l, s->local), ex_const(l, 0));
  return out->w != NULL && out->f != NULL && emit(l, b, s);
}

static int lower_stuck(Low *l, Buf *b, const Value *v, Operand *out);

/* A canonical value. An eliminator here has too few arguments, so it is a
   function. */
static int lower_canon(Low *l, Buf *b, const Value *v, Operand *out) {
  switch (v->op) {
  case OP_NAT:
  case OP_FLAG:
  case OP_UNIT:
  case OP_PROD:
  case OP_SUM:
  case OP_OPTION:
  case OP_LIST:
  case OP_FAMILY:
  case OP_UNIT_VAL:
  case OP_FLAG_NO:
  case OP_U256:
  case OP_ADDR:
  case OP_INTERVAL:
  case OP_SYSTEM:
  case OP_FIN:
  case OP_NAT_IS_SET:
  case OP_U256_IS_SET:
  case OP_FLAG_IS_SET:
    return give(l, out, 0, 0);
  case OP_FLAG_YES:
    return give(l, out, 1, 0);
  case OP_PAIR:
  case OP_PACK:
    return ctor_full(l, b, v, 0, 2, 0, out);
  case OP_INL:
    return ctor_full(l, b, v, 0, 1, 0, out);
  case OP_INR:
  case OP_SOME:
    return ctor_full(l, b, v, 0, 1, 1, out);
  case OP_NONE:
  case OP_NIL:
    return ctor_full(l, b, v, 0, 0, 0, out);
  case OP_CONS:
    return ctor_full(l, b, v, 0, 2, 1, out);
  case OP_CTOR:
    return lower_ctor(l, b, v, out);
  case OP_FIRST:
  case OP_SECOND:
  case OP_EITHER:
  case OP_OPTION_ELIM:
  case OP_PURE:
  case OP_MAP:
  case OP_BIND:
  case OP_FOLD_NAT:
  case OP_FOLD_LIST:
  case OP_FOLD_FAMILY:
  case OP_UNFOLD:
  case OP_FILTER:
  case OP_WITNESS:
  case OP_PAYLOAD:
  case OP_ELIM:
  case OP_NAT_ADD:
  case OP_NAT_SUB:
  case OP_NAT_EQ:
  case OP_NAT_LE:
  case OP_FLAG_IF:
  case OP_U256_ADD:
  case OP_U256_SUB:
  case OP_U256_LE:
  case OP_U256_EQ:
  case OP_TO_U256:
  case OP_ADDR_EQ:
  case OP_NAT_DIV:
  case OP_NAT_MOD:
  case OP_HASH:
  case OP_ADDR_TO_U256:
  case OP_PROJ:
  case OP_FIN_ADD:
  case OP_FIN_NEG:
  case OP_FIN_CASE:
  case OP_EC_ADD:
  case OP_EC_MUL:
  case OP_SCHNORR_OK:
    return closure(l);
  /* No form on the target: the refusals of lower_stuck. */
  case OP_TRANSP:
  case OP_HCOMP:
  case OP_GLUE:
  case OP_GLUE_ELEM:
  case OP_UNGLUE:
  case OP_KMAP:
  case OP_KMAP_OF:
  case OP_KMAP_GET:
  case OP_KMAP_SET:
  case OP_ZK_HP:
  case OP_ZK_KEY:
  case OP_ZK_DIGEST:
  case OP_ZK_SIGN:
  case OP_ZK_VERIFY:
  case OP_ZK_BALLOT:
  case OP_ZK_WITNESS:
  case OP_ZK_CAST:
  case OP_ZK_PROVE:
  case OP_ZK_PROJECT:
  case OP_ZK_CHECK:
    return lower_stuck(l, b, v, out);
  }
  return internal(l, "an unknown operation");
}

/* A Nat operation. CARRY: OP can overflow, and the overflow sets the flag. */
static int nat_bin(Low *l, Buf *b, const Value *v, IrOp op, int carry, Operand *out) {
  Operand x;
  Operand y;
  const IrExpr *f;
  if (!lower(l, b, v->args[0], &x) || !lower(l, b, v->args[1], &y))
    return 0;
  f = ex_or(l, x.f, y.f);
  if (carry)
    f = ex_or(l, f, ex_bin(l, op == IR_OP_ADD ? IR_OP_ADD_CARRY : IR_OP_MUL_CARRY, x.w, y.w));
  return atoms(l, b, ex_bin(l, op, x.w, y.w), f, out);
}

/* Field K of the block X, behind the guard on the flag of X. */
static int field_of(Low *l, Buf *b, const Value *xv, uint32_t k, Operand *out) {
  Operand x;
  Slot res = slot_new(l);
  Buf in = buf_under(b);
  *out = slot_get(l, res);
  return lower(l, b, xv, &x) && store(l, &in, res, field_op(l, x.w, k)) && guard(l, b, x.f, res, &in);
}

static int flag_if(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand c;
  Slot res = slot_new(l);
  Buf yes = buf_under(b);
  Buf no = buf_under(b);
  Buf in = buf_under(b);
  *out = slot_get(l, res);
  return lower(l, b, v->args[0], &c) && lower_into(l, &yes, v->args[1], res) && lower_into(l, &no, v->args[2], res) &&
         if_flag(l, &in, c.w, &yes, &no) && guard(l, b, c.f, res, &in);
}

/* SWITCH on the tag of X, N arms. Arm I applies CASES[I] to the FIELDS[I]
   fields of the block (with 0 fields, the arm lowers CASES[I]). */
static int cases_of(Low *l, Buf *b, const Value *xv, const Value *const *cases, const uint32_t *fields, uint32_t n, Operand *out) {
  IrBlock *arms = arena_alloc(l->m->arena, ((size_t)n + 1u) * sizeof *arms);
  Operand *args;
  Operand x;
  Slot res = slot_new(l);
  Buf in = buf_under(b);
  Buf arm;
  uint32_t i;
  uint32_t k;
  *out = slot_get(l, res);
  if (arms == NULL)
    return oom(l);
  if (!lower(l, b, xv, &x))
    return 0;
  for (i = 0; i < n; i++) {
    arm = buf_under(b);
    args = arena_alloc(l->m->arena, ((size_t)fields[i] + 1u) * sizeof *args);
    if (args == NULL)
      return oom(l);
    for (k = 0; k < fields[i]; k++)
      if (!field_at(l, &arm, x.w, k, &args[k]))
        return 0;
    if (!apply_into(l, &arm, cases[i], args, fields[i], res))
      return 0;
    arms[i] = block_of(&arm);
  }
  return st_switch(l, &in, ex_load(l, x.w, 0), arms, n) && guard(l, b, x.f, res, &in);
}

static int two_cases(Low *l, Buf *b, const Value *v, uint32_t none_fields, Operand *out) {
  uint32_t fields[2];
  fields[0] = none_fields;
  fields[1] = 1;
  return cases_of(l, b, v->args[2], v->args, fields, 2, out);
}

/* The fuel of a walk over a heap spine. */
#define SPINE_FUEL ((uint64_t)1 << 63)

/* A constructor without a recursive field. */
#define NO_REC UINT32_MAX

/* The recursive field of each list tag: nil has none, cons has the tail. */
static const uint32_t LIST_REC[2] = {NO_REC, 1u};

/* The locals of a walk: the reversed chain, its length, the trap flag of
   the end, the node where the walk stopped, and the WHILE condition. */
typedef struct {
  uint32_t rev;
  uint32_t count;
  uint32_t end;
  uint32_t cur;
  uint32_t cont;
} Walk;

/* A fold into B -> B applied to B0 (cairn-lang S8, Q-S8-11): the fold V,
   the recursive field REC of each of the N tags, the binary node BIN, the
   tag NOW whose leaf goes on ACC during the walk and the tag LATER whose
   leaf goes on ACC in the pass. */
typedef struct {
  const Value *v;
  const uint32_t *rec;
  uint32_t n;
  uint32_t bin;
  uint32_t now;
  uint32_t later;
  Slot acc;
} Chain;

static int leaf_at(Low *l, Buf *b, const IrExpr *addr, const Chain *ch, uint32_t i);

static Walk walk_new(Low *l) {
  Walk w;
  w.rev = local_new(l);
  w.count = local_new(l);
  w.end = local_new(l);
  w.cur = local_new(l);
  w.cont = local_new(l);
  return w;
}

/* REV := a new chain cell [1, the word and the flag of OP, REV, 0], and
   COUNT increases by 1. */
static int chain_push(Low *l, Buf *b, const Walk *w, Operand op) {
  Operand ops[2];
  Operand c;
  ops[0] = op;
  ops[1] = operand(ex_local(l, w->rev), ex_const(l, 0));
  return block_ops(l, b, 1, ops, 2u, &c) && set(l, b, w->rev, c.w) &&
         set(l, b, w->count, ex_bin(l, IR_OP_ADD, ex_local(l, w->count), ex_const(l, 1)));
}

/* A walk step at a node whose recursive field is K: push the node, then go
   to field K. A trapped field ends the walk. */
static int walk_step(Low *l, Buf *b, const Walk *w, uint32_t k) {
  const IrExpr *cur = ex_local(l, w->cur);
  return chain_push(l, b, w, operand(cur, ex_const(l, 0))) && set(l, b, w->end, ex_load(l, cur, 2u + 2u * k)) &&
         set(l, b, w->cur, ex_load(l, cur, 1u + 2u * k)) &&
         set(l, b, w->cont, ex_bin(l, IR_OP_EQ, ex_local(l, w->end), ex_const(l, 0)));
}

/* The spine walk of X. A node with tag I has the recursive field REC[I] (or
   NO_REC). A node with a recursive field goes on the chain. The walk stops
   at a node without one (CUR), or at a trapped field (END = 1). The cell of
   the last node comes first. With CH (a fold into B -> B), the leaf of a
   node with the tag NOW goes on ACC before the step. */
static int walk_with(Low *l, Buf *b, Operand x, const uint32_t *rec, uint32_t n, const Chain *ch, Walk *w) {
  IrBlock *arms = arena_alloc(l->m->arena, ((size_t)n + 1u) * sizeof *arms);
  Buf body = buf_under(b);
  Buf arm;
  uint32_t i;
  *w = walk_new(l);
  if (arms == NULL)
    return oom(l);
  for (i = 0; i < n; i++) {
    arm = buf_under(&body);
    if (!(rec[i] == NO_REC ? set(l, &arm, w->cont, ex_const(l, 0))
                           : (ch == NULL || i != ch->now || leaf_at(l, &arm, ex_local(l, w->cur), ch, i)) &&
                                 walk_step(l, &arm, w, rec[i])))
      return 0;
    arms[i] = block_of(&arm);
  }
  return set(l, b, w->rev, ex_const(l, 0)) && set(l, b, w->count, ex_const(l, 0)) && set(l, b, w->end, x.f) &&
         set(l, b, w->cur, x.w) && set(l, b, w->cont, ex_bin(l, IR_OP_EQ, x.f, ex_const(l, 0))) &&
         st_switch(l, &body, ex_load(l, ex_local(l, w->cur), 0), arms, n) &&
         st_while(l, b, ex_const(l, SPINE_FUEL), w->cont, &body);
}

static int walk(Low *l, Buf *b, Operand x, const uint32_t *rec, uint32_t n, Walk *w) {
  return walk_with(l, b, x, rec, n, NULL, w);
}

/* The start of a pass over the chain of W: NODE := word 1 of the cell. */
static int chain_node(Low *l, Buf *body, const Walk *w, const IrExpr **node) {
  uint32_t t = local_new(l);
  *node = ex_local(l, t);
  return set(l, body, t, ex_load(l, ex_local(l, w->rev), 1));
}

/* The start of a pass over a chain of list nodes: H := the head. */
static int chain_head(Low *l, Buf *body, const Walk *w, Operand *h) {
  const IrExpr *node;
  return chain_node(l, body, w, &node) && field_at(l, body, node, 0, h);
}

/* The end of a pass: REV := the next cell, and REPEAT COUNT {BODY}. */
static int chain_end(Low *l, Buf *b, const Walk *w, Buf *body) {
  return set(l, body, w->rev, ex_load(l, ex_local(l, w->rev), 3)) && st_repeat(l, b, ex_local(l, w->count), body);
}

/* OUT := a trap tail when the flag local END is 1, else nil. */
static int tail_init(Low *l, Buf *b, uint32_t end, Slot out) {
  Operand nil;
  Buf in = buf_under(b);
  return block_ops(l, &in, 0, NULL, 0, &nil) && store(l, &in, out, nil) && guard(l, b, ex_local(l, end), out, &in);
}

/* OUT := a new cons cell [1, H, OUT]. */
static int prepend(Low *l, Buf *b, Operand h, Slot out) {
  Operand ops[2];
  Operand c;
  ops[0] = h;
  ops[1] = slot_get(l, out);
  return block_ops(l, b, 1, ops, 2u, &c) && store(l, b, out, c);
}

/* A right fold over a list. F takes the head, then the acc. A trap tail
   gives a trap base acc. */
static int fold_list(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand x;
  Operand args[2];
  Walk w;
  Slot acc = slot_new(l);
  Buf in = buf_under(b);
  Buf body = buf_under(b);
  *out = slot_get(l, acc);
  args[1] = slot_get(l, acc);
  return lower(l, b, v->args[2], &x) && walk(l, b, x, LIST_REC, 2u, &w) && lower_into(l, &in, v->args[1], acc) &&
         guard(l, b, ex_local(l, w.end), acc, &in) && chain_head(l, &body, &w, &args[0]) &&
         apply_into(l, &body, v->args[0], args, 2u, acc) && chain_end(l, b, &w, &body);
}

/* MAP on a list. A trap tail stays a trap tail. */
static int list_map(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand x;
  Operand h;
  Walk w;
  Slot res = slot_new(l);
  Slot ys = slot_new(l);
  Buf body = buf_under(b);
  *out = slot_get(l, res);
  return lower(l, b, v->args[1], &x) && walk(l, b, x, LIST_REC, 2u, &w) && tail_init(l, b, w.end, res) &&
         chain_head(l, &body, &w, &h) && apply_into(l, &body, v->args[0], &h, 1u, ys) &&
         prepend(l, &body, slot_get(l, ys), res) && chain_end(l, b, &w, &body);
}

/* BIND on a list. The pass starts at the last node, so a result of F with
   a trap tail drops the items after it. */
static int list_bind(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand x;
  Operand h;
  Operand y;
  Walk w;
  Walk u;
  Slot res = slot_new(l);
  Slot rs = slot_new(l);
  Buf body = buf_under(b);
  Buf inner = buf_under(&body);
  Buf none = buf_under(&body);
  *out = slot_get(l, res);
  return lower(l, b, v->args[0], &x) && walk(l, b, x, LIST_REC, 2u, &w) && tail_init(l, b, w.end, res) &&
         chain_head(l, &body, &w, &h) && apply_into(l, &body, v->args[1], &h, 1u, rs) &&
         walk(l, &body, slot_get(l, rs), LIST_REC, 2u, &u) && guard(l, &body, ex_local(l, u.end), res, &none) &&
         chain_head(l, &inner, &u, &y) && prepend(l, &inner, y, res) && chain_end(l, &body, &u, &inner) &&
         chain_end(l, b, &w, &body);
}

/* FILTER on a list. The pass starts at the last node, so a trapped test
   drops the items after it: the result is the items kept before the first
   trapped test, then a trap tail. */
static int list_filter(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand x;
  Operand h;
  Walk w;
  Slot res = slot_new(l);
  Slot ts = slot_new(l);
  Buf body = buf_under(b);
  Buf keep = buf_under(&body);
  Buf drop = buf_under(&body);
  Buf test = buf_under(&body);
  *out = slot_get(l, res);
  return lower(l, b, v->args[1], &x) && walk(l, b, x, LIST_REC, 2u, &w) && tail_init(l, b, w.end, res) &&
         chain_head(l, &body, &w, &h) && apply_into(l, &body, v->args[0], &h, 1u, ts) && prepend(l, &keep, h, res) &&
         if_flag(l, &test, ex_local(l, ts.w), &keep, &drop) && guard(l, &body, ex_local(l, ts.f), res, &test) &&
         chain_end(l, b, &w, &body);
}

/* An unfold that ends at a trap: END := 1, and the WHILE stops. */
static int unfold_stop(Low *l, Buf *b, const Walk *w) {
  return set(l, b, w->end, ex_const(l, 1)) && set(l, b, w->cont, ex_const(l, 0));
}

/* Push the item of the pair at P, then SEED := the next seed. */
static int unfold_push(Low *l, Buf *b, const Walk *w, const IrExpr *p, Slot seed) {
  Operand a;
  Operand s;
  return field_at(l, b, p, 0, &a) && field_at(l, b, p, 1, &s) && chain_push(l, b, w, a) && store(l, b, seed, s);
}

/* The WHILE body of an unfold: R := G SEED. None stops; a trap or some(trap)
   stops with END = 1; some(pair) pushes the item. */
static int unfold_step(Low *l, Buf *b, const Walk *w, const Value *g, Slot seed) {
  Operand s = slot_get(l, seed);
  Operand p;
  Slot rs = slot_new(l);
  Operand r = slot_get(l, rs);
  Buf stop = buf_under(b);
  Buf stop_some = buf_under(b);
  Buf push = buf_under(b);
  Buf some = buf_under(b);
  Buf none = buf_under(b);
  Buf tag = buf_under(b);
  return apply_into(l, b, g, &s, 1u, rs) && field_at(l, &some, r.w, 0, &p) && unfold_push(l, &push, w, p.w, seed) &&
         unfold_stop(l, &stop_some, w) && if_flag(l, &some, p.f, &stop_some, &push) &&
         set(l, &none, w->cont, ex_const(l, 0)) && if_flag(l, &tag, ex_load(l, r.w, 0), &some, &none) &&
         unfold_stop(l, &stop, w) && if_flag(l, b, r.f, &stop, &tag);
}

/* UNFOLD G LIMIT SEED. The WHILE pushes the items on a chain, then a pass
   from the last item builds the list. A trap limit gives a trap. */
static int unfold(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand lim;
  Operand h;
  Walk w = walk_new(l);
  Slot res = slot_new(l);
  Slot seed = slot_new(l);
  Buf in = buf_under(b);
  Buf step = buf_under(&in);
  Buf body = buf_under(&in);
  *out = slot_get(l, res);
  return lower(l, b, v->args[1], &lim) && set(l, &in, w.rev, ex_const(l, 0)) && set(l, &in, w.count, ex_const(l, 0)) &&
         set(l, &in, w.end, ex_const(l, 0)) && set(l, &in, w.cont, ex_const(l, 1)) &&
         lower_into(l, &in, v->args[2], seed) && unfold_step(l, &step, &w, v->args[0], seed) &&
         st_while(l, &in, lim.w, w.cont, &step) && tail_init(l, &in, w.end, res) &&
         field_at(l, &body, ex_local(l, w.rev), 0, &h) && prepend(l, &body, h, res) && chain_end(l, &in, &w, &body) &&
         guard(l, b, lim.f, res, &in);
}

/* A fold step at the node ADDR of the constructor C: ACC := FN applied to
   the fields, with ACC for the recursive field R. ELIM (cairn elim, eval.c
   elim_family): the recursive field and then ACC, and a path field and then
   its erased fold. */
static int fam_case(Low *l, Buf *b, const IrExpr *addr, const Value *fn, const CtorInfo *c, uint32_t r, int elim, Slot acc) {
  Operand *args = arena_alloc(l->m->arena, (2u * (size_t)c->field_count + 1u) * sizeof *args);
  uint32_t n = 0;
  uint32_t k;
  if (args == NULL)
    return oom(l);
  for (k = 0; k < c->field_count; k++) {
    if ((k != r || elim) && !field_at(l, b, addr, k, &args[n]))
      return 0;
    n += k != r || elim ? 1u : 0u;
    if (k == r)
      args[n++] = slot_get(l, acc);
    if (elim && c->fields[k].path)
      args[n++] = operand(ex_const(l, 0), ex_const(l, 0));
  }
  return apply_into(l, b, fn, args, n, acc);
}

/* The index of the first binary node of FAM (a point constructor with 2
   recursive fields), or NO_REC. */
static uint32_t bin_of(const Low *l, const FamilyInfo *fam) {
  const CtorInfo *c;
  uint32_t i;
  for (i = 0; i < fam->ctor_count; i++) {
    c = &l->m->ctors[fam->first_ctor + i];
    if (c->interval_count == 0u && rec_count(c) == 2u)
      return i;
  }
  return NO_REC;
}

static int fam_switch(Low *l, Buf *b, const IrExpr *addr, const Value *v, const uint32_t *rec, uint32_t n, int rec_arms, Slot acc);

/* A fold step at the binary node ADDR (constructor I of the fold V,
   cairn-lang S8, Q-S8-6): ACC := the case of I applied to the fields, with
   ACC for the recursive field R and, for the other recursive field (a
   static leaf), its fold by the leaf arms of V. */
static int bin_case(Low *l, Buf *b, const IrExpr *addr, const Value *v, const uint32_t *rec, uint32_t n, uint32_t i, uint32_t r, Slot acc) {
  const FamilyInfo *fam = &l->m->families[v->inst];
  const CtorInfo *c = &l->m->ctors[fam->first_ctor + i];
  Operand *args = arena_alloc(l->m->arena, ((size_t)c->field_count + 1u) * sizeof *args);
  Operand lf = operand(ex_const(l, 0), ex_const(l, 0));
  uint32_t t = local_new(l);
  Slot leaf = slot_new(l);
  Buf in = buf_under(b);
  uint32_t k;
  if (args == NULL)
    return oom(l);
  for (k = 0; k < c->field_count; k++) {
    if (k != r && !field_at(l, b, addr, k, &args[k]))
      return 0;
    lf = k != r && c->fields[k].recursive ? args[k] : lf;
    args[k] = k == r ? slot_get(l, acc) : (c->fields[k].recursive ? slot_get(l, leaf) : args[k]);
  }
  return set(l, b, t, lf.w) && fam_switch(l, &in, ex_local(l, t), v, rec, n, 0, leaf) && guard(l, b, lf.f, leaf, &in) &&
         apply_into(l, b, v->args[i], args, c->field_count, acc);
}

/* SWITCH on the tag of the node at ADDR into ACC, with N arms (arm
   ctor_count is the pseudo tag of a binary node). When REC_ARMS is 1, the
   arms of the constructors with a recursive field apply their case (else
   the arms of the ones without). The other arms are not reached; they give
   a trap. A path constructor has no run-time value: its arm is a trap. */
static int fam_switch(Low *l, Buf *b, const IrExpr *addr, const Value *v, const uint32_t *rec, uint32_t n, int rec_arms, Slot acc) {
  const FamilyInfo *fam = &l->m->families[v->inst];
  IrBlock *arms = arena_alloc(l->m->arena, ((size_t)n + 1u) * sizeof *arms);
  const CtorInfo *c;
  Buf arm;
  uint32_t ci;
  uint32_t i;
  if (arms == NULL)
    return oom(l);
  for (i = 0; i < n; i++) {
    arm = buf_under(b);
    ci = i < fam->ctor_count ? i : bin_of(l, fam);
    c = &l->m->ctors[fam->first_ctor + ci];
    if (!(c->interval_count == 0u && (rec[i] != NO_REC) == rec_arms
              ? (rec_count(c) == 2u ? bin_case(l, &arm, addr, v, rec, n, ci, rec[i], acc)
                                    : fam_case(l, &arm, addr, v->args[ci], c, rec[i], v->op == OP_ELIM, acc))
              : store(l, &arm, acc, trap_value(l))))
      return 0;
    arms[i] = block_of(&arm);
  }
  return st_switch(l, b, ex_load(l, addr, 0), arms, n);
}

/* A fold over a linear family: the walk, the base node into ACC, then a
   pass from the last recursive node with ACC for its recursive field. N is
   the arm count (the pseudo tag of a binary node adds 1). */
static int family_spine(Low *l, Buf *b, const Value *v, const uint32_t *rec, uint32_t n, Operand *out) {
  const IrExpr *node;
  Operand x;
  Walk w;
  Slot acc = slot_new(l);
  Buf in = buf_under(b);
  Buf body = buf_under(b);
  *out = slot_get(l, acc);
  return lower(l, b, v->args[v->argc - 1u], &x) && walk(l, b, x, rec, n, &w) &&
         fam_switch(l, &in, ex_local(l, w.cur), v, rec, n, 0, acc) && guard(l, b, ex_local(l, w.end), acc, &in) &&
         chain_node(l, &body, &w, &node) && fam_switch(l, &body, node, v, rec, n, 1, acc) && chain_end(l, b, &w, &body);
}

/* A fold over a family without a recursive field is a case split. With at
   most one recursive field in each point constructor, it is a spine walk.
   One binary node (cairn-lang S8, Q-S8-6) walks too: a static leaf side
   (lower_ctor) gives the constructor tag (REC = R) or the pseudo tag
   ctor_count (REC = L). A path constructor stops the walk. ELIM over a
   binary node refuses. */
static int family_fold(Low *l, Buf *b, const Value *v, Operand *out) {
  const FamilyInfo *fam = &l->m->families[v->inst];
  uint32_t *fields = arena_alloc(l->m->arena, ((size_t)fam->ctor_count + 1u) * sizeof *fields);
  uint32_t *recs = arena_alloc(l->m->arena, ((size_t)fam->ctor_count + 1u) * sizeof *recs);
  const CtorInfo *c;
  uint32_t bin = bin_of(l, fam);
  uint32_t rec = 0;
  uint32_t here;
  uint32_t i;
  uint32_t k;
  if (fields == NULL || recs == NULL)
    return oom(l);
  recs[fam->ctor_count] = NO_REC;
  for (i = 0; i < fam->ctor_count; i++) {
    c = &l->m->ctors[fam->first_ctor + i];
    here = rec_count(c);
    recs[i] = NO_REC;
    for (k = 0; k < c->field_count; k++) {
      recs[fam->ctor_count] = i == bin && c->fields[k].recursive && recs[i] == NO_REC ? k : recs[fam->ctor_count];
      recs[i] = c->fields[k].recursive ? k : recs[i];
    }
    recs[i] = c->interval_count != 0u ? NO_REC : recs[i];
    if (c->interval_count == 0u && here > (i == bin && v->op != OP_ELIM ? 2u : 1u))
      return diag_fail(l->m->diag, "REFUSE_RUNTIME_TREE", l->m->def, "a fold over a tree of %s must exist at run time (HOST-LIMIT)", fam->name);
    rec += here;
    fields[i] = c->field_count;
  }
  return rec != 0 ? family_spine(l, b, v, recs, bin != NO_REC ? fam->ctor_count + 1u : fam->ctor_count, out)
                  : cases_of(l, b, v->args[v->argc - 1u], v->args, fields, fam->ctor_count, out);
}

/* ACC := the leaf case of the node at ADDR (a point constructor without a
   recursive field) applied to its fields, then to ACC. The other arms are
   not reached; they give a trap. */
static int leaf_step(Low *l, Buf *b, const IrExpr *addr, const Chain *ch) {
  const FamilyInfo *fam = &l->m->families[ch->v->inst];
  IrBlock *arms = arena_alloc(l->m->arena, ((size_t)ch->n + 1u) * sizeof *arms);
  const CtorInfo *c;
  Operand *args;
  Buf arm;
  int live;
  uint32_t i;
  uint32_t k;
  if (arms == NULL)
    return oom(l);
  for (i = 0; i < ch->n; i++) {
    arm = buf_under(b);
    c = &l->m->ctors[fam->first_ctor + (i < fam->ctor_count ? i : ch->bin)];
    live = i < fam->ctor_count && c->interval_count == 0u && rec_count(c) == 0u;
    args = arena_alloc(l->m->arena, ((size_t)c->field_count + 1u) * sizeof *args);
    if (args == NULL)
      return oom(l);
    for (k = 0; live && k < c->field_count; k++)
      if (!field_at(l, &arm, addr, k, &args[k]))
        return 0;
    args[c->field_count] = slot_get(l, ch->acc);
    if (!(live ? apply_into(l, &arm, ch->v->args[i], args, c->field_count + 1u, ch->acc)
               : store(l, &arm, ch->acc, trap_value(l))))
      return 0;
    arms[i] = block_of(&arm);
  }
  return st_switch(l, b, ex_load(l, addr, 0), arms, ch->n);
}

/* ACC := the leaf side of the binary node at ADDR (tag I) applied to ACC:
   the field L for the constructor tag, the field R for the pseudo tag. */
static int leaf_at(Low *l, Buf *b, const IrExpr *addr, const Chain *ch, uint32_t i) {
  const FamilyInfo *fam = &l->m->families[ch->v->inst];
  uint32_t k = ch->rec[i] == ch->rec[fam->ctor_count] ? ch->rec[ch->bin] : ch->rec[fam->ctor_count];
  uint32_t t = local_new(l);
  Operand lf;
  Buf in = buf_under(b);
  return field_at(l, b, addr, k, &lf) && set(l, b, t, lf.w) && leaf_step(l, &in, ex_local(l, t), ch) &&
         guard(l, b, lf.f, ch->acc, &in);
}

/* A pass step at NODE: the leaf of a node with the tag LATER goes on ACC;
   the other arms are empty. */
static int chain_pass(Low *l, Buf *b, const IrExpr *node, const Chain *ch) {
  IrBlock *arms = arena_alloc(l->m->arena, ((size_t)ch->n + 1u) * sizeof *arms);
  Buf arm;
  uint32_t i;
  if (arms == NULL)
    return oom(l);
  for (i = 0; i < ch->n; i++) {
    arm = buf_under(b);
    if (i == ch->later && !leaf_at(l, &arm, node, ch, i))
      return 0;
    arms[i] = block_of(&arm);
  }
  return st_switch(l, b, ex_load(l, node, 0), arms, ch->n);
}

/* 1 when X is the variable Y. */
static int var_is(const Value *x, const Value *y) {
  return x != NULL && x->kind == VAL_VAR && x->nat == y->nat;
}

/* 1 when X is the application F (G D) of three variables. */
static int app2(const Value *x, const Value *f, const Value *g, const Value *d) {
  return x != NULL && x->kind == VAL_APP && var_is(x->dom, f) && x->arg != NULL && x->arg->kind == VAL_APP &&
         var_is(x->arg->dom, g) && var_is(x->arg->arg, d);
}

/* A fold over a family with one binary node into B -> B, applied to B0
   (cairn-lang S8, Q-S8-11): a loop on B. The binary case on new variables
   G, H, then D must be H (G D) (fmChain: the left side first) or G (H D)
   (fmRun: the right side first); else REFUSE_RUNTIME_CLOSURE, as for any
   other application. ACC := B0, then each leaf case on ACC: the leaves
   that go first during the walk (outer node first), the base node, then
   the others in the pass from the innermost node. */
static int chain_fold(Low *l, Buf *b, const Value *app, Operand *out) {
  const Value *v = app->dom;
  const FamilyInfo *fam;
  const CtorInfo *c;
  const Value *g;
  const Value *h;
  const Value *d;
  const Value *x;
  const IrExpr *node;
  uint32_t *recs;
  Chain ch;
  Operand t;
  Walk w;
  Buf in = buf_under(b);
  Buf body = buf_under(b);
  uint32_t i;
  uint32_t k;
  if (v == NULL || v->kind != VAL_STUCK || v->op != OP_FOLD_FAMILY)
    return closure(l);
  fam = &l->m->families[v->inst];
  ch.bin = bin_of(l, fam);
  if (ch.bin == NO_REC)
    return closure(l);
  recs = arena_alloc(l->m->arena, ((size_t)fam->ctor_count + 1u) * sizeof *recs);
  if (recs == NULL)
    return oom(l);
  for (i = 0; i < fam->ctor_count; i++) {
    c = &l->m->ctors[fam->first_ctor + i];
    recs[i] = NO_REC;
    if (c->interval_count == 0u && i != ch.bin && rec_count(c) != 0u)
      return closure(l);
  }
  c = &l->m->ctors[fam->first_ctor + ch.bin];
  recs[fam->ctor_count] = NO_REC;
  for (k = 0; k < c->field_count; k++) {
    recs[fam->ctor_count] = c->fields[k].recursive && recs[fam->ctor_count] == NO_REC ? k : recs[fam->ctor_count];
    recs[ch.bin] = c->fields[k].recursive ? k : recs[ch.bin];
  }
  g = fresh(l, operand(ex_const(l, 0), ex_const(l, 0)));
  h = fresh(l, operand(ex_const(l, 0), ex_const(l, 0)));
  d = fresh(l, operand(ex_const(l, 0), ex_const(l, 0)));
  x = g == NULL || h == NULL || d == NULL ? NULL : apply_value(l->m, v->args[ch.bin], g);
  x = x == NULL ? NULL : apply_value(l->m, x, h);
  x = x == NULL ? NULL : apply_value(l->m, x, d);
  if (!got(l, x))
    return 0;
  if (!app2(x, h, g, d) && !app2(x, g, h, d))
    return closure(l);
  ch.v = v;
  ch.rec = recs;
  ch.n = fam->ctor_count + 1u;
  ch.now = app2(x, h, g, d) ? ch.bin : fam->ctor_count;
  ch.later = app2(x, h, g, d) ? fam->ctor_count : ch.bin;
  ch.acc = slot_new(l);
  *out = slot_get(l, ch.acc);
  return lower_into(l, b, app->arg, ch.acc) && lower(l, b, v->args[v->argc - 1u], &t) &&
         walk_with(l, b, t, recs, ch.n, &ch, &w) && leaf_step(l, &in, ex_local(l, w.cur), &ch) &&
         guard(l, b, ex_local(l, w.end), ch.acc, &in) && chain_node(l, &body, &w, &node) &&
         chain_pass(l, &body, node, &ch) && chain_end(l, b, &w, &body);
}

/* The two arms of an option or a sum, behind the guard on the flag of X. */
static int two_arms(Low *l, Buf *b, Operand x, const Buf *a0, const Buf *a1, Slot res) {
  IrBlock *arms = arena_alloc(l->m->arena, 2u * sizeof *arms);
  Buf in = buf_under(b);
  if (arms == NULL)
    return oom(l);
  arms[0] = block_of(a0);
  arms[1] = block_of(a1);
  return st_switch(l, &in, ex_load(l, x.w, 0), arms, 2u) && guard(l, b, x.f, res, &in);
}

/* Lowers the option or sum XV into X. Arm 0 gets RES := X; arm 1 gets P :=
   the payload. */
static int carrier_begin(Low *l, Buf *b, const Value *xv, Slot res, Operand *x, Buf *a0, Buf *a1, Operand *p) {
  return lower(l, b, xv, x) && store(l, a0, res, *x) && field_at(l, a1, x->w, 0, p);
}

static int carrier_map(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand x;
  Operand p;
  Operand y;
  Operand t;
  Slot res = slot_new(l);
  Slot ts = slot_new(l);
  Buf a0 = buf_under(b);
  Buf a1 = buf_under(b);
  *out = slot_get(l, res);
  t = slot_get(l, ts);
  return carrier_begin(l, b, v->args[1], res, &x, &a0, &a1, &p) && apply_into(l, &a1, v->args[0], &p, 1, ts) &&
         block_ops(l, &a1, 1, &t, 1, &y) && store(l, &a1, res, y) && two_arms(l, b, x, &a0, &a1, res);
}

static int carrier_bind(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand x;
  Operand p;
  Slot res = slot_new(l);
  Buf a0 = buf_under(b);
  Buf a1 = buf_under(b);
  *out = slot_get(l, res);
  return carrier_begin(l, b, v->args[0], res, &x, &a0, &a1, &p) && apply_into(l, &a1, v->args[1], &p, 1, res) &&
         two_arms(l, b, x, &a0, &a1, res);
}

static int carrier_filter(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand x;
  Operand p;
  Operand y;
  Slot res = slot_new(l);
  Slot ts = slot_new(l);
  Buf a0 = buf_under(b);
  Buf a1 = buf_under(b);
  Buf keep = buf_under(&a1);
  Buf drop = buf_under(&a1);
  Buf test = buf_under(&a1);
  *out = slot_get(l, res);
  return carrier_begin(l, b, v->args[1], res, &x, &a0, &a1, &p) && apply_into(l, &a1, v->args[0], &p, 1, ts) &&
         store(l, &keep, res, x) && block_ops(l, &drop, 0, NULL, 0, &y) && store(l, &drop, res, y) &&
         if_flag(l, &test, ex_local(l, ts.w), &keep, &drop) && guard(l, &a1, ex_local(l, ts.f), res, &test) &&
         two_arms(l, b, x, &a0, &a1, res);
}

/* FILTER on a list, or on a sum. */
static int not_option(Low *l, Buf *b, const Value *v, Operand *out) {
  return v->inst == CARRIER_LIST ? list_filter(l, b, v, out) : internal(l, "a filter over a sum");
}

/* ACC := Z, then ACC := STEP ACC, N times. */
static int fold_nat(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand n;
  Operand a;
  Slot acc = slot_new(l);
  Buf in = buf_under(b);
  Buf body = buf_under(&in);
  *out = slot_get(l, acc);
  a = slot_get(l, acc);
  return lower(l, b, v->args[2], &n) && lower_into(l, &in, v->args[1], acc) && apply_into(l, &body, v->args[0], &a, 1, acc) &&
         st_repeat(l, &in, n.w, &body) && guard(l, b, n.f, acc, &in);
}

/* Proof contents erase, but forcing a proof must preserve its trap flag. */
static int proof_op(Low *l, Buf *b, const Value *v, uint32_t first, uint32_t count, Operand *out) {
  Operand p;
  const IrExpr *flag = ex_const(l, 0);
  uint32_t i;
  for (i = first; i < first + count; i++) {
    if (!lower(l, b, v->args[i], &p))
      return 0;
    flag = ex_or(l, flag, p.f);
  }
  return atoms(l, b, ex_const(l, 0), flag, out);
}

/* natDiv and natMod: a zero divisor sets the flag, and the word divides by
   1 (src/ir.h). */
static int nat_divmod(Low *l, Buf *b, const Value *v, IrOp op, Operand *out) {
  Operand x;
  Operand y;
  const IrExpr *zero;
  if (!lower(l, b, v->args[0], &x) || !lower(l, b, v->args[1], &y))
    return 0;
  zero = atom(l, b, ex_bin(l, IR_OP_EQ, y.w, ex_const(l, 0)));
  return atoms(l, b, ex_bin(l, op, x.w, ex_bin(l, IR_OP_ADD, y.w, zero)), ex_or(l, zero, ex_or(l, x.f, y.f)), out);
}

/* CALL on the N arguments of V, behind the guard on their flags. ZERO: the
   result 0 is a trap (a U256 overflow or curve error; never a heap block). */
static int call_op(Low *l, Buf *b, const Value *v, IrCall call, uint32_t n, int zero, Operand *out) {
  const IrExpr **args = arena_alloc(l->m->arena, ((size_t)n + 1u) * sizeof *args);
  const IrExpr *f = ex_const(l, 0);
  const IrExpr *w;
  Operand x;
  Slot res = slot_new(l);
  Buf in = buf_under(b);
  uint32_t i;
  *out = slot_get(l, res);
  if (args == NULL)
    return oom(l);
  for (i = 0; i < n; i++) {
    if (!lower(l, b, v->args[i], &x))
      return 0;
    args[i] = x.w;
    f = ex_or(l, f, x.f);
  }
  w = atom(l, &in, ex_call(l, call, args, n));
  return w != NULL && store(l, &in, res, operand(w, zero ? ex_bin(l, IR_OP_EQ, w, ex_const(l, 0)) : ex_const(l, 0))) &&
         guard(l, b, f, res, &in);
}

/* finAdd n a b and finNeg n a as Nat operations on the index (Q-S8-1; eval.c
   fin_add, fin_neg). Fin 0 has no value, so n = 0 sets the flag and the
   modulus is 1. */
static int fin_op(Low *l, Buf *b, const Value *v, int add, Operand *out) {
  Operand n;
  Operand x;
  Operand y = {NULL, NULL};
  const IrExpr *k;
  const IrExpr *xm;
  const IrExpr *ym;
  const IrExpr *d;
  const IrExpr *f;
  if (!lower(l, b, v->args[0], &n) || !lower(l, b, v->args[1], &x) || (add && !lower(l, b, v->args[2], &y)))
    return 0;
  k = atom(l, b, ex_bin(l, IR_OP_ADD, n.w, ex_bin(l, IR_OP_EQ, n.w, ex_const(l, 0))));
  f = ex_or(l, ex_or(l, n.f, x.f), ex_bin(l, IR_OP_EQ, n.w, ex_const(l, 0)));
  xm = atom(l, b, ex_bin(l, IR_OP_MOD, x.w, k));
  if (!add)
    return atoms(l, b, ex_bin(l, IR_OP_MOD, ex_bin(l, IR_OP_SUB, n.w, xm), k), f, out);
  ym = atom(l, b, ex_bin(l, IR_OP_MOD, y.w, k));
  d = atom(l, b, ex_bin(l, IR_OP_SUB, n.w, ym));
  return atoms(l, b,
               ex_bin(l, IR_OP_ADD, ex_bin(l, IR_OP_SUB, xm, d), ex_bin(l, IR_OP_MUL, ex_bin(l, IR_OP_LT, xm, d), ex_bin(l, IR_OP_ADD, xm, ym))),
               ex_or(l, f, y.f), out);
}

/* finAll at a run-time index k: a SWITCH on k, arm J the body J. */
static int fin_switch(Low *l, Buf *b, const Value *v, Operand *out) {
  IrBlock *arms = arena_alloc(l->m->arena, (size_t)v->argc * sizeof *arms);
  Operand k;
  Slot res = slot_new(l);
  Buf in = buf_under(b);
  Buf arm;
  uint32_t j;
  *out = slot_get(l, res);
  if (arms == NULL)
    return oom(l);
  if (!lower(l, b, v->args[0], &k))
    return 0;
  for (j = 0; j + 1u < v->argc; j++) {
    arm = buf_under(b);
    if (!lower_into(l, &arm, v->args[j + 1u], res))
      return 0;
    arms[j] = block_of(&arm);
  }
  return st_switch(l, &in, k.w, arms, v->argc - 1u) && guard(l, b, k.f, res, &in);
}

/* elim P c1 .. cn x on a variable (eval.c reduce_elim): at Sum the two
   cases, at Unit the one case, with no case a trap, and at a domain family
   (FIELD = the family + 1) the family fold with the cases after P. */
static int elim_of(Low *l, Buf *b, const Value *v, Operand *out) {
  uint32_t fields[2] = {1u, 1u};
  Value *w;
  Operand x;
  Slot res;
  Buf in;
  if (v->field != 0u) {
    w = arena_alloc(l->m->arena, sizeof *w);
    if (w == NULL)
      return oom(l);
    *w = *v;
    w->inst = v->field - 1u;
    w->args = v->args + 1;
    w->argc = v->argc - 1u;
    return family_fold(l, b, w, out);
  }
  if (v->inst == 2u)
    return cases_of(l, b, v->args[3], v->args + 1, fields, 2, out);
  if (v->inst != 1u)
    return lower(l, b, v->args[v->argc - 1u], &x) && atoms(l, b, ex_const(l, 0), ex_const(l, 1), out);
  res = slot_new(l);
  in = buf_under(b);
  *out = slot_get(l, res);
  return lower(l, b, v->args[2], &x) && lower_into(l, &in, v->args[1], res) && guard(l, b, x.f, res, &in);
}

/* An eliminator on a variable. */
static int lower_stuck(Low *l, Buf *b, const Value *v, Operand *out) {
  switch (v->op) {
  case OP_NAT:
  case OP_FLAG:
  case OP_UNIT:
  case OP_PROD:
  case OP_SUM:
  case OP_OPTION:
  case OP_LIST:
  case OP_FAMILY:
  case OP_UNIT_VAL:
  case OP_FLAG_YES:
  case OP_FLAG_NO:
  case OP_PAIR:
  case OP_INL:
  case OP_INR:
  case OP_NONE:
  case OP_SOME:
  case OP_NIL:
  case OP_CONS:
  case OP_PACK:
  case OP_CTOR:
  case OP_PURE:
  case OP_U256:
  case OP_ADDR:
  case OP_INTERVAL:
  case OP_SYSTEM:
  case OP_FIN:
    return internal(l, "a stuck canonical value");
  case OP_FIRST:
  case OP_WITNESS:
    return field_of(l, b, v->args[0], 0, out);
  case OP_SECOND:
  case OP_PAYLOAD:
    return field_of(l, b, v->args[0], 1, out);
  case OP_PROJ:
    return field_of(l, b, v->args[0], v->field, out);
  case OP_EITHER:
    return two_cases(l, b, v, 1, out);
  case OP_OPTION_ELIM:
    return two_cases(l, b, v, 0, out);
  case OP_MAP:
    return v->inst == CARRIER_LIST ? list_map(l, b, v, out) : carrier_map(l, b, v, out);
  case OP_BIND:
    return v->inst == CARRIER_LIST ? list_bind(l, b, v, out) : carrier_bind(l, b, v, out);
  case OP_FILTER:
    return v->inst == CARRIER_OPTION ? carrier_filter(l, b, v, out) : not_option(l, b, v, out);
  case OP_FOLD_NAT:
    return fold_nat(l, b, v, out);
  case OP_FOLD_LIST:
    return fold_list(l, b, v, out);
  case OP_UNFOLD:
    return unfold(l, b, v, out);
  case OP_FOLD_FAMILY:
    return family_fold(l, b, v, out);
  case OP_ELIM:
    return elim_of(l, b, v, out);
  case OP_NAT_ADD:
    return nat_bin(l, b, v, IR_OP_ADD, 1, out);
  case OP_NAT_SUB:
    return nat_bin(l, b, v, IR_OP_SUB, 0, out);
  case OP_NAT_EQ:
    return nat_bin(l, b, v, IR_OP_EQ, 0, out);
  case OP_NAT_LE:
    return nat_bin(l, b, v, IR_OP_LE, 0, out);
  case OP_FLAG_IF:
    return flag_if(l, b, v, out);
  case OP_NAT_DIV:
    return nat_divmod(l, b, v, IR_OP_DIV, out);
  case OP_NAT_MOD:
    return nat_divmod(l, b, v, IR_OP_MOD, out);
  case OP_U256_ADD:
    return call_op(l, b, v, IR_CALL_U256_ADD, 2, 1, out);
  case OP_U256_SUB:
    return call_op(l, b, v, IR_CALL_U256_SUB, 2, 0, out);
  case OP_U256_LE:
    return call_op(l, b, v, IR_CALL_U256_LE, 2, 0, out);
  case OP_U256_EQ:
  case OP_ADDR_EQ:
    return call_op(l, b, v, IR_CALL_U256_EQ, 2, 0, out);
  case OP_TO_U256:
    return call_op(l, b, v, IR_CALL_TO_U256, 1, 0, out);
  case OP_HASH:
    return call_op(l, b, v, IR_CALL_SHA256, 2, 0, out);
  case OP_ADDR_TO_U256:
    return lower(l, b, v->args[0], out);
  case OP_FIN_ADD:
    return fin_op(l, b, v, 1, out);
  case OP_FIN_NEG:
    return fin_op(l, b, v, 0, out);
  case OP_FIN_CASE:
    return fin_switch(l, b, v, out);
  case OP_NAT_IS_SET:
  case OP_U256_IS_SET:
  case OP_FLAG_IS_SET:
    return proof_op(l, b, v, 0, v->argc, out);
  case OP_EC_ADD:
    return call_op(l, b, v, IR_CALL_EC_ADD, 2, 1, out);
  case OP_EC_MUL:
    return call_op(l, b, v, IR_CALL_EC_MUL, 2, 1, out);
  case OP_SCHNORR_OK:
    return call_op(l, b, v, IR_CALL_SCHNORR_OK, 5, 0, out);
  case OP_TRANSP:
    return refuse(l, "a transp that does not reduce at compile time");
  case OP_HCOMP:
    return refuse(l, "an hcomp that does not reduce at compile time");
  case OP_GLUE:
    return refuse(l, "a Glue type that does not reduce at compile time");
  case OP_GLUE_ELEM:
    return refuse(l, "a glue that does not reduce at compile time");
  case OP_UNGLUE:
    return refuse(l, "an unglue that does not reduce at compile time");
  case OP_KMAP:
  case OP_KMAP_OF:
  case OP_KMAP_GET:
  case OP_KMAP_SET:
    return refuse(l, "a run-time map (KMap)");
  case OP_ZK_KEY:
  case OP_ZK_SIGN:
  case OP_ZK_WITNESS:
  case OP_ZK_CAST:
  case OP_ZK_PROVE:
    return secret_op(l);
  case OP_ZK_HP:
  case OP_ZK_DIGEST:
  case OP_ZK_VERIFY:
  case OP_ZK_BALLOT:
  case OP_ZK_PROJECT:
  case OP_ZK_CHECK:
    return refuse(l, "a host proof check (zk)");
  }
  return internal(l, "an unknown operation");
}

static int lower_value(Low *l, Buf *b, const Value *v, Operand *out) {
  Operand p;
  Operand e;
  const Value *end;
  switch (v->kind) {
  case VAL_NAT:
    return give(l, out, v->nat, 0);
  case VAL_WORD:
    return word_block(l, b, v->word, out);
  /* A path, a path type and an interval term erase (Q-S8-1). A path
     application keeps the flag of the path. At a kept end (Q-S8-13) the
     word is the end. */
  case VAL_PLAM:
  case VAL_PATHP:
  case VAL_IVAL:
    return give(l, out, 0, 0);
  case VAL_PAPP:
    end = v->argc != 2u ? NULL : ival_is_end(v->arg, 0) ? v->args[0] : ival_is_end(v->arg, 1) ? v->args[1] : NULL;
    if (end != NULL)
      return lower(l, b, v->dom, &p) && lower(l, b, end, &e) && atoms(l, b, e.w, ex_or(l, p.f, e.f), out);
    return lower(l, b, v->dom, &p) && atoms(l, b, ex_const(l, 0), p.f, out);
  case VAL_TRAP:
    return give(l, out, 0, 1);
  case VAL_UNIV:
  case VAL_PI:
  case VAL_SIGMA:
    return give(l, out, 0, 0);
  case VAL_LAM:
    return closure(l);
  /* A fold into B -> B applied to B0 may be a loop (Q-S8-11). */
  case VAL_APP:
    return chain_fold(l, b, v, out);
  case VAL_VAR:
    if (v->nat >= l->level_count)
      return internal(l, "a variable without an operand");
    *out = l->levels[v->nat];
    return 1;
  case VAL_OP:
    return lower_canon(l, b, v, out);
  case VAL_STUCK:
    return lower_stuck(l, b, v, out);
  }
  return internal(l, "an unknown value");
}

static int lower(Low *l, Buf *b, const Value *v, Operand *out) {
  int ok;
  if (v == NULL)
    return got(l, v);
  if (memo_kind(v) && memo_find(b, v, out))
    return 1;
  if (l->m->depth >= EVAL_DEPTH_LIMIT)
    return diag_fail(l->m->diag, "EVAL_DEPTH", l->m->def, "the lowering nests deeper than %u levels", EVAL_DEPTH_LIMIT);
  l->m->depth++;
  ok = lower_value(l, b, v, out);
  l->m->depth--;
  return ok && (!memo_kind(v) || memo_put(l, b, v, *out));
}

/* The entry INDEX, applied to a variable for each parameter. Only the
   result is strict: its flag runs TRAP. */
static int lower_entry(Machine *m, uint32_t index, const Entry *e, IrFunc *fn) {
  Low l;
  Buf body = buf_under(NULL);
  Buf trap = buf_under(NULL);
  Buf none = buf_under(NULL);
  IrScalar *params = arena_alloc(m->arena, ((size_t)e->param_count + 1u) * sizeof *params);
  const Value *v;
  const Value *x;
  Operand r;
  uint32_t k;
  memset(&l, 0, sizeof l);
  l.m = m;
  l.local_count = e->param_count;
  m->def = m->defs[index].name;
  m->fuel = EVAL_FUEL_STEPS;
  if (params == NULL)
    return oom(&l);
  v = def_value(m, index);
  for (k = 0; k < e->param_count && v != NULL; k++) {
    params[k] = e->param_flag[k] ? IR_SCALAR_FLAG : e->param_u256[k] ? IR_SCALAR_U256 : IR_SCALAR_NAT;
    x = fresh(&l, operand(ex_local(&l, k), ex_const(&l, 0)));
    v = x == NULL ? NULL : apply_value(m, v, x);
  }
  if (!got(&l, v) || !lower(&l, &body, v, &r) || !emit(&l, &trap, stmt_new(&l, IR_STMT_TRAP)) ||
      !if_flag(&l, &body, r.f, &trap, &none) || !emit(&l, &body, stmt_expr(&l, IR_STMT_RETURN, r.w)))
    return 0;
  fn->name = m->defs[index].name;
  fn->params = params;
  fn->param_count = e->param_count;
  fn->result = e->result_flag ? IR_SCALAR_FLAG : e->result_u256 ? IR_SCALAR_U256 : IR_SCALAR_NAT;
  fn->local_count = l.local_count;
  fn->body = block_of(&body);
  return 1;
}

int lower_program(Machine *m, IrProgram *prog) {
  IrFunc *funcs = arena_alloc(m->arena, ((size_t)m->def_count + 1u) * sizeof *funcs);
  Entry e;
  size_t n = 0;
  uint32_t i;
  int entry;
  prog->funcs = funcs;
  prog->func_count = 0;
  if (funcs == NULL)
    return diag_fail(m->diag, "OOM", m->def, "out of memory");
  m->keep_ends = 1;
  for (i = 0; i < m->def_count; i++) {
    m->fuel = EVAL_FUEL_STEPS;
    entry = m->defs[i].origin == ORIGIN_PROGRAM && entry_of(m, m->defs[i].type, &e);
    if (!entry && m->defs[i].origin == ORIGIN_PROGRAM && family_result_of(m, m->defs[i].type))
      fprintf(stderr, "langc: note: %s: a family result has no target form (O24)\n", m->defs[i].name);
    if (entry && !lower_entry(m, i, &e, &funcs[n]))
      return 0;
    n += entry ? 1u : 0u;
  }
  m->keep_ends = 0;
  prog->func_count = n;
  return 1;
}
