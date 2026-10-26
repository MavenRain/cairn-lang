/* The type checker (see front/check.h). SHARED by the tcc kits.

   Checking is bidirectional. A term is checked against an expected type (the
   hint) or its type is inferred. Two types are equal when their normal forms
   are equal (front/eval.c). The checker writes core terms (front/core.h). */
#include "front/check.h"
#include "front/blsag.h"
#include "front/lexer.h"
#include "front/parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CHECK_DEPTH_LIMIT 2000u
#define SPINE_MAX 64u
#define SHOW_MAX 512u
#define NAMES_MAX 64u
#define UNIVERSE_MAX 2u
#define PRINT_MAX 65536u
#define NO_FAMILY UINT32_MAX
#define NO_DEF UINT32_MAX

#define FAIL(c, code, ...) diag_fail((c)->m->diag, (code), (c)->m->def, __VA_ARGS__)

typedef struct Scope Scope;
struct Scope {
  const char *name; /* NULL for the binder of `A -> B` */
  const Value *type;
  uint32_t level;
  const Scope *next;
};

typedef struct {
  Machine *m;
  const DeclList *decls;
  size_t decl_index;
  const Scope *scope; /* the innermost binder first */
  const Env *env;     /* a variable for each binder */
  uint32_t level;     /* the number of binders */
  unsigned depth;
  uint32_t eta_count;
  uint32_t self;       /* the first family of the block whose fields are checked, or NO_FAMILY */
  uint32_t self_count; /* the number of sorts of that block (S6) */
  /* A block fold (S6): the target of each sort, while a case type is made,
     or NULL. */
  const Value *const *fold_targets;
  uint32_t fold_first;
  uint32_t fold_count;
  /* The motive value while the case types of an elim over a domain family
     are built (S7 B1), else NULL. */
  const Value *elim_motive;
  uint32_t self_uses;  /* the uses of a sort of that block in the current field */
  /* The levels of the earlier point fields of the current field, from
     FIELD_BASE up to FIELD_TOP (S4 item 13), and the uses of them. */
  uint32_t field_base;
  uint32_t field_top;
  int field_dep;
  int field_rec;
  CheckMode mode; /* orbit-lang S3: guest or kit */
} Checker;

/* The visibility of a core name (orbit-lang S3). An INTERNAL name is a kit
   item outside the base span (PLAN section 4): only the trusted prelude
   scope and the kit mode resolve it. */
typedef enum {
  VIS_GUEST,
  VIS_INTERNAL,
  /* A cons-list name (S3, USER ruling Q-S3-5 (a), rule R1): the kit and
     the trusted scope use it (the host ABI); a guest use gives REFUSE_CELL. */
  VIS_CELL
} Vis;

typedef enum {
  RULE_TYPE0,
  RULE_TYPE1,
  RULE_TYPE2,
  RULE_INTERVAL,
  RULE_IEND,
  RULE_PATHP,
  RULE_PATH,
  RULE_VALUE0,
  RULE_PAIR,
  RULE_INJ,
  RULE_EMPTY,
  RULE_SOME,
  RULE_CONS,
  RULE_PACK,
  RULE_REFL,
  RULE_PART,
  RULE_SIGMA_PART,
  RULE_EITHER,
  RULE_OPTION,
  RULE_ELIM,
  RULE_PURE,
  RULE_MAP,
  RULE_BIND,
  RULE_FILTER,
  RULE_FOLD,
  RULE_UNFOLD,
  RULE_SYM,
  RULE_CONG,
  RULE_FUNEXT,
  RULE_TRANSP,
  RULE_HCOMP,
  RULE_NAT2,
  RULE_LEN, /* S3: len A r (survey section 7) */
  RULE_U256_2,
  RULE_ADDR2,
  RULE_TO_U256,
  RULE_FLAG_IF,
  RULE_KMAP,
  RULE_KMAP_GET,
  RULE_KMAP_SET,
  RULE_FIXED, /* S2: a fixed signature over U256, Nat, G1 and List (Prod G1 G2) */
  RULE_FIN,
  RULE_FIN_OP,
  RULE_GLUE,
  RULE_GLUE_ELEM,
  RULE_UNGLUE,
  RULE_ZK_CAST,    /* S8 B2b: zkCast M C m c */
  RULE_ZK_PROVE,   /* zkProve b */
  RULE_ZK_PROJECT, /* zkProject C b */
  RULE_ZK_CHECK    /* zkCheck M C w c */
} Rule;

typedef struct {
  const char *name;
  uint32_t arity; /* fold: the least number of arguments */
  Rule rule;
  Op op;
  Op result;     /* VALUE0, NAT2, U256_2, ADDR2, TO_U256: the result type. EMPTY, INJ: the type former. */
  uint32_t part; /* INJ, PART, SIGMA_PART: 0 or 1 */
  Vis vis;
} Builtin;

static const Builtin BUILTINS[] = {
  {"Nat", 0, RULE_TYPE0, OP_NAT, OP_NAT, 0, VIS_INTERNAL},
  {"Flag", 0, RULE_TYPE0, OP_FLAG, OP_NAT, 0, VIS_INTERNAL},
  {"U256", 0, RULE_TYPE0, OP_U256, OP_NAT, 0, VIS_INTERNAL},
  {"Addr", 0, RULE_TYPE0, OP_ADDR, OP_NAT, 0, VIS_INTERNAL},
  {"Unit", 0, RULE_TYPE0, OP_UNIT, OP_NAT, 0, VIS_GUEST},
  {"Prod", 2, RULE_TYPE2, OP_PROD, OP_NAT, 0, VIS_GUEST},
  {"Sum", 2, RULE_TYPE2, OP_SUM, OP_NAT, 0, VIS_GUEST},
  {"Option", 1, RULE_TYPE1, OP_OPTION, OP_NAT, 0, VIS_GUEST},
  {"List", 1, RULE_TYPE1, OP_LIST, OP_NAT, 0, VIS_CELL},
  {"Fin", 1, RULE_FIN, OP_FIN, OP_NAT, 0, VIS_INTERNAL},
  {"finAdd", 3, RULE_FIN_OP, OP_FIN_ADD, OP_FIN, 0, VIS_INTERNAL},
  {"finNeg", 2, RULE_FIN_OP, OP_FIN_NEG, OP_FIN, 0, VIS_INTERNAL},
  {"I", 0, RULE_INTERVAL, OP_INTERVAL, OP_NAT, 0, VIS_GUEST},
  {"i0", 0, RULE_IEND, OP_INTERVAL, OP_INTERVAL, 0, VIS_GUEST},
  {"i1", 0, RULE_IEND, OP_INTERVAL, OP_INTERVAL, 1, VIS_GUEST},
  {"PathP", 3, RULE_PATHP, OP_INTERVAL, OP_NAT, 0, VIS_GUEST},
  {"Path", 3, RULE_PATH, OP_INTERVAL, OP_NAT, 0, VIS_GUEST},
  {"unit", 0, RULE_VALUE0, OP_UNIT_VAL, OP_UNIT, 0, VIS_GUEST},
  {"flagYes", 0, RULE_VALUE0, OP_FLAG_YES, OP_FLAG, 0, VIS_INTERNAL},
  {"flagNo", 0, RULE_VALUE0, OP_FLAG_NO, OP_FLAG, 0, VIS_INTERNAL},
  {"pair", 2, RULE_PAIR, OP_PAIR, OP_PROD, 0, VIS_GUEST},
  {"inl", 1, RULE_INJ, OP_INL, OP_SUM, 0, VIS_GUEST},
  {"inr", 1, RULE_INJ, OP_INR, OP_SUM, 1, VIS_GUEST},
  {"none", 0, RULE_EMPTY, OP_NONE, OP_OPTION, 0, VIS_GUEST},
  {"nil", 0, RULE_EMPTY, OP_NIL, OP_LIST, 0, VIS_CELL},
  {"some", 1, RULE_SOME, OP_SOME, OP_OPTION, 0, VIS_GUEST},
  {"cons", 2, RULE_CONS, OP_CONS, OP_LIST, 0, VIS_CELL},
  {"pack", 2, RULE_PACK, OP_PACK, OP_NAT, 0, VIS_GUEST},
  {"refl", 0, RULE_REFL, OP_INTERVAL, OP_NAT, 0, VIS_GUEST},
  {"first", 1, RULE_PART, OP_FIRST, OP_PROD, 0, VIS_GUEST},
  {"second", 1, RULE_PART, OP_SECOND, OP_PROD, 1, VIS_GUEST},
  {"witness", 1, RULE_SIGMA_PART, OP_WITNESS, OP_NAT, 0, VIS_GUEST},
  {"payload", 1, RULE_SIGMA_PART, OP_PAYLOAD, OP_NAT, 1, VIS_GUEST},
  {"either", 3, RULE_EITHER, OP_EITHER, OP_SUM, 0, VIS_GUEST},
  {"option", 3, RULE_OPTION, OP_OPTION_ELIM, OP_OPTION, 0, VIS_GUEST},
  {"elim", 3, RULE_ELIM, OP_ELIM, OP_SUM, 0, VIS_GUEST},
  {"pure", 1, RULE_PURE, OP_PURE, OP_NAT, 0, VIS_GUEST},
  {"map", 2, RULE_MAP, OP_MAP, OP_NAT, 0, VIS_GUEST},
  {"bind", 2, RULE_BIND, OP_BIND, OP_NAT, 0, VIS_GUEST},
  {"filter", 2, RULE_FILTER, OP_FILTER, OP_NAT, 0, VIS_GUEST},
  {"fold", 2, RULE_FOLD, OP_FOLD_NAT, OP_NAT, 0, VIS_GUEST},
  {"unfold", 3, RULE_UNFOLD, OP_UNFOLD, OP_LIST, 0, VIS_CELL},
  {"sym", 1, RULE_SYM, OP_INTERVAL, OP_NAT, 0, VIS_GUEST},
  {"cong", 2, RULE_CONG, OP_INTERVAL, OP_NAT, 0, VIS_GUEST},
  {"funExt", 1, RULE_FUNEXT, OP_INTERVAL, OP_NAT, 0, VIS_GUEST},
  {"transp", 3, RULE_TRANSP, OP_TRANSP, OP_NAT, 0, VIS_GUEST},
  {"hcomp", 4, RULE_HCOMP, OP_HCOMP, OP_NAT, 0, VIS_GUEST},
  {"Glue", 3, RULE_GLUE, OP_GLUE, OP_NAT, 0, VIS_GUEST},
  {"glue", 2, RULE_GLUE_ELEM, OP_GLUE_ELEM, OP_NAT, 0, VIS_GUEST},
  {"unglue", 1, RULE_UNGLUE, OP_UNGLUE, OP_NAT, 0, VIS_GUEST},
  {"natAdd", 2, RULE_NAT2, OP_NAT_ADD, OP_NAT, 0, VIS_INTERNAL},
  {"natSub", 2, RULE_NAT2, OP_NAT_SUB, OP_NAT, 0, VIS_INTERNAL},
  {"natEq", 2, RULE_NAT2, OP_NAT_EQ, OP_FLAG, 0, VIS_INTERNAL},
  {"natLe", 2, RULE_NAT2, OP_NAT_LE, OP_FLAG, 0, VIS_INTERNAL},
  {"u256Add", 2, RULE_U256_2, OP_U256_ADD, OP_U256, 0, VIS_INTERNAL},
  {"u256Sub", 2, RULE_U256_2, OP_U256_SUB, OP_U256, 0, VIS_INTERNAL},
  {"u256Le", 2, RULE_U256_2, OP_U256_LE, OP_FLAG, 0, VIS_INTERNAL},
  {"u256Eq", 2, RULE_U256_2, OP_U256_EQ, OP_FLAG, 0, VIS_INTERNAL},
  {"toU256", 1, RULE_TO_U256, OP_TO_U256, OP_U256, 0, VIS_INTERNAL},
  {"addrEq", 2, RULE_ADDR2, OP_ADDR_EQ, OP_FLAG, 0, VIS_INTERNAL},
  {"flagIf", 3, RULE_FLAG_IF, OP_FLAG_IF, OP_FLAG, 0, VIS_INTERNAL},
  {"Map", 2, RULE_KMAP, OP_KMAP, OP_NAT, 0, VIS_INTERNAL},
  {"mapEmpty", 0, RULE_EMPTY, OP_KMAP_OF, OP_KMAP, 0, VIS_INTERNAL},
  {"mapGet", 2, RULE_KMAP_GET, OP_KMAP_GET, OP_KMAP, 0, VIS_INTERNAL},
  {"mapSet", 3, RULE_KMAP_SET, OP_KMAP_SET, OP_KMAP, 0, VIS_INTERNAL},
  {"natDiv", 2, RULE_NAT2, OP_NAT_DIV, OP_NAT, 0, VIS_INTERNAL},
  {"natMod", 2, RULE_NAT2, OP_NAT_MOD, OP_NAT, 0, VIS_INTERNAL},
  {"len", 2, RULE_LEN, OP_FM_LEN, OP_NAT, 0, VIS_GUEST},
  {"hash", 2, RULE_U256_2, OP_HASH, OP_U256, 0, VIS_INTERNAL},
  {"ecrecover", 4, RULE_FIXED, OP_ECRECOVER, OP_ADDR, 0, VIS_INTERNAL},
  {"ecAdd", 2, RULE_FIXED, OP_EC_ADD, OP_PROD, 0, VIS_INTERNAL},
  {"ecMul", 2, RULE_FIXED, OP_EC_MUL, OP_PROD, 0, VIS_INTERNAL},
  {"ecPairing", 1, RULE_FIXED, OP_EC_PAIRING, OP_FLAG, 0, VIS_INTERNAL},
  {"addrToU256", 1, RULE_ADDR2, OP_ADDR_TO_U256, OP_U256, 0, VIS_INTERNAL},
  {"zkHp", 1, RULE_FIXED, OP_ZK_HP, OP_PROD, 0, VIS_INTERNAL},
  {"zkKey", 1, RULE_FIXED, OP_ZK_KEY, OP_U256, 0, VIS_INTERNAL},
  {"zkDigest", 1, RULE_FIXED, OP_ZK_DIGEST, OP_U256, 0, VIS_INTERNAL},
  {"zkSign", 4, RULE_FIXED, OP_ZK_SIGN, OP_LIST, 0, VIS_INTERNAL},
  {"zkVerify", 4, RULE_FIXED, OP_ZK_VERIFY, OP_FLAG, 0, VIS_INTERNAL},
  {"ZkBallot", 1, RULE_TYPE1, OP_ZK_BALLOT, OP_NAT, 0, VIS_INTERNAL},
  {"ZkWitness", 0, RULE_TYPE0, OP_ZK_WITNESS, OP_NAT, 0, VIS_INTERNAL},
  {"zkCast", 4, RULE_ZK_CAST, OP_ZK_CAST, OP_ZK_BALLOT, 0, VIS_INTERNAL},
  {"zkProve", 1, RULE_ZK_PROVE, OP_ZK_PROVE, OP_ZK_WITNESS, 0, VIS_INTERNAL},
  {"zkProject", 2, RULE_ZK_PROJECT, OP_ZK_PROJECT, OP_NAT, 0, VIS_INTERNAL},
  {"zkCheck", 4, RULE_ZK_CHECK, OP_ZK_CHECK, OP_NAT, 0, VIS_INTERNAL}
};

#define BUILTIN_COUNT ((uint32_t)(sizeof BUILTINS / sizeof BUILTINS[0]))

/* Names that no declaration can take (rule R4). Prop and SProp are
   reserved because proof irrelevance is prohibited (orbit-lang S1
   ruling): no declaration can give a type the name of a Prop-like
   universe. */
static const char *const RESERVED[] = {"Bool", "Text", "Prop", "SProp"};

/* Time words (S3, O3): no declaration can take one and no name can use
   one (REFUSE_TIME). A contract counts operations instead. */
static const char *const TIME_WORDS[] = {"now", "block", "timestamp", "time", "clock", "epoch"};

#define TIME_HINT "%s is a time word; count operations: a position is `len r` (an OpPos) and a deadline is a count of later operations"

static int time_word(const char *name) {
  size_t i;
  for (i = 0; i < sizeof TIME_WORDS / sizeof TIME_WORDS[0]; i++) {
    if (strcmp(TIME_WORDS[i], name) == 0)
      return 1;
  }
  return 0;
}

typedef enum {
  HEAD_TERM, /* a local, a definition or another term: plain application */
  HEAD_FAMILY,
  HEAD_CTOR,
  HEAD_PROJ,
  HEAD_BUILTIN
} HeadKind;

typedef struct {
  HeadKind kind;
  uint32_t index; /* FAMILY: the family. CTOR, PROJ: the constructor. */
  uint32_t field; /* PROJ: the field number */
  const Builtin *builtin;
} Head;

/* One application of a core name to the arguments of a spine. */
typedef struct {
  Checker *c;
  const Builtin *b;
  const Term *const *args;
  uint32_t argc;      /* the arguments in the spine */
  const Value *hint;  /* the expected type of the result, or NULL */
  const Value *outer; /* the expected type of the whole spine, or NULL */
  uint32_t used;      /* the arguments that the core name takes */
  const Value *type;  /* the type of the result */
} Call;

static const Core *nul(int failed) {
  (void)failed;
  return NULL;
}

static const Value *vnul(int failed) {
  (void)failed;
  return NULL;
}

static Core *new_core(Checker *c, CoreKind kind) {
  Core *k = arena_alloc(c->m->arena, sizeof *k);
  if (k == NULL) {
    FAIL(c, "OOM", "out of memory");
    return NULL;
  }
  k->kind = kind;
  return k;
}

static const Core *core_index(Checker *c, CoreKind kind, uint32_t index) {
  Core *k = new_core(c, kind);
  if (k != NULL)
    k->index = index;
  return k;
}

static const Core *core_leaf(Checker *c, CoreKind kind, uint64_t nat) {
  Core *k = new_core(c, kind);
  if (k != NULL)
    k->nat = nat;
  return k;
}

static const Core *core_word(Checker *c, uint64_t sort, const U256 *word) {
  Core *k = new_core(c, CORE_WORD);
  if (k != NULL) {
    k->nat = sort;
    k->word = word;
  }
  return k;
}

static const Core *core_bind(Checker *c, CoreKind kind, const char *name, const Core *a, const Core *b) {
  Core *k;
  if (b == NULL || (a == NULL && kind != CORE_LAM && kind != CORE_PLAM))
    return NULL;
  k = new_core(c, kind);
  if (k == NULL)
    return NULL;
  k->name = name;
  k->a = a;
  k->b = b;
  return k;
}

static const Core *core_app(Checker *c, const Core *f, const Core *x) {
  Core *k;
  if (f == NULL || x == NULL)
    return NULL;
  k = new_core(c, CORE_APP);
  if (k == NULL)
    return NULL;
  k->a = f;
  k->b = x;
  return k;
}

/* CORE_PATHP (the line B) and CORE_PAPP (the path A at the interval B), with
   the ends E0 and E1. */
static const Core *core_ends(Checker *c, CoreKind kind, const char *name, const Core *a, const Core *b, const Core *e0, const Core *e1) {
  const Core **ends;
  Core *k;
  if (b == NULL || e0 == NULL || e1 == NULL || (a == NULL && kind != CORE_PATHP))
    return NULL;
  ends = arena_alloc(c->m->arena, 2u * sizeof *ends);
  if (ends == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  k = new_core(c, kind);
  if (k == NULL)
    return NULL;
  ends[0] = e0;
  ends[1] = e1;
  k->name = name;
  k->a = a;
  k->b = b;
  k->args = ends;
  k->argc = 2;
  return k;
}

/* An interval term: CORE_INEG (A), CORE_IMEET and CORE_IJOIN (A, B). */
static const Core *core_iop(Checker *c, CoreKind kind, const Core *a, const Core *b) {
  Core *k;
  if (a == NULL || (b == NULL && kind != CORE_INEG))
    return NULL;
  k = new_core(c, kind);
  if (k != NULL) {
    k->a = a;
    k->b = b;
  }
  return k;
}

static const Core *core_op(Checker *c, Op op, uint32_t inst, uint32_t field, const Core *const *args, uint32_t n) {
  const Core **copy = arena_alloc(c->m->arena, ((size_t)n + 1u) * sizeof *copy);
  Core *k;
  uint32_t i;
  if (copy == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  for (i = 0; i < n; i++) {
    if (args[i] == NULL)
      return NULL;
    copy[i] = args[i];
  }
  k = new_core(c, CORE_OP);
  if (k == NULL)
    return NULL;
  k->op = op;
  k->inst = inst;
  k->field = field;
  k->args = copy;
  k->argc = n;
  return k;
}

static const Core *op3(Checker *c, Op op, uint32_t inst, uint32_t n, const Core *a, const Core *b, const Core *x) {
  const Core *args[3];
  args[0] = a;
  args[1] = b;
  args[2] = x;
  return core_op(c, op, inst, 0, args, n);
}

static const Value *tyop(Checker *c, Op op, uint32_t n, const Value *a, const Value *b, const Value *x) {
  return val_make(c->m, op, 0, n, a, b, x);
}

static const Value *nat_type(Checker *c) {
  return tyop(c, OP_NAT, 0, NULL, NULL, NULL);
}

static const Value *flag_type(Checker *c) {
  return tyop(c, OP_FLAG, 0, NULL, NULL, NULL);
}

/* The sort I: the type of an interval term, never a type itself. */
static const Value *interval_type(Checker *c) {
  return tyop(c, OP_INTERVAL, 0, NULL, NULL, NULL);
}

/* The value of the core term K in the current context. */
static const Value *here(Checker *c, const Core *k) {
  return k == NULL ? NULL : eval_core(c->m, c->env, k);
}

/* V in the surface syntax, for a diagnostic. */
static const char *show(Checker *c, const Value *v) {
  const char *names[NAMES_MAX];
  const Scope *s;
  char *buf = arena_alloc(c->m->arena, SHOW_MAX);
  uint32_t count = c->level < NAMES_MAX ? c->level : NAMES_MAX;
  for (s = c->scope; s != NULL; s = s->next) {
    if (s->level < NAMES_MAX)
      names[s->level] = s->name != NULL ? s->name : "_";
  }
  if (buf == NULL || v == NULL)
    return "?";
  value_print(c->m, names, count, v, buf, SHOW_MAX);
  return buf;
}

static int push_value(Checker *c, const char *name, const Value *type, const Value *value) {
  Scope *s;
  const Env *e;
  if (name != NULL && time_word(name))
    return FAIL(c, "REFUSE_TIME", TIME_HINT, name);
  s = arena_alloc(c->m->arena, sizeof *s);
  e = env_push(c->m, c->env, value);
  if (type == NULL || value == NULL || s == NULL || e == NULL)
    return c->m->diag->set ? 0 : FAIL(c, "OOM", "out of memory");
  s->name = name;
  s->type = type;
  s->level = c->level;
  s->next = c->scope;
  c->scope = s;
  c->env = e;
  c->level++;
  return 1;
}

static int push(Checker *c, const char *name, const Value *type) {
  return push_value(c, name, type, val_var(c->m, c->level));
}

/* An interval binder: its value is an interval variable, not a neutral. */
static int push_interval(Checker *c, const char *name) {
  return push_value(c, name, interval_type(c), val_ivar(c->m, c->level));
}

static void pop(Checker *c) {
  c->scope = c->scope->next;
  c->env = c->env->next;
  c->level--;
}

/* 1 when a term of type FOUND can stand where WANT is expected.
   Universes are not cumulative (former F14). */
static int expect(Checker *c, const Value *found, const Value *want) {
  if (found == NULL || want == NULL)
    return 0;
  if (conv_values(c->m, c->level, found, want))
    return 1;
  return FAIL(c, "TYPE_MISMATCH", "expected a term of type %s, found a term of type %s", show(c, want), show(c, found));
}

static const Scope *find_local(const Checker *c, const char *name) {
  const Scope *s;
  for (s = c->scope; s != NULL; s = s->next) {
    if (s->name != NULL && strcmp(s->name, name) == 0)
      return s;
  }
  return NULL;
}

static uint32_t find_def(const Machine *m, const char *name) {
  uint32_t i;
  for (i = 0; i < m->def_count; i++) {
    if (strcmp(m->defs[i].name, name) == 0)
      return i;
  }
  return NO_DEF;
}

/* 1 when ANN is the sort I: the name I, not taken by a local or a
   definition. Only a binder annotation can name I. */
static int is_interval(const Checker *c, const Term *ann) {
  return ann != NULL && ann->kind == TERM_VAR && strcmp(ann->name, "I") == 0 && find_local(c, ann->name) == NULL && find_def(c->m, ann->name) == NO_DEF;
}

/* An environment that binds VALS in order: the last one is index 0. */
static const Env *env_of(Checker *c, const Value *const *vals, uint32_t n) {
  const Env *e = NULL;
  uint32_t i;
  for (i = 0; i < n; i++)
    e = env_push(c->m, e, vals[i]);
  return e;
}

/* The value of K, a term under one interval binder, with R for the binder. */
static const Value *at(Checker *c, const Core *k, const Value *r) {
  const Env *e = env_push(c->m, c->env, r);
  return k == NULL || e == NULL ? NULL : eval_core(c->m, e, k);
}

static int end_fail(Checker *c, int uip, const char *side, const Value *found, const Value *want) {
  if (uip)
    return FAIL(c, "PATH_UIP", "the path at %s is %s, but the type needs %s; these ends are paths, and only a rule of uniqueness of identity proofs can make them equal (rule R9)", side, show(c, found), show(c, want));
  return FAIL(c, "PATH_END", "the path at %s is %s, but the type needs %s", side, show(c, found), show(c, want));
}

/* The end check of a path against the path type E: V0 and V1 are its
   values at i0 and i1. */
static int path_ends(Checker *c, const Value *e, const Value *v0, const Value *v1) {
  const Value *ends = closure_apply(c->m, e, val_iend(c->m, 0));
  int uip = ends != NULL && ends->kind == VAL_PATHP;
  if (v0 == NULL || v1 == NULL)
    return 0;
  if (!conv_values(c->m, c->level, v0, e->args[0]))
    return end_fail(c, uip, "i0", v0, e->args[0]);
  return conv_values(c->m, c->level, v1, e->args[1]) ? 1 : end_fail(c, uip, "i1", v1, e->args[1]);
}

static const Core *elab(Checker *c, const Term *t, const Value *hint, const Value **type);
static const Core *check_lam(Checker *c, const Term *t, const Value *hint, const Value **type);
static const Core *checked_annotation(Checker *c, const Term *ann, const Value *dom);

static const Core *check(Checker *c, const Term *t, const Value *want) {
  const Value *type = NULL;
  return want == NULL ? NULL : elab(c, t, want, &type);
}

static const Core *infer(Checker *c, const Term *t, const Value **type) {
  *type = NULL;
  return elab(c, t, NULL, type);
}

/* infer, but a failure leaves no diagnostic. */
static const Core *try_infer(Checker *c, const Term *t, const Value **type) {
  Diag saved = *c->m->diag;
  const Core *k = infer(c, t, type);
  if (k == NULL)
    *c->m->diag = saved;
  return k;
}

static const Core *check_type(Checker *c, const Term *t, uint64_t *level) {
  const Value *type = NULL;
  const Core *k = infer(c, t, &type);
  if (k == NULL)
    return NULL;
  if (type == NULL || type->kind != VAL_UNIV)
    return nul(FAIL(c, "TYPE_MISMATCH", "expected a type, found a term of type %s", show(c, type)));
  *level = type->nat;
  return k;
}

/* Compares the inferred *TYPE with HINT. */
static const Core *finish(Checker *c, const Core *k, const Value *hint, const Value **type) {
  if (k == NULL || *type == NULL)
    return NULL;
  if (hint != NULL && !expect(c, *type, hint))
    return NULL;
  *type = hint != NULL ? hint : *type;
  return k;
}

/* 1 when T is Option A, List A or Sum E A. *ELEM is A. */
static int carrier_of(const Value *t, Carrier *carrier, const Value **elem) {
  if (val_is(t, OP_OPTION) || val_is(t, OP_LIST)) {
    *carrier = val_is(t, OP_OPTION) ? CARRIER_OPTION : CARRIER_LIST;
    *elem = t->args[0];
    return 1;
  }
  if (!val_is(t, OP_SUM))
    return 0;
  *carrier = CARRIER_SUM;
  *elem = t->args[1];
  return 1;
}

/* SHAPE (Option, List or Sum E) with the element ELEM. */
static const Value *carried(Checker *c, const Value *shape, const Value *elem) {
  if (shape == NULL)
    return NULL;
  return val_is(shape, OP_SUM) ? tyop(c, OP_SUM, 2, shape->args[0], elem, NULL) : tyop(c, shape->op, 1, elem, NULL, NULL);
}

/* Checks T as a function from DOM. *RESULT is WANT, or the result type that
   T infers when WANT is NULL. */
static const Core *fn_into(Checker *c, const Term *t, const Value *dom, const Value *want, const Value **result) {
  const Value *ft = NULL;
  const Core *f;
  if (want != NULL) {
    *result = want;
    return check(c, t, val_arrow(c->m, dom, want));
  }
  f = infer(c, t, &ft);
  if (f == NULL)
    return NULL;
  if (ft == NULL || ft->kind != VAL_PI)
    return nul(FAIL(c, "TYPE_NOT_FUNCTION", "expected a function, found a term of type %s", show(c, ft)));
  *result = closure_apply(c->m, ft, val_var(c->m, c->level));
  /* Check dependence before the result leaves its binder. */
  if (!conv_values(c->m, c->level + 2u, *result,
                   closure_apply(c->m, ft, val_var(c->m, c->level + 1u))))
    return nul(FAIL(c, "TYPE_MISMATCH", "this operation needs a function with a non-dependent result type"));
  return expect(c, ft, val_arrow(c->m, dom, *result)) ? f : NULL;
}

/* The argument type of the function T, from its binder or its type. */
static const Value *lam_domain(Checker *c, const Term *t) {
  uint64_t level = 0;
  const Value *ft = NULL;
  if (t->kind == TERM_LAM && t->left != NULL)
    return here(c, check_type(c, t->left, &level));
  return try_infer(c, t, &ft) != NULL && ft != NULL && ft->kind == VAL_PI ? ft->dom : NULL;
}

/* The carried argument ARG of map, bind or filter. When ARG does not infer
   (`inr 3`), its type comes from the expected type and from the binder of
   the function FN. */
static const Core *carried_arg(Call *k, const Term *arg, const Term *fn, const Value **type) {
  Checker *c = k->c;
  Carrier carrier = CARRIER_OPTION;
  const Value *elem = NULL;
  const Value *dom;
  const Core *x = try_infer(c, arg, type);
  if (x != NULL)
    return x;
  if (!carrier_of(k->hint, &carrier, &elem))
    return infer(c, arg, type);
  dom = lam_domain(c, fn);
  if (dom == NULL)
    return infer(c, arg, type);
  *type = carried(c, k->hint, dom);
  return check(c, arg, *type);
}

static const Core *need_type(Call *k, const char *what) {
  return nul(FAIL(k->c, "TYPE_INFER", "%s needs a declared type %s here", k->b->name, what));
}

static const Core *not_carrier(Call *k, const Value *type) {
  return nul(FAIL(k->c, "TYPE_MISMATCH", "%s needs a value of type Option A, List A or Sum E A, found a term of type %s", k->b->name, show(k->c, type)));
}

/* Infers the argument I of K as a path. */
static const Core *infer_path(Call *k, uint32_t i, const Value **type) {
  const Core *p = infer(k->c, k->args[i], type);
  if (p == NULL)
    return NULL;
  return *type != NULL && (*type)->kind == VAL_PATHP ? p : nul(FAIL(k->c, "TYPE_MISMATCH", "%s needs a path of type PathP L a b, found a term of type %s", k->b->name, show(k->c, *type)));
}

static const Core *rule_type1(Call *k) {
  uint64_t level = 0;
  const Core *a = check_type(k->c, k->args[0], &level);
  k->type = val_univ(k->c->m, level);
  return op3(k->c, k->b->op, 0, 1, a, NULL, NULL);
}

static const Core *rule_type2(Call *k) {
  uint64_t la = 0;
  uint64_t lb = 0;
  const Core *a = check_type(k->c, k->args[0], &la);
  const Core *b = a == NULL ? NULL : check_type(k->c, k->args[1], &lb);
  k->type = val_univ(k->c->m, la > lb ? la : lb);
  return op3(k->c, k->b->op, 0, 2, a, b, NULL);
}

/* The line of PathP: `fun (i : I) => A` or `fun i => A`, with A a type of
   *LEVEL. The core is A, under the interval binder. */
static const Core *check_line(Checker *c, const Term *t, uint64_t *level) {
  const Core *body;
  if (t->kind != TERM_LAM || (t->left != NULL && !is_interval(c, t->left)))
    return nul(FAIL(c, "TYPE_MISMATCH", "the line of PathP must be an interval abstraction fun (i : I) => A"));
  if (!push_interval(c, t->name))
    return NULL;
  body = check_type(c, t->right, level);
  pop(c);
  return body;
}

/* PathP (fun i => A) a b, with a : A[i0] and b : A[i1] (rule R12). The
   level of A is the level of the path type. */
static const Core *rule_pathp(Call *k) {
  Checker *c = k->c;
  uint64_t level = 0;
  const Core *line = check_line(c, k->args[0], &level);
  const Core *a = line == NULL ? NULL : check(c, k->args[1], at(c, line, val_iend(c->m, 0)));
  const Core *b = a == NULL ? NULL : check(c, k->args[2], at(c, line, val_iend(c->m, 1)));
  k->type = val_univ(c->m, level);
  return core_ends(c, CORE_PATHP, k->args[0]->name, NULL, line, a, b);
}

/* Path A a b is PathP (fun _ => A) a b. */
static const Core *rule_path(Call *k) {
  Checker *c = k->c;
  uint64_t level = 0;
  const Core *t = check_type(c, k->args[0], &level);
  const Value *tv = here(c, t);
  const Core *a = check(c, k->args[1], tv);
  const Core *b = a == NULL ? NULL : check(c, k->args[2], tv);
  k->type = val_univ(c->m, level);
  return core_ends(c, CORE_PATHP, "_", NULL, b == NULL ? NULL : quote_value(c->m, c->level + 1u, tv), a, b);
}

static const Core *rule_pair(Call *k) {
  Checker *c = k->c;
  const Value *ta = val_is(k->hint, OP_PROD) ? k->hint->args[0] : NULL;
  const Value *tb = val_is(k->hint, OP_PROD) ? k->hint->args[1] : NULL;
  const Core *a = ta != NULL ? check(c, k->args[0], ta) : infer(c, k->args[0], &ta);
  const Core *b = a == NULL ? NULL : (tb != NULL ? check(c, k->args[1], tb) : infer(c, k->args[1], &tb));
  k->type = tyop(c, OP_PROD, 2, ta, tb, NULL);
  return op3(c, OP_PAIR, 0, 2, a, b, NULL);
}

static const Core *rule_inj(Call *k) {
  if (!val_is(k->hint, OP_SUM))
    return need_type(k, "Sum A B");
  k->type = k->hint;
  return op3(k->c, k->b->op, 0, 1, check(k->c, k->args[0], k->hint->args[k->b->part]), NULL, NULL);
}

static const Core *rule_empty(Call *k) {
  if (!val_is(k->hint, k->b->result))
    return need_type(k, k->b->result == OP_OPTION ? "Option A" : k->b->result == OP_KMAP ? "Map K V" : "List A");
  k->type = k->hint;
  return op3(k->c, k->b->op, 0, 0, NULL, NULL, NULL);
}

static const Core *rule_some(Call *k) {
  Checker *c = k->c;
  const Value *a = val_is(k->hint, OP_OPTION) ? k->hint->args[0] : NULL;
  const Core *x = a != NULL ? check(c, k->args[0], a) : infer(c, k->args[0], &a);
  k->type = tyop(c, OP_OPTION, 1, a, NULL, NULL);
  return op3(c, OP_SOME, 0, 1, x, NULL, NULL);
}

static const Core *rule_cons(Call *k) {
  Checker *c = k->c;
  const Value *a = val_is(k->hint, OP_LIST) ? k->hint->args[0] : NULL;
  const Core *x = a != NULL ? check(c, k->args[0], a) : infer(c, k->args[0], &a);
  const Value *list = tyop(c, OP_LIST, 1, a, NULL, NULL);
  const Core *xs = x == NULL ? NULL : check(c, k->args[1], list);
  k->type = list;
  return op3(c, OP_CONS, 0, 2, x, xs, NULL);
}

static const Core *rule_pack(Call *k) {
  Checker *c = k->c;
  const Value *s = k->hint;
  const Core *w;
  const Core *p;
  if (s == NULL || s->kind != VAL_SIGMA)
    return need_type(k, "Sigma (x : A) B");
  w = check(c, k->args[0], s->dom);
  p = w == NULL ? NULL : check(c, k->args[1], closure_apply(c->m, s, here(c, w)));
  k->type = s;
  return op3(c, OP_PACK, 0, 2, w, p, NULL);
}

/* refl against PathP L a b is fun _ => a. The line must be constant. */
static const Core *rule_refl(Call *k) {
  Checker *c = k->c;
  const Value *e = k->hint;
  if (e == NULL || e->kind != VAL_PATHP)
    return need_type(k, "PathP L a b");
  if (!conv_values(c->m, c->level + 1u, closure_apply(c->m, e, val_ivar(c->m, c->level)), closure_apply(c->m, e, val_iend(c->m, 0))))
    return nul(FAIL(c, "TYPE_MISMATCH", "refl needs a path type with a constant line, found %s", show(c, e)));
  if (!path_ends(c, e, e->args[0], e->args[0]))
    return NULL;
  k->type = e;
  return core_bind(c, CORE_PLAM, "_", NULL, quote_value(c->m, c->level + 1u, e->args[0]));
}

static const Core *rule_part(Call *k) {
  Checker *c = k->c;
  const Value *pt = NULL;
  const Core *p = infer(c, k->args[0], &pt);
  if (p == NULL)
    return NULL;
  if (!val_is(pt, OP_PROD))
    return nul(FAIL(c, "TYPE_MISMATCH", "%s needs a pair, found a term of type %s", k->b->name, show(c, pt)));
  k->type = pt->args[k->b->part];
  return op3(c, k->b->op, 0, 1, p, NULL, NULL);
}

static const Core *rule_sigma_part(Call *k) {
  Checker *c = k->c;
  const Value *st = NULL;
  const Core *s = infer(c, k->args[0], &st);
  const Core *w;
  if (s == NULL)
    return NULL;
  if (st == NULL || st->kind != VAL_SIGMA)
    return nul(FAIL(c, "TYPE_MISMATCH", "%s needs a Sigma value, found a term of type %s", k->b->name, show(c, st)));
  w = op3(c, OP_WITNESS, 0, 1, s, NULL, NULL);
  k->type = k->b->part == 0u ? st->dom : closure_apply(c->m, st, here(c, w));
  return k->b->part == 0u ? w : op3(c, OP_PAYLOAD, 0, 1, s, NULL, NULL);
}

/* either f g s */
static const Core *rule_either(Call *k) {
  Checker *c = k->c;
  const Value *st = NULL;
  const Value *res = NULL;
  const Core *s = infer(c, k->args[2], &st);
  const Core *f;
  const Core *g;
  if (s == NULL)
    return NULL;
  if (!val_is(st, OP_SUM))
    return nul(FAIL(c, "TYPE_MISMATCH", "either needs a value of type Sum A B, found a term of type %s", show(c, st)));
  f = fn_into(c, k->args[0], st->args[0], k->hint, &res);
  g = f == NULL ? NULL : check(c, k->args[1], val_arrow(c->m, st->args[1], res));
  k->type = res;
  return op3(c, OP_EITHER, 0, 3, f, g, s);
}

/* option z f o */
static const Core *rule_option(Call *k) {
  Checker *c = k->c;
  const Value *ot = NULL;
  const Value *res = k->hint;
  const Core *o = infer(c, k->args[2], &ot);
  const Core *z;
  const Core *f;
  if (o == NULL)
    return NULL;
  if (!val_is(ot, OP_OPTION))
    return nul(FAIL(c, "TYPE_MISMATCH", "option needs a value of type Option A, found a term of type %s", show(c, ot)));
  z = res != NULL ? check(c, k->args[0], res) : infer(c, k->args[0], &res);
  f = z == NULL ? NULL : check(c, k->args[1], val_arrow(c->m, ot->args[0], res));
  k->type = res;
  return op3(c, OP_OPTION_ELIM, 0, 3, z, f, o);
}

/* The type of case I of elim with the motive PV on a value of type ST:
   (a : A) -> P (inl a), (b : B) -> P (inr b), or P unit. */
static const Value *elim_case(Checker *c, const Value *pv, const Value *st, uint32_t i) {
  const Value *x = val_var(c->m, c->level);
  if (val_is(st, OP_UNIT))
    return apply_value(c->m, pv, tyop(c, OP_UNIT_VAL, 0, NULL, NULL, NULL));
  return here(c, core_bind(c, CORE_PI, i == 0u ? "a" : "b", quote_value(c->m, c->level, st->args[i]),
                           quote_value(c->m, c->level + 1u, apply_value(c->m, pv, tyop(c, i == 0u ? OP_INL : OP_INR, 1, x, NULL, NULL)))));
}

/* A printed motive has no binder annotation. Its domain can instead come
   from the scrutinee at the Unit or Sum argument position. */
static const Value *elim_domain(Checker *c, const Term *const *args, uint32_t argc) {
  const Value *dom = lam_domain(c, args[0]);
  uint32_t i;
  if (dom != NULL || args[0]->kind != TERM_LAM || args[0]->left != NULL)
    return dom;
  /* A domain family outside a block (S7 B1): the scrutinee follows one
     case for each constructor. */
  for (i = 2u; i < argc; i++) {
    if (try_infer(c, args[i], &dom) != NULL && val_is(dom, OP_FAMILY) && c->m->families[dom->inst].block_count == 1u && i == c->m->families[dom->inst].ctor_count + 1u)
      return dom;
  }
  for (i = 2u; i < argc && i < 4u; i++) {
    if (try_infer(c, args[i], &dom) != NULL && val_is(dom, i == 2u ? OP_UNIT : OP_SUM))
      return dom;
  }
  return NULL;
}

/* The last domain S of the motive type PT = (x1 : X1) -> .. -> (s : S) ->
   Type k, with *D the number of domains before S (S7 B1). */
static const Value *motive_domain(Checker *c, const Value *pt, uint32_t *d) {
  const Value *v = pt;
  uint32_t n;
  for (n = 0; v != NULL && v->kind == VAL_PI; n++) {
    const Value *cod = closure_apply(c->m, v, val_var(c->m, c->level + n));
    if (cod != NULL && cod->kind == VAL_UNIV) {
      *d = n;
      return v->dom;
    }
    v = cod;
  }
  return vnul(FAIL(c, "TYPE_MISMATCH", "elim needs a motive of type X -> Type k, found a term of type %s", show(c, pt)));
}

/* A printed motive over the domain family of ST (S7 B1): one bare binder
   for each index, then one for the value. VALS holds the parameters, then
   the index variables before J. *TYPE gets the core of the motive type. */
static const Core *bare_motive(Checker *c, const Term *t, const Value *st, const Value **vals, uint32_t j, const Core **type) {
  Machine *m = c->m;
  const FamilyInfo *f = &m->families[st->inst];
  uint32_t p = f->param_count;
  uint64_t level = 0;
  const Value *dom;
  const Core *domc;
  const Core *ann = NULL;
  const Core *body;
  const Core *cod = NULL;
  *type = NULL;
  if (t->kind != TERM_LAM)
    return nul(FAIL(c, "TYPE_MISMATCH", "the motive of elim on %s needs one binder for each of its %u indices, then one for the value", f->name, (unsigned)f->index_count));
  dom = j < f->index_count ? eval_core(m, env_of(c, vals, p + j), f->index_types[j]) : val_op(m, VAL_OP, OP_FAMILY, st->inst, 0, vals, p + j);
  if (t->left != NULL && (ann = checked_annotation(c, t->left, dom)) == NULL)
    return NULL;
  domc = quote_value(m, c->level, dom);
  if (domc == NULL || !push(c, t->name, dom))
    return NULL;
  if (j < f->index_count)
    vals[p + j] = val_var(m, c->level - 1u);
  body = j < f->index_count ? bare_motive(c, t->right, st, vals, j + 1u, &cod) : check_type(c, t->right, &level);
  if (body != NULL && j == f->index_count)
    cod = quote_value(m, c->level, val_univ(m, level));
  pop(c);
  *type = cod == NULL ? NULL : core_bind(c, CORE_PI, t->name, domc, cod);
  return body == NULL || *type == NULL ? NULL : core_bind(c, CORE_LAM, t->name, ann, body);
}

static const Core *elim_on_family(Call *k, const Core *motive, const Value *pt, const Value *st, uint32_t d);

/* elim P c1 .. cn x (USER ruling S4): the dependent eliminator of Sum A B
   (two cases) and Unit (one case), with the motive P : X -> Type k and the
   type P x. A domain family outside a block goes to elim_on_family (S7
   B1, O21). Nat, List or a Path has no elim (no K). */
static const Core *rule_elim(Call *k) {
  Checker *c = k->c;
  const Value *st = elim_domain(c, k->args, k->argc);
  const Value *pt = NULL;
  const Value *pv;
  const Core *cores[4] = {NULL, NULL, NULL, NULL};
  const Core *s;
  uint32_t want;
  uint32_t d = 0;
  uint32_t i;
  if (st != NULL && val_is(st, OP_FAMILY) && k->args[0]->kind == TERM_LAM && k->args[0]->left == NULL) {
    const Value **vals = arena_alloc(c->m->arena, ((size_t)st->argc + 1u) * sizeof *vals);
    const Core *ty = NULL;
    for (i = 0; vals != NULL && i < st->argc; i++)
      vals[i] = st->args[i];
    cores[0] = vals == NULL ? nul(FAIL(c, "OOM", "out of memory")) : bare_motive(c, k->args[0], st, vals, 0, &ty);
    pt = here(c, ty);
  } else if (st != NULL && k->args[0]->kind == TERM_LAM && k->args[0]->left == NULL) {
    uint64_t level = 0;
    const Core *body;
    if (!push(c, k->args[0]->name, st))
      return NULL;
    body = check_type(c, k->args[0]->right, &level);
    pop(c);
    cores[0] = core_bind(c, CORE_LAM, k->args[0]->name, NULL, body);
    pt = val_arrow(c->m, st, val_univ(c->m, level));
  } else {
    cores[0] = infer(c, k->args[0], &pt);
  }
  if (cores[0] == NULL)
    return NULL;
  st = motive_domain(c, pt, &d);
  if (st == NULL)
    return NULL;
  if (val_is(st, OP_FAMILY) && c->m->families[st->inst].block_count == 1u)
    return elim_on_family(k, cores[0], pt, st, d);
  if (d != 0u || (!val_is(st, OP_SUM) && !val_is(st, OP_UNIT)))
    return nul(FAIL(c, "TYPE_MISMATCH", "elim needs a motive on Sum A B, Unit or a domain family outside a block, found a motive of type %s", show(c, pt)));
  want = val_is(st, OP_SUM) ? 2u : 1u;
  if (k->argc < want + 2u)
    return nul(FAIL(c, "TYPE_MISMATCH", "elim on %s needs a motive, %u cases and a scrutinee", show(c, st), want));
  s = check(c, k->args[want + 1u], st);
  if (s == NULL)
    return NULL;
  pv = here(c, cores[0]);
  for (i = 0; i < want && cores[i] != NULL; i++)
    cores[i + 1u] = check(c, k->args[i + 1u], elim_case(c, pv, st, i));
  if (cores[want] == NULL)
    return NULL;
  cores[want + 1u] = s;
  k->type = apply_value(c->m, pv, here(c, s));
  k->used = want + 2u;
  return core_op(c, OP_ELIM, want, 0, cores, want + 2u);
}

static const Core *rule_pure(Call *k) {
  Carrier carrier = CARRIER_OPTION;
  const Value *a = NULL;
  if (!carrier_of(k->hint, &carrier, &a))
    return need_type(k, "Option A, List A or Sum E A");
  k->type = k->hint;
  return op3(k->c, OP_PURE, carrier, 1, check(k->c, k->args[0], a), NULL, NULL);
}

/* map f m */
static const Core *rule_map(Call *k) {
  Checker *c = k->c;
  Carrier carrier = CARRIER_OPTION;
  Carrier hint_carrier = CARRIER_OPTION;
  const Value *mt = NULL;
  const Value *a = NULL;
  const Value *b = NULL;
  const Value *hint_elem = NULL;
  const Core *x = carried_arg(k, k->args[1], k->args[0], &mt);
  const Core *f;
  if (x == NULL)
    return NULL;
  if (!carrier_of(mt, &carrier, &a))
    return not_carrier(k, mt);
  f = fn_into(c, k->args[0], a, carrier_of(k->hint, &hint_carrier, &hint_elem) ? hint_elem : NULL, &b);
  k->type = carried(c, mt, b);
  return op3(c, OP_MAP, carrier, 2, f, x, NULL);
}

/* bind m f */
static const Core *rule_bind(Call *k) {
  Checker *c = k->c;
  Carrier carrier = CARRIER_OPTION;
  Carrier result_carrier = CARRIER_OPTION;
  const Value *mt = NULL;
  const Value *a = NULL;
  const Value *b = NULL;
  const Value *res = NULL;
  const Core *x = carried_arg(k, k->args[0], k->args[1], &mt);
  const Core *f;
  if (x == NULL)
    return NULL;
  if (!carrier_of(mt, &carrier, &a))
    return not_carrier(k, mt);
  f = fn_into(c, k->args[1], a, k->hint, &res);
  if (f == NULL)
    return NULL;
  if (!carrier_of(res, &result_carrier, &b) || result_carrier != carrier)
    return nul(FAIL(c, "TYPE_MISMATCH", "bind needs a function into the carrier of %s, found one into %s", show(c, mt), show(c, res)));
  if (carrier == CARRIER_SUM && !conv_values(c->m, c->level, mt->args[0], res->args[0]))
    return nul(FAIL(c, "TYPE_MISMATCH", "bind must preserve the Sum error type %s, found %s", show(c, mt->args[0]), show(c, res->args[0])));
  k->type = res;
  return op3(c, OP_BIND, carrier, 2, x, f, NULL);
}

/* filter p m */
static const Core *rule_filter(Call *k) {
  Checker *c = k->c;
  Carrier carrier = CARRIER_OPTION;
  const Value *mt = NULL;
  const Value *a = NULL;
  const Core *x = carried_arg(k, k->args[1], k->args[0], &mt);
  const Core *p;
  if (x == NULL)
    return NULL;
  if (!carrier_of(mt, &carrier, &a) || carrier == CARRIER_SUM)
    return nul(FAIL(c, "TYPE_MISMATCH", "filter needs a value of type Option A or List A, found a term of type %s", show(c, mt)));
  p = check(c, k->args[0], val_arrow(c->m, a, flag_type(c)));
  k->type = mt;
  return op3(c, OP_FILTER, carrier, 2, p, x, NULL);
}

/* fold step z n (Nat) or fold f z xs (List A). */
static const Core *fold_simple(Call *k, const Core *s, const Value *st) {
  Checker *c = k->c;
  const Value *res = k->hint;
  const Core *z = res != NULL ? check(c, k->args[1], res) : infer(c, k->args[1], &res);
  int list = val_is(st, OP_LIST);
  const Value *step = list ? val_arrow(c->m, st->args[0], val_arrow(c->m, res, res)) : val_arrow(c->m, res, res);
  const Core *f = z == NULL ? NULL : check(c, k->args[0], step);
  k->used = 3u;
  k->type = res;
  return op3(c, list ? OP_FOLD_LIST : OP_FOLD_NAT, 0, 3, f, z, s);
}

/* The Pi telescope of the fold case of CTOR around BODY, a core at the
   level c->level + field_count: one argument for each field (a recursive
   field arrives folded, so its type is RES). Field k is the variable at
   level c->level + k, its type at the parameters ENV and the earlier
   fields (S4 item 13). A path field `Path F x y` arrives folded, so its
   type is `Path RES x' y'` (S4 item 14). */
/* The folded type of a recursive field of type T: the target of its sort
   in a block fold (S6), else RES. */
static const Value *sort_res(const Checker *c, const Core *t, const Value *res) {
  return c->fold_targets != NULL && t != NULL && t->kind == CORE_OP && t->op == OP_FAMILY && t->inst >= c->fold_first && t->inst - c->fold_first < c->fold_count ? c->fold_targets[t->inst - c->fold_first] : res;
}

/* The binder position of the field I in a case. An elim case (S7 B1) has
   the IH of each recursive or path field after the field. */
static uint32_t field_pos(const Checker *c, const CtorInfo *ctor, uint32_t i) {
  uint32_t pos = i;
  uint32_t j;
  for (j = 0; j < i && c->elim_motive != NULL; j++)
    pos += ctor->fields[j].recursive || ctor->fields[j].path ? 1u : 0u;
  return pos;
}

/* The motive PV at the indices of the family type T, then at X (S7 B1). */
static const Value *motive_at(Checker *c, const Value *pv, const Value *t, const Value *x) {
  const Value *acc = pv;
  uint32_t i;
  if (t == NULL || x == NULL || pv == NULL)
    return NULL;
  if (!val_is(t, OP_FAMILY))
    return vnul(FAIL(c, "TYPE_MISMATCH", "an elim case needs a value of a domain family here"));
  for (i = c->m->families[t->inst].param_count; i < t->argc && acc != NULL; i++)
    acc = apply_value(c->m, acc, t->args[i]);
  return acc == NULL ? NULL : apply_value(c->m, acc, x);
}

/* The IH of the end E of a path field in an elim case: the variable after
   E (S7 B1). */
static const Value *ih_end(Checker *c, const Value *e) {
  return e != NULL && e->kind == VAL_VAR && e->nat >= c->level ? val_var(c->m, e->nat + 1u) : NULL;
}

/* The IH type of the recursive or path field FIELD of CTOR at the level AT,
   a core at AT + 1 (S7 B1): P idx x for a recursive field x, and PathP (fun
   i => P idx (x @ i)) ih0 ih1 for a path field, with ih0 and ih1 the IHs of
   its ends. ENV holds the parameters and the earlier fields. */
static const Core *ih_type(Checker *c, const CtorInfo *ctor, const FieldInfo *field, const Env *env, uint32_t at) {
  Machine *m = c->m;
  const Value *x = val_var(m, at);
  const Value *r = val_ivar(m, at + 1u);
  const Env *inner = env_push(m, env, r);
  uint32_t e0 = c->level + field_pos(c, ctor, field->ends[0]);
  uint32_t e1 = c->level + field_pos(c, ctor, field->ends[1]);
  const Value *line;
  if (!field->path)
    return quote_value(m, at + 1u, motive_at(c, c->elim_motive, eval_core(m, env, field->type), x));
  line = inner == NULL ? NULL : motive_at(c, c->elim_motive, eval_core(m, inner, field->type->b), path_apply(m, x, r, val_var(m, e0), val_var(m, e1)));
  return line == NULL ? NULL : core_ends(c, CORE_PATHP, "_", NULL, quote_value(m, at + 2u, line), quote_value(m, at + 1u, val_var(m, e0 + 1u)), quote_value(m, at + 1u, val_var(m, e1 + 1u)));
}

/* The motive at the constructor INST at the parameters of ST, the field
   variables at their elim case positions and the interval values R0 and
   R1, at the result indices of INST (S7 B1). */
static const Value *elim_res(Checker *c, uint32_t inst, const Value *st, const Value *r0, const Value *r1) {
  Machine *m = c->m;
  const CtorInfo *ctor = &m->ctors[inst];
  const FamilyInfo *f = &m->families[ctor->family];
  uint32_t p = f->param_count;
  uint32_t n = ctor->field_count;
  uint32_t d = ctor->interval_count;
  uint32_t q = ctor->result == NULL ? p : p + f->index_count;
  const Value **a = arena_alloc(m->arena, ((size_t)p + n + d + 1u) * sizeof *a);
  const Value **t = arena_alloc(m->arena, ((size_t)q + 1u) * sizeof *t);
  const Env *env = env_of(c, st->args, p);
  uint32_t i;
  int ok = 1;
  if (a == NULL || t == NULL)
    return vnul(FAIL(c, "OOM", "out of memory"));
  for (i = 0; i < p; i++) {
    a[i] = st->args[i];
    t[i] = st->args[i];
  }
  /* An empty ENV is NULL (a family without parameters), so only a failed
     push stops the loops. */
  for (i = 0; i < n && ok; i++) {
    a[p + i] = val_var(m, c->level + field_pos(c, ctor, i));
    env = env_push(m, env, a[p + i]);
    ok = env != NULL;
  }
  for (i = p; i < q && ok; i++) {
    t[i] = eval_core(m, env, ctor->result[i - p]);
    ok = t[i] != NULL;
  }
  if (!ok)
    return NULL;
  if (d > 0u)
    a[p + n] = r0;
  if (d > 1u)
    a[p + n + 1u] = r1;
  return motive_at(c, c->elim_motive, val_op(m, VAL_OP, OP_FAMILY, ctor->family, 0, t, q), val_ctor(m, inst, a, p + n + d));
}

/* The line of a path case at R0 and R1: RES in a fold, the motive at the
   constructor in an elim (S7 B1). */
static const Value *line_res(Checker *c, uint32_t inst, const Value *st, const Value *res, const Value *r0, const Value *r1) {
  return c->elim_motive == NULL ? res : elim_res(c, inst, st, r0, r1);
}

static const Core *case_fields(Checker *c, const CtorInfo *ctor, const Env *env, const Value *res, const Core *body) {
  Machine *m = c->m;
  uint32_t n = ctor->field_count;
  const Core **types = arena_alloc(m->arena, ((size_t)n + 1u) * sizeof *types);
  const Core **ihs = arena_alloc(m->arena, ((size_t)n + 1u) * sizeof *ihs);
  const Core *t = body;
  uint32_t i;
  if (body == NULL)
    return NULL;
  if (types == NULL || ihs == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  for (i = 0; i < n; i++) {
    const FieldInfo *field = &ctor->fields[i];
    uint32_t at = c->level + field_pos(c, ctor, i);
    int ih = c->elim_motive != NULL && (field->recursive || field->path);
    /* A recursive field at an index over an earlier recursive field (S6)
       arrives folded: the fold is not dependent, so the index goes. An
       elim case (S7 B1) keeps the field. */
    if (c->elim_motive == NULL && field->over_recursive && !field->path && !field->recursive)
      return nul(FAIL(c, "HIT_BOUNDARY", "the fold of %s has no case type for the field %s: a field over an earlier recursive field must be Path F x y, with x and y earlier recursive fields", ctor->name, field->name));
    if (c->elim_motive != NULL)
      types[i] = quote_value(m, at, eval_core(m, env, field->type));
    else
      types[i] = field->path ? core_ends(c, CORE_PATHP, "_", NULL, quote_value(m, at + 1u, sort_res(c, field->type->b, res)), quote_value(m, at, val_var(m, c->level + field->ends[0])), quote_value(m, at, val_var(m, c->level + field->ends[1]))) : quote_value(m, at, field->recursive ? sort_res(c, field->type, res) : eval_core(m, env, field->type));
    ihs[i] = ih ? ih_type(c, ctor, field, env, at) : NULL;
    env = env_push(m, env, val_var(m, at));
    if (types[i] == NULL || env == NULL || (ih && ihs[i] == NULL))
      return NULL;
  }
  for (i = n; i > 0u && t != NULL; i--) {
    t = ihs[i - 1u] == NULL ? t : core_bind(c, CORE_PI, "ih", ihs[i - 1u], t);
    t = t == NULL ? NULL : core_bind(c, CORE_PI, ctor->fields[i - 1u].name, types[i - 1u], t);
  }
  return t;
}

/* The type of the fold case of CTOR: the fields, then RES. */
static const Value *case_type(Checker *c, const CtorInfo *ctor, const Env *env, const Value *res) {
  return here(c, case_fields(c, ctor, env, res, quote_value(c->m, c->level + ctor->field_count, res)));
}

/* The fold image of V, a boundary value of a path constructor of FAM at the
   field variables (S4 R4 (a)). A field variable (a level from c->level up)
   is its folded variable; a point constructor is its case, one of the DONE
   cases in CORES, at the images of its fields. Path fields are mapped
   under a fresh interval binder. ENV and LEVEL include the boundary
   fields and surrounding interval binders to preserve captures. */
static const Value *fold_image(Checker *c, uint32_t fam, const Core *const *cores, uint32_t done, const Value *v, const Env *env, uint32_t level) {
  Machine *m = c->m;
  const FamilyInfo *f = &m->families[fam];
  /* The constructor slots of a block are contiguous (S6). */
  uint32_t base = m->families[f->block_first].first_ctor;
  const CtorInfo *ci;
  const Value *acc;
  uint32_t i;
  if (v == NULL)
    return NULL;
  /* In an elim case (S7 B1) the IH of the field at L is at L + 1. */
  if (v->kind == VAL_VAR && v->nat >= c->level)
    return c->elim_motive == NULL ? v : val_var(m, v->nat + 1u);
  /* p @ r for a path field p (dimension 2, S4 item 15): the folded p has
     the same level, and its ends are earlier recursive fields. In an elim
     case it is the IH path at r. */
  if (v->kind == VAL_PAPP && v->dom != NULL && v->dom->kind == VAL_VAR && v->dom->nat >= c->level)
    return c->elim_motive == NULL ? v : path_apply(m, val_var(m, v->dom->nat + 1u), v->arg, ih_end(c, v->argc == 2u ? v->args[0] : NULL), ih_end(c, v->argc == 2u ? v->args[1] : NULL));
  if (!val_is(v, OP_CTOR) || v->inst < base || v->inst - base >= done || m->ctors[v->inst].interval_count != 0u)
    return vnul(FAIL(c, "HIT_BOUNDARY", "the fold cannot map the boundary term %s: a boundary term must be a field, a path field at an interval, or an earlier point constructor", show(c, v)));
  ci = &m->ctors[v->inst];
  acc = here(c, cores[v->inst - base]);
  for (i = 0; i < ci->field_count && acc != NULL; i++) {
    const FieldInfo *field = &ci->fields[i];
    const Value *arg = v->args[f->param_count + i];
    /* An elim case takes the field, then its IH (S7 B1). */
    if (c->elim_motive != NULL && (field->recursive || field->path))
      acc = apply_value(m, acc, arg);
    if (field->recursive)
      arg = fold_image(c, fam, cores, done, arg, env, level);
    else if (field->path) {
      const Value *r = val_ivar(m, level);
      const Env *inner = env_push(m, env, r);
      const Value *at = inner == NULL ? NULL : path_apply(m, arg, r, v->args[f->param_count + field->ends[0]], v->args[f->param_count + field->ends[1]]);
      const Value *image = at == NULL ? NULL : fold_image(c, fam, cores, done, at, inner, level + 1u);
      arg = image == NULL ? NULL : val_closure(m, VAL_PLAM, "_", NULL, env, quote_value(m, level + 1u, image));
    }
    acc = arg == NULL || acc == NULL ? NULL : apply_value(m, acc, arg);
  }
  return acc;
}

/* The fold image of the constructor INST at the parameters of ST, the
   field variables (field k at level c->level + k) and the interval values
   R0 (and R1 at dimension 2). At an end it is the image of the boundary. */
static const Value *case_image(Checker *c, uint32_t inst, const Value *st, const Core *const *cores, uint32_t done, const Value *r0, const Value *r1, uint32_t level) {
  Machine *m = c->m;
  const CtorInfo *ctor = &m->ctors[inst];
  uint32_t p = m->families[ctor->family].param_count;
  uint32_t n = ctor->field_count;
  uint32_t d = ctor->interval_count;
  uint32_t b = field_pos(c, ctor, n);
  const Value **a = arena_alloc(m->arena, ((size_t)p + n + d) * sizeof *a);
  const Env *env = c->env;
  uint32_t i;
  if (a == NULL)
    return vnul(FAIL(c, "OOM", "out of memory"));
  for (i = 0; i < p; i++)
    a[i] = st->args[i];
  /* An elim case (S7 B1) has the IH binders between the fields. */
  for (i = 0; i < n; i++)
    a[p + i] = val_var(m, c->level + field_pos(c, ctor, i));
  for (i = 0; i < b; i++) {
    env = env_push(m, env, val_var(m, c->level + i));
    if (env == NULL)
      return NULL;
  }
  for (i = c->level + b; i < level; i++) {
    env = env_push(m, env, val_ivar(m, i));
    if (env == NULL)
      return NULL;
  }
  a[p + n] = r0;
  if (d == 2u)
    a[p + n + 1u] = r1;
  return fold_image(c, ctor->family, cores, done, val_ctor(m, inst, a, p + n + d), env, level);
}

/* The square type of the fold case of INST at dimension 2 (S4 item 15), a
   core at L = c->level + field_count:
   PathP (fun (i : I) => PathP (fun (_ : I) => RES) X(i) Y(i)) P Q,
   with X(i) and Y(i) the images at (i, i0) and (i, i1), and P and Q the
   paths fun (j : I) => the image at (i0, j) and (i1, j). For squash0 it is
   Path (Path RES x' y') p' q' (with path eta, R11), so the case type is
   isSet RES (R8 (a)). */
static const Core *square_type(Checker *c, uint32_t inst, const Value *st, const Value *res, const Core *const *cores, uint32_t done) {
  Machine *m = c->m;
  uint32_t l = c->level + field_pos(c, &m->ctors[inst], m->ctors[inst].field_count);
  const Value *i = val_ivar(m, l);
  const Core *x = quote_value(m, l + 1u, case_image(c, inst, st, cores, done, i, val_iend(m, 0), l + 1u));
  const Core *y = x == NULL ? NULL : quote_value(m, l + 1u, case_image(c, inst, st, cores, done, i, val_iend(m, 1), l + 1u));
  const Core *inner = y == NULL ? NULL : core_ends(c, CORE_PATHP, "_", NULL, quote_value(m, l + 2u, line_res(c, inst, st, res, i, val_ivar(m, l + 1u))), x, y);
  const Core *ends[2];
  int side;
  for (side = 0; side < 2; side++) {
    ends[side] = inner == NULL ? NULL : core_bind(c, CORE_PLAM, "_", NULL, quote_value(m, l + 1u, case_image(c, inst, st, cores, done, val_iend(m, side), i, l + 1u)));
    if (ends[side] == NULL)
      return NULL;
  }
  return core_ends(c, CORE_PATHP, "_", NULL, inner, ends[0], ends[1]);
}

/* The type of the fold case of the path constructor INST: (folded fields)
   -> PathP (fun (_ : I) => RES) B0 B1 at dimension 1, with B0 and B1 the
   fold images of the boundary at i0 and i1, or the square type at
   dimension 2. Field k is the variable at level c->level + k: the field in
   the boundary, its folded value in the image. */
static const Value *path_case_type(Checker *c, uint32_t inst, const Value *st, const Value *res, const Core *const *cores, uint32_t done) {
  Machine *m = c->m;
  const CtorInfo *ctor = &m->ctors[inst];
  const Env *env = env_of(c, st->args, m->families[ctor->family].param_count);
  uint32_t l = c->level + field_pos(c, ctor, ctor->field_count);
  const Core *ends[2];
  const Core *t;
  int side;
  if (ctor->interval_count != 1u && ctor->interval_count != 2u)
    return vnul(FAIL(c, "HIT_BOUNDARY", "a fold of %s, a constructor with %u interval fields, is not in S4: S4 folds dimensions 1 and 2", ctor->name, (unsigned)ctor->interval_count));
  if (ctor->interval_count == 2u)
    return here(c, case_fields(c, ctor, env, res, square_type(c, inst, st, res, cores, done)));
  for (side = 0; side < 2; side++)
    ends[side] = quote_value(m, l, case_image(c, inst, st, cores, done, val_iend(m, side), NULL, l));
  t = ends[0] == NULL || ends[1] == NULL ? NULL : core_ends(c, CORE_PATHP, "_", NULL, quote_value(m, l + 1u, line_res(c, inst, st, res, val_ivar(m, l), NULL)), ends[0], ends[1]);
  return here(c, case_fields(c, ctor, env, res, t));
}

/* fold case... x over a domain family: one case for each constructor. */
static const Core *fold_family(Call *k, const Core *s, const Value *st) {
  Checker *c = k->c;
  Machine *m = c->m;
  const FamilyInfo *f = &m->families[st->inst];
  const Env *env = env_of(c, st->args, f->param_count);
  const Value *res = k->hint;
  const Core **cores = arena_alloc(m->arena, ((size_t)f->ctor_count + 2u) * sizeof *cores);
  uint32_t j;
  if (cores == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  const Value *ct;
  if (res == NULL && m->ctors[f->first_ctor].field_count == 0u && m->ctors[f->first_ctor].interval_count == 0u)
    cores[0] = infer(c, k->args[0], &res);
  if (res == NULL)
    return nul(FAIL(c, "TYPE_INFER", "fold over %s needs a declared result type here", f->name));
  for (j = 0; j < f->ctor_count; j++) {
    ct = cores[j] != NULL ? NULL : m->ctors[f->first_ctor + j].interval_count == 0u ? case_type(c, &m->ctors[f->first_ctor + j], env, res) : path_case_type(c, f->first_ctor + j, st, res, cores, j);
    cores[j] = cores[j] != NULL ? cores[j] : ct == NULL ? NULL : check(c, k->args[j], ct);
    if (cores[j] == NULL)
      return NULL;
  }
  /* The result type RES goes before the scrutinee: the fold of a formal
     hcomp is an hcomp in RES (S4 R3 (a)). */
  cores[f->ctor_count] = quote_value(m, c->level, res);
  cores[f->ctor_count + 1u] = s;
  if (cores[f->ctor_count] == NULL)
    return NULL;
  k->used = f->ctor_count + 1u;
  k->type = res;
  return core_op(c, OP_FOLD_FAMILY, st->inst, 0, cores, f->ctor_count + 2u);
}

/* elim P c1 .. cn x over the domain family of ST outside a block (S7 B1,
   O21): the motive type PT is (idx) -> F params idx -> Type k, with D the
   index count. Each case takes the fields, with the IH of a recursive or
   path field after it. The type is P idx' x. The motive goes in
   c->elim_motive only while a case type is built, not while a case is
   checked, so a fold or elim in a case is not in elim mode. */
static const Core *elim_on_family(Call *k, const Core *motive, const Value *pt, const Value *st, uint32_t d) {
  Checker *c = k->c;
  Machine *m = c->m;
  const FamilyInfo *f = &m->families[st->inst];
  uint32_t p = f->param_count;
  uint32_t want = f->ctor_count;
  const Core **cores = arena_alloc(m->arena, ((size_t)want + 2u) * sizeof *cores);
  const Value *xt = NULL;
  const Value *v = pt;
  const Value *pv;
  const Env *env;
  const Core *s;
  uint32_t j;
  int ok;
  if (cores == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  if (f->index_count != d)
    return nul(FAIL(c, "TYPE_MISMATCH", "elim on %s needs a motive over its %u indices, then a value, found a motive of type %s", f->name, (unsigned)f->index_count, show(c, pt)));
  if (k->argc < want + 2u)
    return nul(FAIL(c, "TYPE_MISMATCH", "elim on %s needs a motive, %u cases and a scrutinee", f->name, (unsigned)want));
  s = infer(c, k->args[want + 1u], &xt);
  if (s == NULL)
    return NULL;
  if (!val_is(xt, OP_FAMILY) || xt->inst != st->inst)
    return nul(FAIL(c, "TYPE_MISMATCH", "elim with a motive on %s needs a scrutinee of %s, found a term of type %s", f->name, f->name, show(c, xt)));
  /* The parameters of the motive domain are the scrutinee parameters; the
     motive takes the index types, then the family at its own indices. */
  env = env_of(c, xt->args, p);
  for (j = 0; j < p; j++) {
    if (!conv_values(m, c->level + d, st->args[j], xt->args[j]))
      return nul(FAIL(c, "TYPE_MISMATCH", "the motive of elim has the type %s, but the scrutinee has the type %s", show(c, pt), show(c, xt)));
  }
  for (j = 0; j < d && v != NULL; j++) {
    const Value *it = eval_core(m, env, f->index_types[j]);
    if (it == NULL || v->kind != VAL_PI || !conv_values(m, c->level + j, v->dom, it))
      return nul(FAIL(c, "TYPE_MISMATCH", "the motive of elim on %s must take its indices, then a value, found a motive of type %s", f->name, show(c, pt)));
    env = env_push(m, env, val_var(m, c->level + j));
    v = env == NULL ? NULL : closure_apply(m, v, val_var(m, c->level + j));
  }
  for (j = 0; j < d && v != NULL; j++) {
    if (!conv_values(m, c->level + d, st->args[p + j], val_var(m, c->level + j)))
      return nul(FAIL(c, "TYPE_MISMATCH", "the motive of elim on %s must take its indices, then a value at those indices, found a motive of type %s", f->name, show(c, pt)));
  }
  pv = v == NULL ? NULL : here(c, motive);
  cores[0] = motive;
  for (j = 0, ok = pv != NULL; ok && j < want; j++) {
    uint32_t inst = f->first_ctor + j;
    const CtorInfo *ctor = &m->ctors[inst];
    const Value *ct;
    c->elim_motive = pv;
    ct = ctor->interval_count != 0u ? path_case_type(c, inst, xt, NULL, cores + 1, j) : here(c, case_fields(c, ctor, env_of(c, xt->args, p), NULL, quote_value(m, c->level + field_pos(c, ctor, ctor->field_count), elim_res(c, inst, xt, NULL, NULL))));
    c->elim_motive = NULL;
    cores[j + 1u] = ct == NULL ? NULL : check(c, k->args[j + 1u], ct);
    ok = cores[j + 1u] != NULL;
  }
  if (!ok)
    return NULL;
  cores[want + 1u] = s;
  k->type = motive_at(c, pv, xt, here(c, s));
  k->used = want + 2u;
  return k->type == NULL ? NULL : core_op(c, OP_ELIM, want, st->inst + 1u, cores, want + 2u);
}

/* The case of the constructor slot J of the block fold of ST into the
   TARGETS of the sorts from FIRST (S6). The constructor slots start at
   BASE; CORES holds the TOTAL cases. */
static int block_case(Call *k, const Value *st, const Value *const *targets, uint32_t first, uint32_t base, const Core **cores, uint32_t total, uint32_t j) {
  Checker *c = k->c;
  Machine *m = c->m;
  const CtorInfo *ctor = &m->ctors[base + j];
  const Value *res = targets[ctor->family - first];
  const Value *ct;
  c->fold_targets = targets;
  c->fold_first = first;
  c->fold_count = m->families[first].block_count;
  ct = ctor->interval_count == 0u ? case_type(c, ctor, env_of(c, st->args, m->families[first].param_count), res) : path_case_type(c, base + j, st, res, cores, total);
  c->fold_targets = NULL;
  cores[j] = ct == NULL ? NULL : check(c, k->args[c->fold_count + j], ct);
  return cores[j] != NULL;
}

/* fold T1 .. Tn case... x over a block of n sorts (S6): one target type
   for each sort, then one case for each constructor of each sort in slot
   order, then x. A recursive field of a sort arrives folded at the target
   of that sort. The point cases are checked first, so a boundary can name
   a point constructor of a later sort. The core holds the cases, the n
   targets, the target of x and x. */
static const Core *fold_block(Call *k, const Core *s, const Value *st) {
  Checker *c = k->c;
  Machine *m = c->m;
  const FamilyInfo *f = &m->families[st->inst];
  uint32_t first = f->block_first;
  uint32_t n = f->block_count;
  const FamilyInfo *last = &m->families[first + n - 1u];
  uint32_t base = m->families[first].first_ctor;
  uint32_t total = last->first_ctor + last->ctor_count - base;
  const Value **targets = arena_alloc(m->arena, (size_t)n * sizeof *targets);
  const Core **cores = arena_alloc(m->arena, ((size_t)total + n + 2u) * sizeof *cores);
  uint32_t j;
  int pass;
  if (targets == NULL || cores == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  if (k->argc != n + total + 1u)
    return nul(FAIL(c, "TYPE_MISMATCH", "fold over the block of %s needs %u target types, one case for each of its %u constructors, and the value; found %u arguments", f->name, (unsigned)n, (unsigned)total, (unsigned)k->argc));
  /* Every case is instantiated with the scrutinee's parameters. Check
     that those parameters inhabit the telescope of every block sort. */
  for (j = 0; j < n; j++) {
    const FamilyInfo *sort = &m->families[first + j];
    uint32_t p;
    if (sort->param_count != f->param_count)
      return nul(FAIL(c, "TYPE_MISMATCH", "fold over the block of %s needs the same parameter telescope for every sort", f->name));
    for (p = 0; p < f->param_count; p++) {
      const Env *env = env_of(c, st->args, p);
      if (!expect(c, eval_core(m, env, f->param_types[p]), eval_core(m, env, sort->param_types[p])))
        return NULL;
    }
  }
  memset(cores, 0, ((size_t)total + n + 2u) * sizeof *cores);
  for (j = 0; j < n; j++) {
    uint64_t level = 0;
    cores[total + j] = check_type(c, k->args[j], &level);
    targets[j] = cores[total + j] == NULL ? NULL : here(c, cores[total + j]);
    if (targets[j] == NULL)
      return NULL;
  }
  for (pass = 0; pass < 2; pass++)
    for (j = 0; j < total; j++)
      if ((m->ctors[base + j].interval_count != 0u) == (pass == 1) && !block_case(k, st, targets, first, base, cores, total, j))
        return NULL;
  cores[total + n] = cores[total + (st->inst - first)];
  cores[total + n + 1u] = s;
  k->used = n + total + 1u;
  k->type = targets[st->inst - first];
  return core_op(c, OP_FOLD_FAMILY, st->inst, 0, cores, total + n + 2u);
}

static int simple_scrutinee(const Value *t) {
  return val_is(t, OP_NAT) || val_is(t, OP_LIST);
}

/* FAM is a sort of the block whose fields are checked (S6). */
static int in_block(const Checker *c, uint32_t fam) {
  return c->self != NO_FAMILY && fam >= c->self && fam - c->self < c->self_count;
}

/* fold takes its scrutinee last, so the scrutinee type sets the arity. */
static const Core *rule_fold(Call *k) {
  Checker *c = k->c;
  const Value *st = NULL;
  const Core *s = try_infer(c, k->args[k->argc - 1u], &st);
  uint32_t fam = val_is(st, OP_FAMILY) ? st->inst : NO_FAMILY;
  if (s != NULL && fam != NO_FAMILY) {
    /* The constructor slots of a block are reserved before its
       constructors are checked (S6), so the test is the block. */
    if (in_block(c, fam))
      return nul(FAIL(c, "HIT_BOUNDARY", "a constructor boundary cannot fold %s before all of its constructors are checked", c->m->families[fam].name));
    if (c->m->families[fam].block_count > 1u) {
      k->hint = k->outer;
      return fold_block(k, s, st);
    }
  }
  if (s != NULL && fam != NO_FAMILY && c->m->families[fam].ctor_count + 1u == k->argc) {
    k->hint = k->outer;
    return fold_family(k, s, st);
  }
  if (s != NULL && k->argc == 3u && simple_scrutinee(st)) {
    k->hint = k->outer;
    return fold_simple(k, s, st);
  }
  s = k->argc > 3u ? try_infer(c, k->args[2], &st) : NULL;
  if (s != NULL && simple_scrutinee(st)) {
    k->hint = NULL;
    return fold_simple(k, s, st);
  }
  s = infer(c, k->args[k->argc - 1u], &st);
  return s == NULL ? NULL : nul(FAIL(c, "TYPE_MISMATCH", "fold needs a last argument of type Nat, List A or a domain family, and one case for each constructor; found %u arguments and a last argument of type %s", k->argc, show(c, st)));
}

/* unfold g limit seed, with g : S -> Option (Prod A S). */
static const Core *rule_unfold(Call *k) {
  Checker *c = k->c;
  const Value *st = NULL;
  const Value *a;
  const Core *seed;
  const Core *limit;
  const Core *g;
  if (!val_is(k->hint, OP_LIST))
    return need_type(k, "List A");
  a = k->hint->args[0];
  seed = infer(c, k->args[2], &st);
  limit = seed == NULL ? NULL : check(c, k->args[1], nat_type(c));
  g = limit == NULL ? NULL : check(c, k->args[0], val_arrow(c->m, st, tyop(c, OP_OPTION, 1, tyop(c, OP_PROD, 2, a, st, NULL), NULL, NULL)));
  k->type = k->hint;
  return op3(c, OP_UNFOLD, 0, 3, g, limit, seed);
}

/* The binder name of a path abstraction made from a path type: the name of
   its line, or `i` for the constant line `_` of Path. The printed term must
   name the variable that its body uses. */
static const char *path_binder(const char *name) {
  return name == NULL || strcmp(name, "_") == 0 ? "i" : name;
}

/* sym p is (fun q => fun i => q @ ~i) p, of type PathP (fun i => L[~i]) b a
   for p : PathP L a b. The application keeps p outside the interval
   binder, so p needs no shift. */
static const Core *rule_sym(Call *k) {
  Checker *c = k->c;
  const Value *pt = NULL;
  const Core *p = infer_path(k, 0, &pt);
  const Core *flip;
  const Core *line;
  const Core *body;
  if (p == NULL)
    return NULL;
  flip = core_iop(c, CORE_INEG, core_index(c, CORE_VAR, 0), NULL);
  line = quote_value(c->m, c->level + 1u, closure_apply(c->m, pt, at(c, flip, val_ivar(c->m, c->level))));
  k->type = here(c, core_ends(c, CORE_PATHP, pt->name, NULL, line, quote_value(c->m, c->level, pt->args[1]), quote_value(c->m, c->level, pt->args[0])));
  body = core_ends(c, CORE_PAPP, NULL, core_index(c, CORE_VAR, 1), flip, quote_value(c->m, c->level + 2u, pt->args[0]), quote_value(c->m, c->level + 2u, pt->args[1]));
  return core_app(c, core_bind(c, CORE_LAM, "q", NULL, core_bind(c, CORE_PLAM, path_binder(pt->name), NULL, body)), p);
}

/* cong f p is (fun g => fun q => fun i => g (q @ i)) f p, for
   f : (x : A) -> B and p : Path A a b, of type
   PathP (fun i => B[p @ i]) (f a) (f b). */
static const Core *rule_cong(Call *k) {
  Checker *c = k->c;
  const Value *pt = NULL;
  const Value *ft = NULL;
  const Value *i = val_ivar(c->m, c->level);
  const Core *p = infer_path(k, 1, &pt);
  const Core *f = p == NULL ? NULL : try_infer(c, k->args[0], &ft);
  const Value *fv;
  const Value *pv;
  const Core *line;
  const Core *body;
  if (p == NULL)
    return NULL;
  if (f == NULL) {
    ft = k->hint != NULL && k->hint->kind == VAL_PATHP ? val_arrow(c->m, closure_apply(c->m, pt, val_iend(c->m, 0)), closure_apply(c->m, k->hint, val_iend(c->m, 0))) : NULL;
    f = ft != NULL ? check(c, k->args[0], ft) : infer(c, k->args[0], &ft);
  }
  if (f == NULL)
    return NULL;
  if (ft == NULL || ft->kind != VAL_PI)
    return nul(FAIL(c, "TYPE_NOT_FUNCTION", "cong needs a function, found a term of type %s", show(c, ft)));
  if (!conv_values(c->m, c->level + 1u, closure_apply(c->m, pt, i), ft->dom))
    return nul(FAIL(c, "TYPE_MISMATCH", "cong needs a path in %s, found a term of type %s", show(c, ft->dom), show(c, pt)));
  fv = here(c, f);
  pv = here(c, p);
  line = quote_value(c->m, c->level + 1u, closure_apply(c->m, ft, path_apply(c->m, pv, i, pt->args[0], pt->args[1])));
  k->type = here(c, core_ends(c, CORE_PATHP, pt->name, NULL, line, quote_value(c->m, c->level, apply_value(c->m, fv, pt->args[0])), quote_value(c->m, c->level, apply_value(c->m, fv, pt->args[1]))));
  body = core_app(c, core_index(c, CORE_VAR, 2), core_ends(c, CORE_PAPP, NULL, core_index(c, CORE_VAR, 1), core_index(c, CORE_VAR, 0), quote_value(c->m, c->level + 3u, pt->args[0]), quote_value(c->m, c->level + 3u, pt->args[1])));
  return core_app(c, core_app(c, core_bind(c, CORE_LAM, "g", NULL, core_bind(c, CORE_LAM, "q", NULL, core_bind(c, CORE_PLAM, path_binder(pt->name), NULL, body))), f), p);
}

/* funExt h is (fun g => fun i => fun x => g x @ i) h, for
   h : (x : A) -> PathP L a b, of type
   PathP (fun i => (x : A) -> L[i]) (fun x => a) (fun x => b). */
static const Core *rule_funext(Call *k) {
  Checker *c = k->c;
  const Value *ht = NULL;
  const Core *h = infer(c, k->args[0], &ht);
  const char *x;
  const Value *cod;
  const Value *at_x;
  const Value *inner;
  const Core *line;
  const Core *body;
  if (h == NULL)
    return NULL;
  cod = ht != NULL && ht->kind == VAL_PI ? closure_apply(c->m, ht, val_var(c->m, c->level)) : NULL;
  if (cod == NULL || cod->kind != VAL_PATHP)
    return nul(FAIL(c, "TYPE_MISMATCH", "funExt needs a function into paths (x : A) -> PathP L a b, found a term of type %s", show(c, ht)));
  x = ht->name != NULL ? ht->name : "x";
  /* The line: i at LEVEL, x at LEVEL + 1. */
  at_x = closure_apply(c->m, ht, val_var(c->m, c->level + 1u));
  /* The body: h at LEVEL, i at LEVEL + 1, x at LEVEL + 2. */
  inner = closure_apply(c->m, ht, val_var(c->m, c->level + 2u));
  if (at_x == NULL || inner == NULL || at_x->kind != VAL_PATHP || inner->kind != VAL_PATHP)
    return nul(FAIL(c, "INTERNAL", "funExt: the codomain of %s is not a path type at each argument", show(c, ht)));
  line = core_bind(c, CORE_PI, x, quote_value(c->m, c->level + 1u, ht->dom), quote_value(c->m, c->level + 2u, closure_apply(c->m, at_x, val_ivar(c->m, c->level))));
  k->type = here(c, core_ends(c, CORE_PATHP, cod->name, NULL, line, core_bind(c, CORE_LAM, x, NULL, quote_value(c->m, c->level + 1u, cod->args[0])), core_bind(c, CORE_LAM, x, NULL, quote_value(c->m, c->level + 1u, cod->args[1]))));
  body = core_ends(c, CORE_PAPP, NULL, core_app(c, core_index(c, CORE_VAR, 2), core_index(c, CORE_VAR, 0)), core_index(c, CORE_VAR, 1), quote_value(c->m, c->level + 3u, inner->args[0]), quote_value(c->m, c->level + 3u, inner->args[1]));
  return core_app(c, core_bind(c, CORE_LAM, "g", NULL, core_bind(c, CORE_PLAM, path_binder(cod->name), NULL, core_bind(c, CORE_LAM, x, NULL, body))), h);
}

/* TRANSP_LINE: the line names its interval variable in the message. */
static const Core *transp_line_fail(Checker *c, const char *name, const Value *face, const Core *line) {
  const Value *v = at(c, line, val_ivar(c->m, c->level));
  const char *f = show(c, face);
  const char *zero = show(c, at(c, line, val_iend(c->m, 0)));
  const char *bound;
  if (!push_interval(c, path_binder(name)))
    return NULL;
  bound = show(c, v);
  pop(c);
  return nul(FAIL(c, "TRANSP_LINE", "the line of transp must be constant where %s holds, but it is %s at %s and %s at i0", f, bound, path_binder(name), zero));
}

/* transp (fun i => A) phi a, with phi : I and a : A[i0], of type A[i1]
   (S2 R3 (a)). On each face of phi the line is constant: under the face
   map of each conjunction, A at a fresh i is A at i0. */
static const Core *rule_transp(Call *k) {
  Checker *c = k->c;
  uint64_t level = 0;
  const Core *line = check_line(c, k->args[0], &level);
  const Core *phi = line == NULL ? NULL : check(c, k->args[1], interval_type(c));
  const Core *a = phi == NULL ? NULL : check(c, k->args[2], at(c, line, val_iend(c->m, 0)));
  const Value *face = a == NULL ? NULL : face_norm(c->m, here(c, phi));
  const Env *e = NULL;
  uint32_t j;
  if (face == NULL)
    return NULL;
  for (j = 0; j < face->conj_count; j++) {
    if (!env_face(c->m, c->env, &face->conj[j], &e))
      return NULL;
    if (!conv_values(c->m, c->level + 1u, eval_core(c->m, env_push(c->m, e, val_ivar(c->m, c->level)), line), eval_core(c->m, env_push(c->m, e, val_iend(c->m, 0)), line)))
      return transp_line_fail(c, k->args[0]->name, face, line);
  }
  k->type = at(c, line, val_iend(c->m, 1));
  return op3(c, OP_TRANSP, 0, 3, core_bind(c, CORE_PLAM, path_binder(k->args[0]->name), NULL, line), phi, a);
}

/* A branch `k -> t` of a system: the core of its key, the face normal
   form of the key and the core of its body (under the tube binder). */
typedef struct {
  const Term *term;
  const Core *key;
  const Value *face;
  const Core *body;
} Branch;

static uint32_t system_size(const Term *sys) {
  uint32_t n = 0;
  for (; sys != NULL && sys->left != NULL; sys = sys->right)
    n++;
  return n;
}

/* The body core BODY at the interval value R, in the env E. */
static const Value *branch_at(Checker *c, const Env *e, const Core *body, const Value *r) {
  const Env *with = env_push(c->m, e, r);
  return with == NULL ? NULL : eval_core(c->m, with, body);
}

/* Split disjunctive keys before elaboration: inferred arguments can be
   different on each conjunction, so each one needs its own body core. */
static Branch *split_branches(Checker *c, const Branch *bs, uint32_t n, uint32_t *count) {
  Branch *out;
  Value *face;
  uint32_t i;
  uint32_t j;
  uint32_t k = 0;
  *count = 0;
  for (i = 0; i < n; i++)
    *count += bs[i].face->conj_count > 0 ? bs[i].face->conj_count : 1u;
  out = arena_alloc(c->m->arena, ((size_t)*count + 1u) * sizeof *out);
  if (out == NULL) {
    FAIL(c, "OOM", "out of memory");
    return NULL;
  }
  for (i = 0; i < n; i++) {
    if (bs[i].face->conj_count <= 1u) {
      out[k++] = bs[i];
      continue;
    }
    for (j = 0; j < bs[i].face->conj_count; j++) {
      face = arena_alloc(c->m->arena, sizeof *face);
      if (face == NULL) {
        FAIL(c, "OOM", "out of memory");
        return NULL;
      }
      *face = *bs[i].face;
      face->conj = &bs[i].face->conj[j];
      face->conj_count = 1;
      out[k] = bs[i];
      out[k].face = face;
      out[k].key = quote_value(c->m, c->level, face);
      if (out[k].key == NULL)
        return NULL;
      k++;
    }
  }
  return out;
}

/* The body of B against A, with the tube binder NAME, under its single
   conjunction. A key with no conjunction checks in the plain context.
   NAME is NULL for a binder that no term can use (Glue and glue). */
static const Core *hcomp_branch(Checker *c, const char *name, const Core *a, const Branch *b) {
  const Env *saved = c->env;
  const Scope *scope = c->scope;
  const Scope *s;
  Scope *copy;
  const Scope **tail;
  const Scope *acted;
  const Core *type;
  const Env *e;
  const Core *core = NULL;
  const Core *one;
  uint32_t n = b->face->conj_count > 0 ? b->face->conj_count : 1u;
  uint32_t j;
  for (j = 0; j < n; j++) {
    e = saved;
    if (b->face->conj_count > 0 && !env_face(c->m, saved, &b->face->conj[j], &e))
      return NULL;
    acted = NULL;
    tail = &acted;
    for (s = scope; s != NULL; s = s->next) {
      type = quote_value(c->m, c->level, s->type);
      copy = arena_alloc(c->m->arena, sizeof *copy);
      if (type == NULL || copy == NULL)
        return nul(c->m->diag->set ? 0 : FAIL(c, "OOM", "out of memory"));
      *copy = *s;
      copy->type = eval_core(c->m, e, type);
      if (copy->type == NULL)
        return NULL;
      *tail = copy;
      tail = &copy->next;
    }
    *tail = NULL;
    c->scope = acted;
    c->env = e;
    if (!push_interval(c, name)) {
      c->env = saved;
      c->scope = scope;
      return NULL;
    }
    one = check(c, b->term->right, eval_core(c->m, e, a));
    pop(c);
    c->env = saved;
    c->scope = scope;
    if (one == NULL)
      return NULL;
    core = core == NULL ? one : core;
  }
  return core;
}

/* SYSTEM_OVERLAP: the branches X and Y agree at a fresh i on each
   conjunction of JOINT, the meet of their faces. */
static int hcomp_overlap(Checker *c, const char *name, const Value *joint, const Branch *x, const Branch *y) {
  const Value *i = val_ivar(c->m, c->level);
  const Env *e = NULL;
  const Value *vx;
  const Value *vy;
  const char *sx;
  const char *sy;
  uint32_t j;
  if (joint == NULL)
    return 0;
  for (j = 0; j < joint->conj_count; j++) {
    if (!env_face(c->m, c->env, &joint->conj[j], &e))
      return 0;
    vx = branch_at(c, e, x->body, i);
    vy = branch_at(c, e, y->body, i);
    if (vx == NULL || vy == NULL)
      return 0;
    if (!conv_values(c->m, c->level + 1u, vx, vy)) {
      if (!push_interval(c, path_binder(name)))
        return 0;
      sx = show(c, vx);
      sy = show(c, vy);
      pop(c);
      return FAIL(c, "SYSTEM_OVERLAP", "the branches for %s and %s must agree where both keys hold, but they are %s and %s", show(c, here(c, x->key)), show(c, here(c, y->key)), sx, sy);
    }
  }
  return 1;
}

/* HCOMP_CAP: on each conjunction of the face of B, B at i0 is the base U0. */
static int hcomp_cap(Checker *c, const Core *u0, const Branch *b) {
  const Env *e = NULL;
  const Value *at0;
  const Value *base;
  uint32_t j;
  for (j = 0; j < b->face->conj_count; j++) {
    if (!env_face(c->m, c->env, &b->face->conj[j], &e))
      return 0;
    at0 = branch_at(c, e, b->body, val_iend(c->m, 0));
    base = eval_core(c->m, e, u0);
    if (at0 == NULL || base == NULL)
      return 0;
    if (!conv_values(c->m, c->level, at0, base))
      return FAIL(c, "HCOMP_CAP", "the base of hcomp must be the branch for %s at i0, but the base is %s and the branch is %s at i0", show(c, here(c, b->key)), show(c, base), show(c, at0));
  }
  return 1;
}

/* The branches of the system body of TUBE. Each key is checked against I
   and face-normalized. The join of the keys must be the face PHI
   (SYSTEM_COVER). A disjunctive key gives a branch for each conjunction
   (split_branches). *COUNT gets the number of branches. hcomp, Glue and
   glue use this. */
static Branch *system_branches(Checker *c, const Term *tube, const Value *phi, uint32_t *count) {
  uint32_t n = system_size(tube->right);
  Branch *bs = arena_alloc(c->m->arena, ((size_t)n + 1u) * sizeof *bs);
  const Value *join = val_iend(c->m, 0);
  const Value *pf;
  const Term *cell;
  uint32_t i;
  if (bs == NULL) {
    FAIL(c, "OOM", "out of memory");
    return NULL;
  }
  for (cell = tube->right, i = 0; i < n; cell = cell->right, i++) {
    bs[i].term = cell->left;
    bs[i].key = check(c, cell->left->left, interval_type(c));
    bs[i].face = face_norm(c->m, here(c, bs[i].key));
    if (bs[i].face == NULL)
      return NULL;
    join = face_join(c->m, join, bs[i].face);
  }
  pf = face_norm(c->m, phi);
  if (join == NULL || pf == NULL)
    return NULL;
  if (!conv_values(c->m, c->level, join, pf)) {
    FAIL(c, "SYSTEM_COVER", "the keys of the system must cover %s, but their join is %s", show(c, pf), show(c, join));
    return NULL;
  }
  return split_branches(c, bs, n, count);
}

/* SYSTEM_OVERLAP for each pair of the N checked branches BS. */
static int system_overlaps(Checker *c, const char *name, const Branch *bs, uint32_t n) {
  uint32_t i;
  uint32_t j;
  for (i = 0; i < n; i++)
    for (j = i + 1u; j < n; j++)
      if (!hcomp_overlap(c, name, face_meet(c->m, bs[i].face, bs[j].face), &bs[i], &bs[j]))
        return 0;
  return 1;
}

/* The OP_SYSTEM core of the N checked branches BS: each key, then its body
   under the path binder NAME. */
static const Core *system_core(Checker *c, const char *name, const Branch *bs, uint32_t n) {
  const Core **sys = arena_alloc(c->m->arena, (2u * (size_t)n + 1u) * sizeof *sys);
  uint32_t i;
  if (sys == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  for (i = 0; i < n; i++) {
    sys[2u * i] = bs[i].key;
    sys[2u * i + 1u] = core_bind(c, CORE_PLAM, name, NULL, bs[i].body);
  }
  return core_op(c, OP_SYSTEM, 0, 0, sys, 2u * n);
}

/* hcomp A phi (fun i => [ k0 -> t0, ... ]) u0, with phi : I, each key
   k : I in the outer context, u0 : A and each branch t : A for i : I where
   its key holds (S2 R2 (a)), of type A. The keys cover phi (SYSTEM_COVER),
   two branches agree where both keys hold (SYSTEM_OVERLAP) and each branch
   at i0 is u0 (HCOMP_CAP). Hcomp at a universe is Glue (S3). A face acts
   on both the env values and the local scope types. */
static const Core *rule_hcomp(Call *k) {
  Checker *c = k->c;
  uint64_t level = 0;
  const Term *tube = k->args[2];
  const Core *a = check_type(c, k->args[0], &level);
  const Value *av = here(c, a);
  const Core *phi;
  const Core *u0;
  const Core *args[4];
  Branch *bs;
  uint32_t n = 0;
  uint32_t i;
  if (av == NULL)
    return NULL;
  if (av->kind == VAL_UNIV && av->nat != 0u)
    return nul(FAIL(c, "HCOMP_UNIVERSE", "hcomp at Type %llu needs Glue above Type 0", (unsigned long long)av->nat));
  phi = check(c, k->args[1], interval_type(c));
  if (phi == NULL)
    return NULL;
  if (tube->kind != TERM_LAM || (tube->left != NULL && !is_interval(c, tube->left)) || tube->right == NULL || tube->right->kind != TERM_SYSTEM)
    return nul(FAIL(c, "TYPE_MISMATCH", "the tube of hcomp must be a system abstraction fun (i : I) => [ phi -> t, ... ]"));
  bs = system_branches(c, tube, here(c, phi), &n);
  if (bs == NULL)
    return NULL;
  for (i = 0; i < n; i++) {
    bs[i].body = hcomp_branch(c, tube->name, a, &bs[i]);
    if (bs[i].body == NULL)
      return NULL;
  }
  if (!system_overlaps(c, tube->name, bs, n))
    return NULL;
  u0 = check(c, k->args[3], av);
  if (u0 == NULL)
    return NULL;
  for (i = 0; i < n; i++)
    if (!hcomp_cap(c, u0, &bs[i]))
      return NULL;
  args[0] = a;
  args[1] = phi;
  args[2] = system_core(c, path_binder(tube->name), bs, n);
  args[3] = u0;
  k->type = av;
  return core_op(c, OP_HCOMP, 0, 0, args, 4);
}

/* GLUE_BINDER: the tube of Glue and glue is fun (_ : I) => [ k -> t, ... ].
   The binder must be _, so a branch cannot use it and an outer i stays
   visible. The branches check under a binder with no name (see
   glue_branch), so a branch cannot use the name _ for it. */
static int glue_tube(Call *k, const Term *tube) {
  Checker *c = k->c;
  if (tube->kind != TERM_LAM || (tube->left != NULL && !is_interval(c, tube->left)) || tube->right == NULL || tube->right->kind != TERM_SYSTEM)
    return FAIL(c, "TYPE_MISMATCH", "the tube of %s must be a system abstraction fun (_ : I) => [ phi -> t, ... ]", k->b->name);
  if (tube->name != NULL && strcmp(tube->name, "_") != 0)
    return FAIL(c, "GLUE_BINDER", "the tube binder of %s must be _, found %s; a branch of %s cannot use the tube variable", k->b->name, tube->name, k->b->name);
  return 1;
}

/* Sigma (T : Type 0) (Equiv T A), the type of a Glue branch, for the type
   AV in the current context. Equiv is the domain definition. */
static const Core *equiv_sigma(Checker *c, const Value *av) {
  uint32_t d = find_def(c->m, "Equiv");
  const Core *equiv;
  if (d == NO_DEF)
    return nul(FAIL(c, "NAME_UNKNOWN", "Glue needs the domain definition Equiv, which is not declared"));
  equiv = core_app(c, core_app(c, core_index(c, CORE_GLOBAL, d), core_index(c, CORE_VAR, 0)), quote_value(c->m, c->level + 1u, av));
  return core_bind(c, CORE_SIGMA, "T", core_leaf(c, CORE_UNIV, 0), equiv);
}

/* 1 for OOM and for a failure of the evaluator. These failures are not
   about a branch, so GLUE_EQUIV does not replace them. */
static int kept_failure(const char *code) {
  return strcmp(code, "OOM") == 0 || strcmp(code, "INTERNAL") == 0 || strcmp(code, "EVAL_FUEL") == 0 || strcmp(code, "EVAL_DEPTH") == 0;
}

/* GLUE_EQUIV: the branch B of Glue A phi sys is pack T e of type SIGMA,
   that is Sigma (T : Type 0) (Equiv T A), where its key holds. A failure
   of this check becomes GLUE_EQUIV, with the first code and message. */
static const Core *glue_branch(Checker *c, const Core *sigma, const Value *av, const Branch *b) {
  char inner[sizeof c->m->diag->msg];
  const char *code;
  Diag clean = *c->m->diag;
  const Core *body = hcomp_branch(c, NULL, sigma, b);
  if (body != NULL || !c->m->diag->set)
    return body;
  code = c->m->diag->code != NULL ? c->m->diag->code : "-";
  if (kept_failure(code))
    return NULL;
  snprintf(inner, sizeof inner, "%s", c->m->diag->msg);
  *c->m->diag = clean;
  return nul(FAIL(c, "GLUE_EQUIV", "the branch for %s must be pack T e with T : Type 0 and e : Equiv T %s: %s: %s", show(c, here(c, b->key)), show(c, av), code, inner));
}

/* Glue A phi (fun (_ : I) => [ k0 -> pack T0 e0, ... ]), with A : Type 0,
   phi : I, each key k : I in the outer context and each branch of type
   Sigma (T : Type 0) (Equiv T A) where its key holds (S3b), of type
   Type 0. The keys cover phi (SYSTEM_COVER), each branch is an
   equivalence into A (GLUE_EQUIV) and two branches agree where both keys
   hold (SYSTEM_OVERLAP). The core is OP_GLUE (A, phi, sys), with sys as
   hcomp keeps it. */
static const Core *rule_glue(Call *k) {
  Checker *c = k->c;
  const Term *tube = k->args[2];
  const Core *a = check(c, k->args[0], val_univ(c->m, 0));
  const Value *av = here(c, a);
  const Core *phi = av == NULL ? NULL : check(c, k->args[1], interval_type(c));
  const Core *sigma;
  const Core *args[3];
  Branch *bs;
  uint32_t n = 0;
  uint32_t i;
  if (phi == NULL || !glue_tube(k, tube))
    return NULL;
  sigma = equiv_sigma(c, av);
  bs = sigma == NULL ? NULL : system_branches(c, tube, here(c, phi), &n);
  if (bs == NULL)
    return NULL;
  for (i = 0; i < n; i++) {
    bs[i].body = glue_branch(c, sigma, av, &bs[i]);
    if (bs[i].body == NULL)
      return NULL;
  }
  if (!system_overlaps(c, "_", bs, n))
    return NULL;
  args[0] = a;
  args[1] = phi;
  args[2] = system_core(c, "_", bs, n);
  k->type = val_univ(c->m, 0);
  return core_op(c, OP_GLUE, 0, 0, args, 3);
}

/* GLUE_BOUNDARY: on each conjunction of the face of B, unglue of the
   branch t of glue is the base A0. G is the expected Glue type. A key of G
   holds there, so unglue t is witness e t (S3b face rule). */
static int glue_boundary(Checker *c, const Value *g, const Core *a0, const Branch *b) {
  const Core *args[4];
  const Core *un;
  const Env *e = NULL;
  const Value *vu;
  const Value *va;
  uint32_t j;
  for (j = 0; j < 3u; j++)
    args[j] = quote_value(c->m, c->level + 1u, g->args[j]);
  args[3] = b->body;
  un = core_op(c, OP_UNGLUE, 0, 0, args, 4);
  if (un == NULL)
    return 0;
  for (j = 0; j < b->face->conj_count; j++) {
    if (!env_face(c->m, c->env, &b->face->conj[j], &e))
      return 0;
    vu = branch_at(c, e, un, val_iend(c->m, 1));
    va = eval_core(c->m, e, a0);
    if (vu == NULL || va == NULL)
      return 0;
    if (!conv_values(c->m, c->level, vu, va))
      return FAIL(c, "GLUE_BOUNDARY", "unglue of the branch for %s must be the base of glue where its key holds, but it is %s and the base is %s", show(c, here(c, b->key)), show(c, vu), show(c, va));
  }
  return 1;
}

/* glue (fun (_ : I) => [ k0 -> t0, ... ]) a against Glue A phi sys (S3b),
   in check mode only. The keys cover phi (SYSTEM_COVER), each branch t is
   a term of the Glue type where its key holds, that is of T, two branches
   agree where both keys hold (SYSTEM_OVERLAP), a : A and unglue t is a
   where the key of t holds (GLUE_BOUNDARY). The core is OP_GLUE_ELEM (phi,
   sys, a). */
static const Core *rule_glue_elem(Call *k) {
  Checker *c = k->c;
  const Value *g = k->hint;
  const Term *tube = k->args[0];
  const Core *gt;
  const Core *a;
  const Core *args[3];
  Branch *bs;
  uint32_t n = 0;
  uint32_t i;
  if (!val_is(g, OP_GLUE) || g->argc != 3u)
    return need_type(k, "Glue A phi sys");
  if (!glue_tube(k, tube))
    return NULL;
  gt = quote_value(c->m, c->level, g);
  bs = gt == NULL ? NULL : system_branches(c, tube, g->args[1], &n);
  if (bs == NULL)
    return NULL;
  for (i = 0; i < n; i++) {
    bs[i].body = hcomp_branch(c, NULL, gt, &bs[i]);
    if (bs[i].body == NULL)
      return NULL;
  }
  if (!system_overlaps(c, "_", bs, n))
    return NULL;
  a = check(c, k->args[1], g->args[0]);
  if (a == NULL)
    return NULL;
  for (i = 0; i < n; i++)
    if (!glue_boundary(c, g, a, &bs[i]))
      return NULL;
  args[0] = quote_value(c->m, c->level, g->args[1]);
  args[1] = system_core(c, "_", bs, n);
  args[2] = a;
  k->type = g;
  return core_op(c, OP_GLUE_ELEM, 0, 0, args, 3);
}

/* unglue g, with g of the type Glue A phi sys (S3b), of type A. The core
   keeps A, phi and sys, as p @ r keeps its ends (S1), because a neutral
   carries no type. The type of g must be a Glue type (UNGLUE_TYPE). The
   core is OP_UNGLUE (A, phi, sys, g). */
static const Core *rule_unglue(Call *k) {
  Checker *c = k->c;
  const Value *gt = NULL;
  const Core *g = infer(c, k->args[0], &gt);
  const Core *args[4];
  uint32_t i;
  if (g == NULL)
    return NULL;
  if (!val_is(gt, OP_GLUE) || gt->argc != 3u)
    return nul(FAIL(c, "UNGLUE_TYPE", "unglue needs a term of a type Glue A phi sys, found a term of type %s", show(c, gt)));
  for (i = 0; i < 3u; i++)
    args[i] = quote_value(c->m, c->level, gt->args[i]);
  args[3] = g;
  k->type = gt->args[0];
  return core_op(c, OP_UNGLUE, 0, 0, args, 4);
}

/* natAdd, u256Add, addrEq, toU256 and the like: each argument has the type
   former ARG. */
static const Core *rule_prim(Call *k, Op arg) {
  Checker *c = k->c;
  const Value *t = tyop(c, arg, 0, NULL, NULL, NULL);
  const Core *x = check(c, k->args[0], t);
  const Core *y = x == NULL || k->b->arity < 2u ? NULL : check(c, k->args[1], t);
  k->type = tyop(c, k->b->result, 0, NULL, NULL, NULL);
  return k->b->arity < 2u ? op3(c, k->b->op, 0, 1, x, NULL, NULL) : op3(c, k->b->op, 0, 2, x, y, NULL);
}

static uint32_t find_family(const Machine *m, const char *name);

/* len A r (S3, USER ruling Q-S3-3 (a)): the op count of the trace
   r : FreeMon A, a Nat (an OpPos). A C builtin: the domain cannot prove
   the Nat monoid laws that fmFold needs (S3 survey section 7). */
static const Core *rule_len(Call *k) {
  Checker *c = k->c;
  uint32_t fam = find_family(c->m, "FreeMon");
  const Core *args[2];
  const Value *a;
  args[0] = check(c, k->args[0], val_univ(c->m, 0));
  a = here(c, args[0]);
  args[1] = a == NULL || fam >= c->m->family_count ? NULL : check(c, k->args[1], val_op(c->m, VAL_OP, OP_FAMILY, fam, 0, &a, 1));
  k->type = tyop(c, OP_NAT, 0, NULL, NULL, NULL);
  return args[1] == NULL ? NULL : core_op(c, OP_FM_LEN, 0, 0, args, 2);
}

/* The curve ops (S2, R17) and the zk ops (S8 B2a): fixed signatures over
   U256, Nat, G1 = Prod U256 U256, G2 = Prod G1 G1 (the EIP-197 field order),
   List (Prod G1 G2), a ring List G1 and a word list List U256. The result is
   the row result; OP_PROD in the row means G1 and OP_LIST means List U256. */
static const Core *rule_fixed(Call *k) {
  enum { SHAPE_U256, SHAPE_NAT, SHAPE_G1, SHAPE_PAIRS, SHAPE_RING, SHAPE_WORDS };
  static const struct {
    Op op;
    unsigned char arg[4];
  } ROWS[] = {
    {OP_ECRECOVER, {SHAPE_U256, SHAPE_NAT, SHAPE_U256, SHAPE_U256}},
    {OP_EC_ADD, {SHAPE_G1, SHAPE_G1, SHAPE_U256, SHAPE_U256}},
    {OP_EC_MUL, {SHAPE_G1, SHAPE_U256, SHAPE_U256, SHAPE_U256}},
    {OP_EC_PAIRING, {SHAPE_PAIRS, SHAPE_U256, SHAPE_U256, SHAPE_U256}},
    {OP_ZK_HP, {SHAPE_G1, SHAPE_U256, SHAPE_U256, SHAPE_U256}},
    {OP_ZK_KEY, {SHAPE_NAT, SHAPE_U256, SHAPE_U256, SHAPE_U256}},
    {OP_ZK_DIGEST, {SHAPE_RING, SHAPE_U256, SHAPE_U256, SHAPE_U256}},
    {OP_ZK_SIGN, {SHAPE_RING, SHAPE_U256, SHAPE_U256, SHAPE_U256}},
    {OP_ZK_VERIFY, {SHAPE_RING, SHAPE_U256, SHAPE_U256, SHAPE_WORDS}},
  };
  Checker *c = k->c;
  const Value *u = tyop(c, OP_U256, 0, NULL, NULL, NULL);
  const Value *g1 = tyop(c, OP_PROD, 2, u, u, NULL);
  const Value *g2 = tyop(c, OP_PROD, 2, g1, g1, NULL);
  const Value *words = tyop(c, OP_LIST, 1, u, NULL, NULL);
  const Value *shape[6] = {u, nat_type(c), g1, tyop(c, OP_LIST, 1, tyop(c, OP_PROD, 2, g1, g2, NULL), NULL, NULL), tyop(c, OP_LIST, 1, g1, NULL, NULL), words};
  const Core *args[4] = {NULL, NULL, NULL, NULL};
  size_t row = 0;
  while (row < sizeof ROWS / sizeof ROWS[0] && ROWS[row].op != k->b->op) row++;
  if (row == sizeof ROWS / sizeof ROWS[0] || k->b->arity > 4u) return NULL;
  for (uint32_t i = 0; i < k->b->arity; i++) {
    args[i] = check(c, k->args[i], shape[ROWS[row].arg[i]]);
    if (args[i] == NULL) return NULL;
  }
  k->type = k->b->result == OP_PROD ? g1 : k->b->result == OP_LIST ? words : tyop(c, k->b->result, 0, NULL, NULL, NULL);
  return core_op(c, k->b->op, 0, 0, args, k->b->arity);
}

/* zkCast M C m c (S8 B2b): M and C in Type 0, m : M, c : C; a ZkBallot C.
   The types are arguments because the constructor index needs the type. */
static const Core *rule_zk_cast(Call *k) {
  Checker *c = k->c;
  const Core *args[4] = {NULL, NULL, NULL, NULL};
  args[0] = check(c, k->args[0], val_univ(c->m, 0));
  args[1] = check(c, k->args[1], val_univ(c->m, 0));
  if (args[0] == NULL || args[1] == NULL)
    return NULL;
  args[2] = check(c, k->args[2], here(c, args[0]));
  args[3] = check(c, k->args[3], here(c, args[1]));
  if (args[2] == NULL || args[3] == NULL)
    return NULL;
  k->type = tyop(c, OP_ZK_BALLOT, 1, here(c, args[1]), NULL, NULL);
  return core_op(c, OP_ZK_CAST, 0, 0, args, 4u);
}

/* zkProve b: b : ZkBallot C for some C; a ZkWitness. */
static const Core *rule_zk_prove(Call *k) {
  Checker *c = k->c;
  const Core *args[1] = {NULL};
  const Value *type = NULL;
  args[0] = infer(c, k->args[0], &type);
  if (args[0] == NULL)
    return NULL;
  if (!val_is(type, OP_ZK_BALLOT) || type->argc != 1u)
    return nul(FAIL(c, "TYPE_MISMATCH", "zkProve needs a ballot of type ZkBallot C, found %s", show(c, type)));
  k->type = tyop(c, OP_ZK_WITNESS, 0, NULL, NULL, NULL);
  return core_op(c, OP_ZK_PROVE, 0, 0, args, 1u);
}

/* zkProject C b: C in Type 0, b : ZkBallot C; the choice, of type C. */
static const Core *rule_zk_project(Call *k) {
  Checker *c = k->c;
  const Core *args[2] = {NULL, NULL};
  args[0] = check(c, k->args[0], val_univ(c->m, 0));
  if (args[0] == NULL)
    return NULL;
  args[1] = check(c, k->args[1], tyop(c, OP_ZK_BALLOT, 1, here(c, args[0]), NULL, NULL));
  if (args[1] == NULL)
    return NULL;
  k->type = here(c, args[0]);
  return core_op(c, OP_ZK_PROJECT, 0, 0, args, 2u);
}

/* zkCheck M C w c: M and C in Type 0, w : ZkWitness, c : C; a type in
   Type 0 (Unit on a host pass, neutral otherwise). */
static const Core *rule_zk_check(Call *k) {
  Checker *c = k->c;
  const Core *args[4] = {NULL, NULL, NULL, NULL};
  args[0] = check(c, k->args[0], val_univ(c->m, 0));
  args[1] = check(c, k->args[1], val_univ(c->m, 0));
  if (args[0] == NULL || args[1] == NULL)
    return NULL;
  args[2] = check(c, k->args[2], tyop(c, OP_ZK_WITNESS, 0, NULL, NULL, NULL));
  args[3] = check(c, k->args[3], here(c, args[1]));
  if (args[2] == NULL || args[3] == NULL)
    return NULL;
  k->type = val_univ(c->m, 0);
  return core_op(c, OP_ZK_CHECK, 0, 0, args, 4u);
}

/* Fin n: n : Nat, Fin n : Type 0. */
static const Core *rule_fin(Call *k) {
  const Core *n = check(k->c, k->args[0], nat_type(k->c));
  k->type = val_univ(k->c->m, 0);
  return op3(k->c, OP_FIN, 0, 1, n, NULL, NULL);
}

/* finAdd n a b, finNeg n a: n : Nat, each other argument : Fin n, the
   result : Fin n. */
static const Core *rule_fin_op(Call *k) {
  Checker *c = k->c;
  const Core *n = check(c, k->args[0], nat_type(c));
  const Value *fin = n == NULL ? NULL : tyop(c, OP_FIN, 1, here(c, n), NULL, NULL);
  const Core *x = fin == NULL ? NULL : check(c, k->args[1], fin);
  const Core *y = x == NULL || k->b->arity < 3u ? NULL : check(c, k->args[2], fin);
  k->type = fin;
  return op3(c, k->b->op, 0, k->b->arity, n, x, y);
}

/* flagIf b yes no */
static const Core *rule_flag_if(Call *k) {
  Checker *c = k->c;
  const Value *res = k->hint;
  const Core *b = check(c, k->args[0], flag_type(c));
  const Core *yes = b == NULL ? NULL : (res != NULL ? check(c, k->args[1], res) : infer(c, k->args[1], &res));
  const Core *no = yes == NULL ? NULL : check(c, k->args[2], res);
  k->type = res;
  return op3(c, OP_FLAG_IF, 0, 3, b, yes, no);
}

static int kmap_word(const Value *t, int flag_ok) {
  return val_is(t, OP_NAT) || val_is(t, OP_U256) || val_is(t, OP_ADDR) || (flag_ok && val_is(t, OP_FLAG));
}

/* Map K V (slice K4a0): K is Nat, U256 or Addr. V is Nat, Flag, U256 or Addr. */
static const Core *rule_kmap(Call *k) {
  const Core *t = rule_type2(k);
  const Value *v = here(k->c, t);
  if (v == NULL)
    return NULL;
  if (!kmap_word(v->args[0], 0))
    return nul(FAIL(k->c, "REFUSE_MAP", "the key type of a Map must be Nat, U256 or Addr, found %s", show(k->c, v->args[0])));
  if (!kmap_word(v->args[1], 1))
    return nul(FAIL(k->c, "REFUSE_MAP", "the value type of a Map must be Nat, Flag, U256 or Addr, found %s", show(k->c, v->args[1])));
  return t;
}

/* mapGet m k and mapSet m k v (slice K4a0). The core puts V first, so the
   evaluator can make the zero value of V. */
static const Core *kmap_access(Call *k, int set) {
  Checker *c = k->c;
  const Value *t = set && val_is(k->hint, OP_KMAP) ? k->hint : NULL;
  const Core *map = t != NULL ? check(c, k->args[0], t) : infer(c, k->args[0], &t);
  const Core *key;
  const Core *args[4];
  if (map == NULL)
    return NULL;
  if (!val_is(t, OP_KMAP))
    return nul(FAIL(c, "TYPE_MISMATCH", "%s needs a Map K V, found %s", k->b->name, show(c, t)));
  key = check(c, k->args[1], t->args[0]);
  args[0] = op3(c, t->args[1]->op, 0, 0, NULL, NULL, NULL);
  args[1] = map;
  args[2] = key;
  args[3] = set && key != NULL ? check(c, k->args[2], t->args[1]) : NULL;
  k->type = set ? t : t->args[1];
  return core_op(c, k->b->op, 0, 0, args, set ? 4u : 3u);
}

static const Core *rule(Call *k) {
  switch (k->b->rule) {
  case RULE_TYPE0:
    k->type = val_univ(k->c->m, 0);
    return op3(k->c, k->b->op, 0, 0, NULL, NULL, NULL);
  case RULE_VALUE0:
    k->type = tyop(k->c, k->b->result, 0, NULL, NULL, NULL);
    return op3(k->c, k->b->op, 0, 0, NULL, NULL, NULL);
  case RULE_TYPE1:
    return rule_type1(k);
  case RULE_TYPE2:
    return rule_type2(k);
  case RULE_INTERVAL:
    return nul(FAIL(k->c, "INTERVAL_TYPE", "I is the interval, not a type; only a path abstraction or a PathP line can bind an interval variable (rule R9)"));
  case RULE_IEND:
    k->type = interval_type(k->c);
    return new_core(k->c, k->b->part == 0u ? CORE_IZERO : CORE_IONE);
  case RULE_PATHP:
    return rule_pathp(k);
  case RULE_PATH:
    return rule_path(k);
  case RULE_PAIR:
    return rule_pair(k);
  case RULE_INJ:
    return rule_inj(k);
  case RULE_EMPTY:
    return rule_empty(k);
  case RULE_SOME:
    return rule_some(k);
  case RULE_CONS:
    return rule_cons(k);
  case RULE_PACK:
    return rule_pack(k);
  case RULE_REFL:
    return rule_refl(k);
  case RULE_PART:
    return rule_part(k);
  case RULE_SIGMA_PART:
    return rule_sigma_part(k);
  case RULE_EITHER:
    return rule_either(k);
  case RULE_ELIM:
    return rule_elim(k);
  case RULE_OPTION:
    return rule_option(k);
  case RULE_PURE:
    return rule_pure(k);
  case RULE_MAP:
    return rule_map(k);
  case RULE_BIND:
    return rule_bind(k);
  case RULE_FILTER:
    return rule_filter(k);
  case RULE_FOLD:
    return rule_fold(k);
  case RULE_UNFOLD:
    return rule_unfold(k);
  case RULE_SYM:
    return rule_sym(k);
  case RULE_CONG:
    return rule_cong(k);
  case RULE_FUNEXT:
    return rule_funext(k);
  case RULE_TRANSP:
    return rule_transp(k);
  case RULE_HCOMP:
    return rule_hcomp(k);
  case RULE_GLUE:
    return rule_glue(k);
  case RULE_GLUE_ELEM:
    return rule_glue_elem(k);
  case RULE_UNGLUE:
    return rule_unglue(k);
  case RULE_NAT2:
  case RULE_TO_U256:
    return rule_prim(k, OP_NAT);
  case RULE_LEN:
    return rule_len(k);
  case RULE_U256_2:
    return rule_prim(k, OP_U256);
  case RULE_ADDR2:
    return rule_prim(k, OP_ADDR);
  case RULE_FIN_OP:
    return rule_fin_op(k);
  case RULE_FIN:
    return rule_fin(k);
  case RULE_FLAG_IF:
    return rule_flag_if(k);
  case RULE_KMAP:
    return rule_kmap(k);
  case RULE_KMAP_GET:
    return kmap_access(k, 0);
  case RULE_KMAP_SET:
    return kmap_access(k, 1);
  case RULE_FIXED:
    return rule_fixed(k);
  case RULE_ZK_CAST:
    return rule_zk_cast(k);
  case RULE_ZK_PROVE:
    return rule_zk_prove(k);
  case RULE_ZK_PROJECT:
    return rule_zk_project(k);
  case RULE_ZK_CHECK:
    return rule_zk_check(k);
  }
  return NULL;
}

/* Checks the first N family arguments ARGS: the parameters, then the
   indices (S6), each at the values of the arguments before it. VALS gets
   their values. */
static int check_params(Checker *c, const FamilyInfo *f, uint32_t n, const Term *const *args, const Core **cores, const Value **vals) {
  uint32_t i;
  for (i = 0; i < n; i++) {
    const Core *t = i < f->param_count ? f->param_types[i] : f->index_types[i - f->param_count];
    cores[i] = check(c, args[i], eval_core(c->m, env_of(c, vals, i), t));
    vals[i] = here(c, cores[i]);
    if (vals[i] == NULL)
      return 0;
  }
  return 1;
}

static const Core *family_app(Call *k, uint32_t fam) {
  Checker *c = k->c;
  const FamilyInfo *f = &c->m->families[fam];
  uint32_t n = f->param_count + f->index_count;
  const Core **cores = arena_alloc(c->m->arena, ((size_t)n + 1u) * sizeof *cores);
  const Value **vals = arena_alloc(c->m->arena, ((size_t)n + 1u) * sizeof *vals);
  if (cores == NULL || vals == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  if (!check_params(c, f, n, k->args, cores, vals))
    return NULL;
  k->type = val_univ(c->m, f->universe);
  return core_op(c, OP_FAMILY, fam, 0, cores, n);
}

/* A constructor takes the family parameters (the indices), then its fields,
   then its interval arguments (a path constructor). */
static const Core *ctor_app(Call *k, uint32_t ctor) {
  Checker *c = k->c;
  const CtorInfo *ci = &c->m->ctors[ctor];
  const FamilyInfo *f = &c->m->families[ci->family];
  uint32_t p = f->param_count;
  uint32_t q = p + ci->field_count;
  uint32_t n = q + ci->interval_count;
  const Core **cores = arena_alloc(c->m->arena, ((size_t)n + 1u) * sizeof *cores);
  const Value **vals = arena_alloc(c->m->arena, ((size_t)p + f->index_count + 1u) * sizeof *vals);
  const Env *env;
  uint32_t j;
  if (cores == NULL || vals == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  if (!check_params(c, f, p, k->args, cores, vals))
    return NULL;
  env = env_of(c, vals, p);
  /* Field j is checked at the parameters and the earlier field values. */
  for (j = 0; j < ci->field_count; j++) {
    const Value *fv;
    cores[p + j] = check(c, k->args[p + j], eval_core(c->m, env, ci->fields[j].type));
    fv = here(c, cores[p + j]);
    env = fv == NULL ? NULL : env_push(c->m, env, fv);
    if (env == NULL)
      return NULL;
  }
  for (j = q; j < n; j++) {
    cores[j] = check(c, k->args[j], interval_type(c));
    if (cores[j] == NULL)
      return NULL;
  }
  /* The result indices at the parameters and the field values (S6). */
  for (j = 0; ci->result != NULL && j < f->index_count; j++) {
    vals[p + j] = eval_core(c->m, env, ci->result[j]);
    if (vals[p + j] == NULL)
      return NULL;
  }
  k->type = val_op(c->m, VAL_OP, OP_FAMILY, ci->family, 0, vals, ci->result == NULL ? p : p + f->index_count);
  return core_op(c, OP_CTOR, ctor, 0, cores, n);
}

/* A field of a one-constructor family is a projection. */
static const Core *proj_app(Call *k, uint32_t ctor, uint32_t field) {
  Checker *c = k->c;
  const CtorInfo *ci = &c->m->ctors[ctor];
  const Value *xt = NULL;
  const Core *x = infer(c, k->args[0], &xt);
  const Env *env;
  uint32_t i;
  if (x == NULL)
    return NULL;
  if (!val_is(xt, OP_FAMILY) || xt->inst != ci->family)
    return nul(FAIL(c, "TYPE_MISMATCH", "%s needs a value of the family %s, found a term of type %s", ci->fields[field].name, c->m->families[ci->family].name, show(c, xt)));
  /* The field type at the parameters and the projections of the earlier
     fields of X (S4 item 13), as the second part of a Sigma. */
  env = env_of(c, xt->args, c->m->families[ci->family].param_count);
  for (i = 0; i < field; i++) {
    const Value *fv = here(c, core_op(c, OP_PROJ, ctor, i, &x, 1));
    env = fv == NULL ? NULL : env_push(c->m, env, fv);
    if (env == NULL)
      return NULL;
  }
  k->type = eval_core(c->m, env, ci->fields[field].type);
  return core_op(c, OP_PROJ, ctor, field, &x, 1);
}

static int find_known(Checker *c, const char *name, Head *h) {
  const Machine *m = c->m;
  uint32_t i;
  uint32_t j;
  for (i = 0; i < m->family_count; i++) {
    if (strcmp(m->families[i].name, name) == 0) {
      h->kind = HEAD_FAMILY;
      h->index = i;
      c->self_uses += in_block(c, i);
      return 1;
    }
  }
  for (i = 0; i < m->ctor_count; i++) {
    /* A reserved slot of a block has no name yet (S6). */
    if (m->ctors[i].name != NULL && strcmp(m->ctors[i].name, name) == 0) {
      h->kind = HEAD_CTOR;
      h->index = i;
      return 1;
    }
  }
  for (i = 0; i < m->ctor_count; i++) {
    /* Record projections require a point constructor. A path constructor
       can reduce to its boundary before a projection can select a field. */
    for (j = 0; m->families[m->ctors[i].family].ctor_count == 1u && m->ctors[i].interval_count == 0u && j < m->ctors[i].field_count; j++) {
      if (strcmp(m->ctors[i].fields[j].name, name) == 0) {
        h->kind = HEAD_PROJ;
        h->index = i;
        h->field = j;
        return 1;
      }
    }
  }
  for (i = 0; i < BUILTIN_COUNT; i++) {
    if (strcmp(BUILTINS[i].name, name) == 0) {
      h->kind = HEAD_BUILTIN;
      h->builtin = &BUILTINS[i];
      return 1;
    }
  }
  return 0;
}

/* An unknown name. When it names this declaration or a later one, the use
   is recursion (rule R3). */
static int unknown_name(Checker *c, const char *name) {
  size_t i;
  if (time_word(name))
    return FAIL(c, "REFUSE_TIME", TIME_HINT, name);
  for (i = c->decl_index; i < c->decls->count; i++) {
    if (strcmp(c->decls->items[i].name, name) == 0)
      return FAIL(c, "REFUSE_REC", "%s is this definition or a later one; a definition can use only earlier ones (rule R3)", name);
  }
  return FAIL(c, "NAME_UNKNOWN", "unknown name %s", name);
}

/* A program declaration in guest mode (orbit-lang S3). The domain file and
   the contract prelude are the trusted scope: no kit refusal there. */
static int guest(const Checker *c) {
  return c->mode == CHECK_GUEST && c->decls->items[c->decl_index].origin == ORIGIN_PROGRAM;
}

static int kit_refused(Checker *c, const char *what) {
  return FAIL(c, "REFUSE_KIT", "%s is a kit item outside the base span (PLAN section 4); only the kit mode langc --kit admits it", what);
}

/* An INTERNAL core name, or a family, constructor or field of the contract
   prelude (ORIGIN_KIT). */
static int kit_head(const Checker *c, const Head *h) {
  const Machine *m = c->m;
  uint32_t fam = NO_FAMILY;
  switch (h->kind) {
  case HEAD_TERM:
    return 0;
  case HEAD_BUILTIN:
    return h->builtin->vis == VIS_INTERNAL;
  case HEAD_FAMILY:
    fam = h->index;
    break;
  case HEAD_CTOR:
  case HEAD_PROJ:
    fam = m->ctors[h->index].family;
    break;
  }
  return fam != NO_FAMILY && m->families[fam].origin == ORIGIN_KIT;
}

/* Name resolution order: locals (the innermost first), definitions,
   families, constructors, projections, core names. */
static int resolve_head(Checker *c, const Term *head, Head *h) {
  memset(h, 0, sizeof *h);
  h->kind = HEAD_TERM;
  if (head->kind != TERM_VAR || find_local(c, head->name) != NULL || find_def(c->m, head->name) != NO_DEF)
    return 1;
  if (!find_known(c, head->name, h))
    return unknown_name(c, head->name);
  if (guest(c) && h->kind == HEAD_BUILTIN && h->builtin->vis == VIS_CELL)
    return FAIL(c, "REFUSE_CELL", "%s is a cons-list name, not a cell monad; a guest program uses FreeMon of domain/domain.lang (rule R1)", head->name);
  return guest(c) && kit_head(c, h) ? kit_refused(c, head->name) : 1;
}

/* A Nat or a word literal is a kit item (orbit-lang S3). */
static int literal_ok(Checker *c, const char *what) {
  return guest(c) ? kit_refused(c, what) : 1;
}

static uint32_t known_arity(const Checker *c, const Head *h) {
  const CtorInfo *ci = h->kind == HEAD_CTOR ? &c->m->ctors[h->index] : NULL;
  switch (h->kind) {
  case HEAD_TERM:
    return 0u;
  case HEAD_FAMILY:
    return c->m->families[h->index].param_count + c->m->families[h->index].index_count;
  case HEAD_CTOR:
    return ci == NULL ? 0u : c->m->families[ci->family].param_count + ci->field_count + ci->interval_count;
  case HEAD_PROJ:
    return 1u;
  case HEAD_BUILTIN:
    return h->builtin->arity;
  }
  return 0u;
}

static const char *known_name(const Checker *c, const Head *h) {
  switch (h->kind) {
  case HEAD_TERM:
    return "?";
  case HEAD_FAMILY:
    return c->m->families[h->index].name;
  case HEAD_CTOR:
    return c->m->ctors[h->index].name;
  case HEAD_PROJ:
    return c->m->ctors[h->index].fields[h->field].name;
  case HEAD_BUILTIN:
    return h->builtin->name;
  }
  return "?";
}

static const Core *known_core(Call *k, const Head *h) {
  switch (h->kind) {
  case HEAD_TERM:
    return nul(FAIL(k->c, "INTERNAL", "a plain term reached the core name rules"));
  case HEAD_FAMILY:
    return family_app(k, h->index);
  case HEAD_CTOR:
    return ctor_app(k, h->index);
  case HEAD_PROJ:
    return proj_app(k, h->index, h->field);
  case HEAD_BUILTIN:
    return rule(k);
  }
  return NULL;
}

/* An under-applied core name checked against a function type: WHOLE becomes
   `fun x => WHOLE x`, with a name that the lexer cannot make. */
static const Core *eta(Checker *c, const Term *whole, const Value *hint, const Value **type) {
  char *name = arena_alloc(c->m->arena, 24);
  Term *x = arena_alloc(c->m->arena, sizeof *x);
  Term *app = arena_alloc(c->m->arena, sizeof *app);
  Term *lam = arena_alloc(c->m->arena, sizeof *lam);
  if (name == NULL || x == NULL || app == NULL || lam == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  c->eta_count++;
  snprintf(name, 24, "%%eta%u", c->eta_count);
  x->kind = TERM_VAR;
  x->name = name;
  app->kind = TERM_APP;
  app->left = whole;
  app->right = x;
  lam->kind = TERM_LAM;
  lam->name = name;
  lam->right = app;
  return check_lam(c, lam, hint, type);
}

/* Applies K of type TY to ARGS, one by one. */
static const Core *apply_rest(Checker *c, const Core *k, const Value *ty, const Term *const *args, uint32_t argc, const Value *hint, const Value **type) {
  uint32_t i;
  for (i = 0; i < argc && k != NULL; i++) {
    const Core *a;
    if (ty == NULL || ty->kind != VAL_PI)
      return nul(FAIL(c, "TYPE_NOT_FUNCTION", "a term of type %s is applied to an argument", show(c, ty)));
    a = check(c, args[i], ty->dom);
    ty = closure_apply(c->m, ty, here(c, a));
    k = core_app(c, k, a);
  }
  *type = ty;
  return finish(c, k, hint, type);
}

static const Core *var_core(Checker *c, const char *name, const Value **type) {
  const Scope *s = find_local(c, name);
  uint32_t d = find_def(c->m, name);
  if (s != NULL) {
    *type = s->type;
    if (s->level >= c->field_base && s->level < c->field_top) {
      c->field_dep = 1;
      c->field_rec |= val_is(s->type, OP_FAMILY) && in_block(c, s->type->inst);
    }
    return core_index(c, CORE_VAR, c->level - 1u - s->level);
  }
  if (d != NO_DEF) {
    *type = c->m->defs[d].type;
    return core_index(c, CORE_GLOBAL, d);
  }
  if (time_word(name))
    return nul(FAIL(c, "REFUSE_TIME", TIME_HINT, name));
  return nul(FAIL(c, "NAME_UNKNOWN", "unknown name %s", name));
}

static const Core *apply_term(Checker *c, const Term *head, const Term *const *args, uint32_t argc, const Value *hint, const Value **type) {
  const Value *ty = NULL;
  const Core *k = head->kind == TERM_VAR ? var_core(c, head->name, &ty) : infer(c, head, &ty);
  return apply_rest(c, k, ty, args, argc, hint, type);
}

static const Core *apply_known(Checker *c, const Term *whole, const Head *h, const Term *const *args, uint32_t argc, const Value *hint, const Value **type) {
  uint32_t arity = known_arity(c, h);
  Call k;
  const Core *core;
  if (h->kind == HEAD_BUILTIN && h->builtin->rule == RULE_ELIM && argc >= 3u &&
      hint != NULL && hint->kind == VAL_PI) {
    const Value *pt = NULL;
    const Value *dom = NULL;
    uint32_t d = 0;
    /* An indexed motive starts with its indices, so its first binder
       does not determine the number of constructor cases. */
    if (try_infer(c, args[0], &pt) != NULL) {
      Diag saved = *c->m->diag;
      dom = motive_domain(c, pt, &d);
      *c->m->diag = saved;
    }
    if (dom == NULL)
      dom = elim_domain(c, args, argc);
    /* A bare motive can acquire its domain from the scrutinee after eta. */
    if (dom == NULL)
      return eta(c, whole, hint, type);
    if (val_is(dom, OP_SUM))
      arity = 4u;
    else if (val_is(dom, OP_FAMILY))
      arity = c->m->families[dom->inst].ctor_count + 2u;
  }
  if (argc < arity && hint != NULL && hint->kind == VAL_PI)
    return eta(c, whole, hint, type);
  if (argc < arity)
    return nul(FAIL(c, "TYPE_ARITY", "%s needs %u arguments, found %u", known_name(c, h), arity, argc));
  memset(&k, 0, sizeof k);
  k.c = c;
  k.b = h->builtin;
  k.args = args;
  k.argc = argc;
  k.hint = argc == arity ? hint : NULL;
  k.outer = hint;
  k.used = arity;
  core = known_core(&k, h);
  return apply_rest(c, core, k.type, args + k.used, argc - k.used, hint, type);
}

static const Core *elab_spine(Checker *c, const Term *t, const Value *hint, const Value **type) {
  const Term **args;
  const Term *head = t;
  uint32_t argc = 0;
  uint32_t i;
  Head h;
  while (head->kind == TERM_APP) {
    argc++;
    head = head->left;
  }
  if (argc > SPINE_MAX)
    return nul(FAIL(c, "TYPE_ARITY", "an application has more than %u arguments", SPINE_MAX));
  args = arena_alloc(c->m->arena, ((size_t)argc + 1u) * sizeof *args);
  if (args == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  for (head = t, i = argc; head->kind == TERM_APP; head = head->left)
    args[--i] = head->right;
  if (!resolve_head(c, head, &h))
    return NULL;
  return h.kind == HEAD_TERM ? apply_term(c, head, args, argc, hint, type) : apply_known(c, t, &h, args, argc, hint, type);
}

/* Retain the checked syntax before conversion erases law-only references. */
static const Core *checked_annotation(Checker *c, const Term *ann, const Value *dom) {
  uint64_t level = 0;
  const Core *a = check_type(c, ann, &level);
  const Value *v = here(c, a);
  if (v == NULL)
    return NULL;
  return conv_values(c->m, c->level, v, dom) ? a : nul(FAIL(c, "TYPE_MISMATCH", "the binder type %s differs from the expected type %s", show(c, v), show(c, dom)));
}

/* A path abstraction against the path type HINT: the body at the interval
   variable, then the ends at i0 and i1. */
static const Core *check_plam(Checker *c, const Term *t, const Value *hint, const Value **type) {
  const Core *body;
  if (t->left != NULL && !is_interval(c, t->left))
    return nul(FAIL(c, "TYPE_MISMATCH", "a term of type %s is a path; its binder must be an interval variable fun (%s : I) => ...", show(c, hint), t->name));
  if (!push_interval(c, t->name))
    return NULL;
  body = check(c, t->right, closure_apply(c->m, hint, c->env->value));
  pop(c);
  if (body == NULL || !path_ends(c, hint, at(c, body, val_iend(c->m, 0)), at(c, body, val_iend(c->m, 1))))
    return NULL;
  *type = hint;
  return core_bind(c, CORE_PLAM, t->name, NULL, body);
}

/* fun (i : I) => t with no expected type: PathP (fun i => T) t[i0] t[i1]. */
static const Core *infer_plam(Checker *c, const Term *t, const Value **type) {
  const Value *bt = NULL;
  const Core *body;
  if (!push_interval(c, t->name))
    return NULL;
  body = infer(c, t->right, &bt);
  pop(c);
  if (body == NULL)
    return NULL;
  if (bt == NULL || val_is(bt, OP_INTERVAL))
    return nul(FAIL(c, "INTERVAL_TYPE", "the path fun (%s : I) => ... has values in I; the line of a path must be a type (rule R9)", t->name));
  *type = here(c, core_ends(c, CORE_PATHP, t->name, NULL, quote_value(c->m, c->level + 1u, bt), quote_value(c->m, c->level, at(c, body, val_iend(c->m, 0))), quote_value(c->m, c->level, at(c, body, val_iend(c->m, 1)))));
  return core_bind(c, CORE_PLAM, t->name, NULL, body);
}

/* The literal bound of a Fin n domain, or 0 when there is no finAll: the
   bound must leave room for the k argument in the uint32_t arity. */
static uint64_t fin_cases(const Value *dom) {
  if (!val_is(dom, OP_FIN) || dom->args[0]->kind != VAL_NAT || dom->args[0]->nat >= UINT32_MAX)
    return 0;
  return dom->args[0]->nat;
}

/* finAll (S3a): the body at each literal j below n, with the binder bound
   to j. The core is OP_FIN_CASE (k, body0, ..., body(n-1)). */
static const Core *check_fin_all(Checker *c, const Term *t, const Value *hint, uint64_t n) {
  const Core **args = arena_alloc(c->m->arena, ((size_t)n + 1u) * sizeof *args);
  char inner[sizeof c->m->diag->msg];
  const char *code;
  Diag clean = *c->m->diag;
  uint64_t j;
  if (args == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  args[0] = core_index(c, CORE_VAR, 0);
  for (j = 0; j < n; j++) {
    const Value *x = val_nat(c->m, j);
    if (!push_value(c, t->name, hint->dom, x))
      return NULL;
    args[j + 1u] = check(c, t->right, closure_apply(c->m, hint, x));
    pop(c);
    if (args[j + 1u] == NULL) {
      code = c->m->diag->code != NULL ? c->m->diag->code : "-";
      if (strcmp(code, "OOM") == 0)
        return NULL;
      snprintf(inner, sizeof inner, "%s", c->m->diag->msg);
      *c->m->diag = clean;
      return nul(FAIL(c, "FIN_CASE", "the case %llu of Fin %llu fails: %s: %s", (unsigned long long)j, (unsigned long long)n, code, inner));
    }
  }
  return core_op(c, OP_FIN_CASE, 0, 0, args, (uint32_t)n + 1u);
}

static const Core *check_lam(Checker *c, const Term *t, const Value *hint, const Value **type) {
  const Core *body;
  const Core *ann = NULL;
  Diag clean;
  if (hint->kind == VAL_PATHP)
    return check_plam(c, t, hint, type);
  if (hint->kind != VAL_PI)
    return nul(FAIL(c, "TYPE_MISMATCH", "a function is not a term of type %s", show(c, hint)));
  if (t->left != NULL && (ann = checked_annotation(c, t->left, hint->dom)) == NULL)
    return NULL;
  clean = *c->m->diag;
  if (!push(c, t->name, hint->dom))
    return NULL;
  body = check(c, t->right, closure_apply(c->m, hint, c->env->value));
  pop(c);
  /* finAll: the ordinary check failed on a Fin n domain (literal n), so
     check each case. The ordinary core stays when the ordinary check passes. */
  if (body == NULL && fin_cases(hint->dom) > 0 && strcmp(c->m->diag->code != NULL ? c->m->diag->code : "-", "OOM") != 0) {
    *c->m->diag = clean;
    body = check_fin_all(c, t, hint, fin_cases(hint->dom));
  }
  *type = hint;
  return core_bind(c, CORE_LAM, t->name, ann, body);
}

static const Core *infer_lam(Checker *c, const Term *t, const Value **type) {
  uint64_t level = 0;
  const Value *dom;
  const Value *bt = NULL;
  const Core *body;
  const Core *quoted;
  const Core *ann;
  if (t->left == NULL)
    return nul(FAIL(c, "TYPE_INFER", "fun %s => ... needs a binder type here", t->name));
  if (is_interval(c, t->left))
    return infer_plam(c, t, type);
  ann = check_type(c, t->left, &level);
  dom = here(c, ann);
  if (!push(c, t->name, dom))
    return NULL;
  body = infer(c, t->right, &bt);
  quoted = body == NULL ? NULL : quote_value(c->m, c->level, bt);
  pop(c);
  *type = val_closure(c->m, VAL_PI, t->name, dom, c->env, quoted);
  return core_bind(c, CORE_LAM, t->name, ann, body);
}

/* (x : A) -> B and Sigma (x : A) B. */
static const Core *elab_binder(Checker *c, const Term *t, CoreKind kind, const Value *hint, const Value **type) {
  uint64_t la = 0;
  uint64_t lb = 0;
  const Core *a = check_type(c, t->left, &la);
  const Core *b;
  if (a == NULL || !push(c, t->name, here(c, a)))
    return NULL;
  b = check_type(c, t->right, &lb);
  pop(c);
  *type = val_univ(c->m, la > lb ? la : lb);
  return finish(c, core_bind(c, kind, t->name, a, b), hint, type);
}

/* p @ r: p : PathP L a b and r : I give L[r]. The core keeps a and b. */
static const Core *elab_papp(Checker *c, const Term *t, const Value *hint, const Value **type) {
  const Value *pt = NULL;
  const Core *p = infer(c, t->left, &pt);
  const Core *r;
  if (p == NULL)
    return NULL;
  if (pt == NULL || pt->kind != VAL_PATHP)
    return nul(FAIL(c, "TYPE_MISMATCH", "@ needs a path of type PathP L a b, found a term of type %s", show(c, pt)));
  r = check(c, t->right, interval_type(c));
  *type = r == NULL ? NULL : closure_apply(c->m, pt, here(c, r));
  return finish(c, core_ends(c, CORE_PAPP, NULL, p, r, quote_value(c->m, c->level, pt->args[0]), quote_value(c->m, c->level, pt->args[1])), hint, type);
}

/* ~r, r /\ s and r \/ s: the operands and the result are in I. */
static const Core *elab_ival(Checker *c, const Term *t, CoreKind kind, const Value *hint, const Value **type) {
  const Value *iv = interval_type(c);
  const Core *a = check(c, t->left, iv);
  const Core *b = kind == CORE_INEG || a == NULL ? NULL : check(c, t->right, iv);
  *type = iv;
  return finish(c, core_iop(c, kind, a, b), hint, type);
}

static const Core *elab_inner(Checker *c, const Term *t, const Value *hint, const Value **type) {
  switch (t->kind) {
  case TERM_VAR:
  case TERM_APP:
    return elab_spine(c, t, hint, type);
  case TERM_NAT:
    if (!literal_ok(c, "a Nat literal"))
      return NULL;
    if (val_is(hint, OP_FIN) && hint->args[0]->kind == VAL_NAT) {
      if (t->nat >= hint->args[0]->nat)
        return nul(FAIL(c, "FIN_RANGE", "the literal %llu is not below %llu, so it is not a value of Fin %llu", (unsigned long long)t->nat, (unsigned long long)hint->args[0]->nat, (unsigned long long)hint->args[0]->nat));
      *type = hint;
      return core_leaf(c, CORE_NAT, t->nat);
    }
    *type = nat_type(c);
    return finish(c, core_leaf(c, CORE_NAT, t->nat), hint, type);
  case TERM_WORD:
    if (!literal_ok(c, "a word literal"))
      return NULL;
    *type = tyop(c, t->nat == WORD_ADDR ? OP_ADDR : OP_U256, 0, NULL, NULL, NULL);
    return finish(c, core_word(c, t->nat, t->word), hint, type);
  case TERM_LAM:
    return hint != NULL ? check_lam(c, t, hint, type) : infer_lam(c, t, type);
  case TERM_PI:
    return elab_binder(c, t, CORE_PI, hint, type);
  case TERM_SIGMA:
    return elab_binder(c, t, CORE_SIGMA, hint, type);
  case TERM_UNIV:
    if (t->nat >= UNIVERSE_MAX)
      return nul(FAIL(c, "TYPE_UNIVERSE", "Type %llu is above the largest universe Type %u", (unsigned long long)t->nat, UNIVERSE_MAX - 1u));
    *type = val_univ(c->m, t->nat + 1u);
    return finish(c, core_leaf(c, CORE_UNIV, t->nat), hint, type);
  case TERM_PAPP:
    return elab_papp(c, t, hint, type);
  case TERM_INEG:
    return elab_ival(c, t, CORE_INEG, hint, type);
  case TERM_IMEET:
    return elab_ival(c, t, CORE_IMEET, hint, type);
  case TERM_IJOIN:
    return elab_ival(c, t, CORE_IJOIN, hint, type);
  case TERM_SYSTEM:
  case TERM_BRANCH:
    return nul(FAIL(c, "TYPE_MISMATCH", "a system [ phi -> t, ... ] is only the tube of hcomp, fun (i : I) => [ ... ]"));
  }
  return NULL;
}

static const Core *elab(Checker *c, const Term *t, const Value *hint, const Value **type) {
  const Core *k;
  if (c->depth >= CHECK_DEPTH_LIMIT)
    return nul(FAIL(c, "CHECK_DEPTH", "the term is nested deeper than %u levels", CHECK_DEPTH_LIMIT));
  c->depth++;
  k = elab_inner(c, t, hint, type);
  c->depth--;
  return k;
}

/* NULL when NAME is free, else the reason it is taken (rule R4). */
static const char *name_owner(const Checker *c, const char *name) {
  const Machine *m = c->m;
  uint32_t i;
  uint32_t j;
  for (i = 0; i < sizeof RESERVED / sizeof RESERVED[0]; i++) {
    if (strcmp(RESERVED[i], name) == 0)
      return "a reserved name";
  }
  for (i = 0; i < BUILTIN_COUNT; i++) {
    if (strcmp(BUILTINS[i].name, name) == 0)
      return "a core name";
  }
  for (i = 0; i < m->family_count; i++) {
    if (strcmp(m->families[i].name, name) == 0)
      return "a domain family";
  }
  for (i = 0; i < m->ctor_count; i++) {
    for (j = 0; !m->ctors[i].event && j < m->ctors[i].field_count; j++) {
      if (strcmp(m->ctors[i].fields[j].name, name) == 0)
        return "a domain field";
    }
    if (m->ctors[i].name != NULL && strcmp(m->ctors[i].name, name) == 0)
      return m->ctors[i].event ? "an event" : "a domain constructor";
  }
  i = find_def(m, name);
  if (i == NO_DEF)
    return NULL;
  return m->defs[i].origin != ORIGIN_PROGRAM ? "a domain definition" : "already defined";
}

static int name_free(Checker *c, const char *name) {
  const char *why;
  if (time_word(name))
    return FAIL(c, "REFUSE_TIME", TIME_HINT, name);
  why = name_owner(c, name);
  return why == NULL ? 1 : FAIL(c, "REFUSE_NAME", "%s is %s (rule R4)", name, why);
}

/* T is a sort of the block of FAM at the parameter variables of FAM
   `F p1 .. pn`, then any index arguments (S6), in the context of the
   parameters and SHIFT earlier fields (S4 items 9 and 13). */
static int own_family(const Machine *m, uint32_t fam, const Core *t, uint32_t shift) {
  const FamilyInfo *f = &m->families[fam];
  uint32_t p = f->param_count;
  uint32_t k;
  if (t->kind != CORE_OP || t->op != OP_FAMILY || t->inst < f->block_first || t->inst - f->block_first >= f->block_count)
    return 0;
  if (m->families[t->inst].param_count != p || t->argc != p + m->families[t->inst].index_count)
    return 0;
  for (k = 0; k < p; k++)
    if (t->args[k]->kind != CORE_VAR || t->args[k]->index != p - 1u - k + shift)
      return 0;
  return 1;
}

/* A family with a type parameter is a type constructor, so it must be a
   cell signature (S3, rule R1): only type parameters, no and block, each
   type parameter the one field of exactly one generator constructor, and
   every other point field closed data, the family at its own parameters
   or a path in it. REFUSE_CELL comes before DOMAIN_FAMILY. */
static int cell_type_argument(const Core *t) {
  while (t->kind == CORE_PI)
    t = t->b;
  return t->kind == CORE_UNIV;
}

static int has_type_param(const FamilyInfo *f) {
  uint32_t k;
  for (k = 0; k < f->param_count; k++)
    if (cell_type_argument(f->param_types[k]))
      return 1;
  return 0;
}

/* The type parameter that T is, in the context of the parameters and
   EARLIER fields, or the parameter count when T is not one. */
static uint32_t cell_param(const FamilyInfo *f, const Core *t, uint32_t earlier) {
  uint32_t p = f->param_count;
  uint32_t k = t->kind == CORE_VAR && t->index >= earlier && t->index - earlier < p ? p - 1u - (t->index - earlier) : p;
  return k < p && f->param_types[k]->kind == CORE_UNIV ? k : p;
}

static int cell_header(Checker *c, const Decl *d, uint32_t first, uint32_t count) {
  uint32_t s, k;
  for (s = 0; s < count; s++) {
    const FamilyInfo *f = &c->m->families[first + s];
    for (k = 0; k < f->index_count; k++)
      if (cell_type_argument(f->index_types[k]))
        return FAIL(c, "REFUSE_CELL", "%s has a type index; a cell signature binds its type arguments as uniform parameters (rule R1)", f->name);
    for (k = 0; has_type_param(f) && k < f->param_count; k++) {
      if (count > 1u && f->param_types[k]->kind == CORE_UNIV)
        return FAIL(c, "REFUSE_CELL", "%s has the type parameter %s in an and block; a type constructor is one cell signature (rule R1)", f->name, d[s].params[k].name);
      if (f->param_types[k]->kind != CORE_UNIV)
        return FAIL(c, "REFUSE_CELL", "%s has a type argument and the parameter %s is not a universe; a cell signature has only type parameters (rule R1)", f->name, d[s].params[k].name);
    }
  }
  return 1;
}

/* Closed operation labels cannot depend on a type parameter or an earlier
   field. Binders inside the label type still bind their own variables. */
static int cell_closed(const Core *t, uint32_t depth) {
  uint32_t i;
  int binder;
  if (t == NULL)
    return 1;
  if (t->kind == CORE_VAR)
    return t->index < depth;
  binder = t->kind == CORE_PI || t->kind == CORE_SIGMA || t->kind == CORE_LAM || t->kind == CORE_PLAM || t->kind == CORE_PATHP;
  if (!cell_closed(t->a, depth) || !cell_closed(t->b, depth + (uint32_t)binder))
    return 0;
  for (i = 0; i < t->argc; i++)
    if (!cell_closed(t->args[i], depth))
      return 0;
  return 1;
}

static int cell_field(Checker *c, uint32_t fam, const FieldInfo *out, uint32_t earlier, size_t field_count) {
  const FamilyInfo *f = &c->m->families[fam];
  const Core *t = out->type;
  if (cell_param(f, t, earlier) < f->param_count)
    return field_count == 1u ? 1 : FAIL(c, "REFUSE_CELL", "%s is not a cell signature: the field %s has a type parameter as its type, so it must be the one field of its constructor (rule R1)", f->name, out->name);
  if (own_family(c->m, fam, t, earlier) || (t->kind == CORE_PATHP && own_family(c->m, fam, t->b, earlier + 1u)))
    return 1;
  if (c->self_uses == 0u && cell_closed(t, 0u))
    return 1;
  return FAIL(c, "REFUSE_CELL", "%s is not a cell signature: the field %s must be a type parameter, closed data, %s at its own parameters or a path in %s (rule R1)", f->name, out->name, f->name, f->name);
}

/* Each type parameter has exactly one generator (S3, open point QA1:
   two generators for one parameter are refused). */
static int cell_generators(Checker *c, const Decl *d, uint32_t fam) {
  const Machine *m = c->m;
  const FamilyInfo *f = &m->families[fam];
  uint32_t k, i;
  for (k = 0; k < f->param_count; k++) {
    uint32_t gens = 0;
    for (i = 0; i < f->ctor_count; i++)
      gens += m->ctors[f->first_ctor + i].field_count == 1u && cell_param(f, m->ctors[f->first_ctor + i].fields[0].type, 0u) == k;
    if (gens == 0u)
      return FAIL(c, "REFUSE_CELL", "%s is not a cell signature: the type parameter %s occurs in no constructor (rule R1)", f->name, d->params[k].name);
    if (gens > 1u)
      return FAIL(c, "REFUSE_CELL", "%s is not a cell signature: the type parameter %s has %u generators (rule R1)", f->name, d->params[k].name, (unsigned)gens);
  }
  return 1;
}

/* A recursive field must be the whole field type, the family at its own
   parameters. The EARLIER point fields are locals (S4 item 13). */
static int check_field(Checker *c, uint32_t fam, const Field *field, FieldInfo *out, uint32_t earlier, size_t field_count) {
  uint64_t level = 0;
  const Core *t;
  if (!name_free(c, field->name))
    return 0;
  c->self_uses = 0;
  c->field_base = c->level - earlier;
  c->field_top = c->level;
  c->field_dep = 0;
  c->field_rec = 0;
  t = check_type(c, field->type, &level);
  c->field_top = c->field_base;
  if (t == NULL)
    return 0;
  if (level > c->m->families[fam].universe)
    c->m->families[fam].universe = level;
  out->name = field->name;
  out->type = t;
  /* A path type over earlier recursive fields names the family, but it is
     a path field, not a recursive field (S4 item 14). */
  out->recursive = c->self_uses > 0u && !(t->kind == CORE_PATHP && c->field_rec);
  out->dependent = c->field_dep;
  out->over_recursive = c->field_rec;
  out->history = field->history;
  if (has_type_param(&c->m->families[fam]) && !cell_field(c, fam, out, earlier, field_count))
    return 0;
  if (out->recursive && !own_family(c->m, fam, t, earlier))
    return FAIL(c, "DOMAIN_FAMILY", "the recursive field %s must have the type %s at its own parameters", field->name, c->m->families[fam].name);
  return 1;
}

/* The field EARLIER of FIELDS is a path field when it uses an earlier
   recursive field and its type is `Path F x y`: F the family at its own
   parameters, x and y earlier recursive fields (S4 item 14). An other
   field over a recursive field has no fold case type. */
static void path_field(const Machine *m, uint32_t fam, FieldInfo *fields, uint32_t earlier) {
  FieldInfo *f = &fields[earlier];
  const Core *t = f->type;
  int side;
  f->path = f->over_recursive && t->kind == CORE_PATHP && own_family(m, fam, t->b, earlier + 1u);
  for (side = 0; side < 2; side++) {
    const Core *e = f->path ? t->args[side] : NULL;
    f->path = f->path && e != NULL && e->kind == CORE_VAR && e->index < earlier && fields[earlier - 1u - e->index].recursive;
    f->ends[side] = f->path ? earlier - 1u - e->index : 0u;
  }
}

/* A term that the checker makes, at the place of CTOR. */
static Term *made_term(Checker *c, const Ctor *ctor, TermKind kind, const char *name, const Term *left, const Term *right) {
  Term *t = arena_alloc(c->m->arena, sizeof *t);
  if (t == NULL) {
    FAIL(c, "OOM", "out of memory");
    return NULL;
  }
  t->kind = kind;
  t->line = ctor->line;
  t->col = ctor->col;
  t->name = name;
  t->nat = 0;
  t->left = left;
  t->right = right;
  return t;
}

/* COVER \/ (~NAME \/ NAME), or ~NAME \/ NAME when COVER is NULL. */
static const Term *cover_term(Checker *c, const Ctor *ctor, const Term *cover, const char *name) {
  const Term *v = made_term(c, ctor, TERM_VAR, name, NULL, NULL);
  const Term *neg = v == NULL ? NULL : made_term(c, ctor, TERM_INEG, NULL, v, NULL);
  const Term *ends = neg == NULL ? NULL : made_term(c, ctor, TERM_IJOIN, NULL, neg, v);
  return cover == NULL || ends == NULL ? ends : made_term(c, ctor, TERM_IJOIN, NULL, cover, ends);
}

/* A branch of a path constructor of the indexed sort FAM failed (S6). A
   body of the type FAM at another index is HIT_INDEX (the gate row "a
   wrong index in a path constructor"), not the TYPE_MISMATCH of the
   branch check. */
static int branch_index(Checker *c, uint32_t fam, const Branch *b) {
  Diag saved = *c->m->diag;
  const Value *t = NULL;
  if (c->m->families[fam].index_count == 0u || saved.code == NULL || strcmp(saved.code, "TYPE_MISMATCH") != 0 || b->term == NULL || b->term->right == NULL)
    return 0;
  c->m->diag->set = 0;
  if (infer(c, b->term->right, &t) != NULL && val_is(t, OP_FAMILY) && t->inst == fam) {
    *c->m->diag = saved;
    c->m->diag->set = 0;
    return FAIL(c, "HIT_INDEX", "a boundary term of a path constructor of %s has the type %s, the family at another index", c->m->families[fam].name, show(c, t));
  }
  *c->m->diag = saved;
  return 0;
}

/* The boundary of CTOR under its fields: the first POINTS fields of CTOR
   (with FIELDS their checked types) and then its interval fields are
   locals. The family parameters are already locals (check_family). Each
   branch has the family at the parameters and RESULT, the result indices
   of CTOR (S6), NULL for a uniform family. */
static const Core *boundary_core(Checker *c, uint32_t fam, const Ctor *ctor, const FieldInfo *fields, size_t points, const Core *const *result) {
  Machine *m = c->m;
  uint32_t p = m->families[fam].param_count;
  uint32_t q = result == NULL ? p : p + m->families[fam].index_count;
  const Value **params = arena_alloc(m->arena, ((size_t)q + 1u) * sizeof *params);
  Term *tube = arena_alloc(m->arena, sizeof *tube);
  const Term *cover = NULL;
  const Core *phi;
  const Core *a;
  Branch *bs;
  uint32_t n = 0;
  uint32_t i;
  size_t j;
  if (params == NULL || tube == NULL)
    return nul(FAIL(c, "OOM", "out of memory"));
  for (i = 0; i < p; i++)
    params[i] = val_var(m, c->level - p + i);
  /* Field j is over the earlier fields: evaluate it after they are pushed. */
  for (j = 0; j < points; j++)
    if (!push(c, fields[j].name, here(c, fields[j].type)))
      return NULL;
  /* The result indices at the parameters and the point fields (S6). */
  for (i = p; i < q; i++) {
    params[i] = here(c, result[i - p]);
    if (params[i] == NULL)
      return NULL;
  }
  for (j = points; j < ctor->field_count; j++) {
    cover = cover_term(c, ctor, cover, ctor->fields[j].name);
    if (cover == NULL || !push_interval(c, ctor->fields[j].name))
      return NULL;
  }
  phi = check(c, cover, interval_type(c));
  a = quote_value(m, c->level, val_op(m, VAL_OP, OP_FAMILY, fam, 0, params, q));
  if (phi == NULL || a == NULL)
    return NULL;
  tube->kind = TERM_LAM;
  tube->line = ctor->line;
  tube->col = ctor->col;
  tube->name = NULL;
  tube->nat = 0;
  tube->left = NULL;
  tube->right = ctor->boundary;
  bs = system_branches(c, tube, here(c, phi), &n);
  if (bs == NULL)
    return NULL;
  for (i = 0; i < n; i++) {
    bs[i].body = hcomp_branch(c, NULL, a, &bs[i]);
    if (bs[i].body == NULL)
      return nul(branch_index(c, fam, &bs[i]));
  }
  if (!system_overlaps(c, "_", bs, n))
    return NULL;
  return system_core(c, "_", bs, n);
}

/* The boundary `[ k1 -> t1, ... ]` of a path constructor (S4 R1 (a)),
   checked as the tube of an hcomp at the family: each branch has the
   family type at the parameters, the keys cover ~i \/ i (and ~j \/ j)
   (SYSTEM_COVER) and two branches agree where both keys hold
   (SYSTEM_OVERLAP). The core is OP_SYSTEM, each body under a path binder
   that no term can use, as an hcomp tube. The context comes back as it
   was. */
static const Core *check_boundary(Checker *c, uint32_t fam, const Ctor *ctor, const FieldInfo *fields, size_t points, const Core *const *result) {
  const Scope *scope = c->scope;
  const Env *env = c->env;
  uint32_t level = c->level;
  const Core *sys = boundary_core(c, fam, ctor, fields, points, result);
  c->scope = scope;
  c->env = env;
  c->level = level;
  return sys;
}

/* The result `: F params indices` of CTOR under its POINTS point fields
   (S6). A constructor of an indexed sort states it: its head is the sort
   at its own parameters, and its index arguments check against the index
   telescope. A constructor of a uniform sort can state the sort at its
   parameters. RESULT gets the index cores, or NULL. */
static int ctor_result(Checker *c, uint32_t fam, const Ctor *ctor, uint32_t points, const Core *const **result) {
  const FamilyInfo *f = &c->m->families[fam];
  uint64_t level = 0;
  const Core *t;
  *result = NULL;
  if (ctor->result == NULL)
    return f->index_count == 0u ? 1 : FAIL(c, "HIT_INDEX", "the constructor %s of the indexed family %s needs a result `: %s ...` after its fields", ctor->name, f->name, f->name);
  t = check_type(c, ctor->result, &level);
  if (t == NULL)
    return 0;
  if (!own_family(c->m, fam, t, points) || t->inst != fam)
    return FAIL(c, "HIT_INDEX", "the result of %s must be %s at its own parameters", ctor->name, f->name);
  *result = f->index_count == 0u ? NULL : t->args + f->param_count;
  return 1;
}

/* A constructor takes point fields, then (a path constructor) 1 or 2
   interval fields `(i : I)` and a boundary. It fills the reserved slot
   SLOT (S6). */
static int check_ctor(Checker *c, uint32_t fam, const Ctor *ctor, uint32_t slot) {
  Machine *m = c->m;
  FieldInfo *fields = arena_alloc(m->arena, (ctor->field_count + 1u) * sizeof *fields);
  CtorInfo *ci = &m->ctors[slot];
  const Core *const *result = NULL;
  const Scope *scope = c->scope;
  const Env *env = c->env;
  uint32_t level = c->level;
  const Core *boundary = NULL;
  size_t points = 0;
  size_t j;
  if (fields == NULL)
    return FAIL(c, "OOM", "out of memory");
  if (!name_free(c, ctor->name))
    return 0;
  for (j = 0; j < ctor->field_count; j++) {
    int interval = is_interval(c, ctor->fields[j].type);
    if (strcmp(ctor->fields[j].name, ctor->name) == 0)
      return FAIL(c, "REFUSE_NAME", "%s is this domain constructor (rule R4)", ctor->name);
    for (size_t k = 0; k < j; k++)
      if (strcmp(ctor->fields[j].name, ctor->fields[k].name) == 0)
        return FAIL(c, "REFUSE_NAME", "%s is already a field of this domain constructor (rule R4)", ctor->fields[j].name);
    if (!interval && points < j)
      return FAIL(c, "HIT_BOUNDARY", "the point field %s of %s must come before the interval fields", ctor->fields[j].name, ctor->name);
    if (interval && !name_free(c, ctor->fields[j].name))
      return 0;
    if (!interval && !check_field(c, fam, &ctor->fields[j], &fields[j], (uint32_t)points, ctor->field_count))
      return 0;
    if (!interval) {
      path_field(m, fam, fields, (uint32_t)points);
      if (c->self_uses > 0u && !fields[j].recursive && !fields[j].path)
        return FAIL(c, "DOMAIN_FAMILY", "the field %s must be a direct recursive field or a path between earlier recursive fields", fields[j].name);
    }
    /* A later field type can use this field (S4 item 13). */
    if (!interval && !push(c, fields[j].name, here(c, fields[j].type)))
      return 0;
    points += !interval;
  }
  if (!ctor_result(c, fam, ctor, (uint32_t)points, &result))
    return 0;
  c->scope = scope;
  c->env = env;
  c->level = level;
  if (ctor->field_count - points > 2u)
    return FAIL(c, "HIT_BOUNDARY", "%s has %u interval fields; a path constructor has 1 or 2", ctor->name, (unsigned)(ctor->field_count - points));
  if (points < ctor->field_count && ctor->boundary == NULL)
    return FAIL(c, "HIT_BOUNDARY", "the path constructor %s needs a boundary [ ~i -> t0, i -> t1 ] after its interval fields", ctor->name);
  /* A boundary with no interval field (a point constructor or a state)
     gives DOMAIN_FAMILY, as the USER guard of cdfc751 did. */
  if (points == ctor->field_count && ctor->boundary != NULL)
    return FAIL(c, "DOMAIN_FAMILY", "%s has a boundary but no interval field; only a path constructor has a boundary", ctor->name);
  if (ctor->boundary != NULL) {
    boundary = check_boundary(c, fam, ctor, fields, points, result);
    if (boundary == NULL)
      return 0;
  }
  ci->name = ctor->name;
  ci->family = fam;
  ci->fields = fields;
  ci->field_count = (uint32_t)points;
  ci->event = 0;
  ci->interval_count = (uint32_t)(ctor->field_count - points);
  ci->boundary = boundary;
  ci->result = result;
  return 1;
}

/* An event (slice K4a1): a constructor of Out with fields of the ABI word
   types Nat, Flag, U256 and Addr. The field names are local: they are not
   projections and not core names. */
static int check_event(Checker *c, uint32_t fam, const Decl *d) {
  Machine *m = c->m;
  const Ctor *ctor = &d->ctors[0];
  FieldInfo *fields = arena_alloc(m->arena, (ctor->field_count + 1u) * sizeof *fields);
  CtorInfo *ci = &m->ctors[m->ctor_count];
  size_t j;
  m->def = d->name;
  if (fields == NULL)
    return FAIL(c, "OOM", "out of memory");
  if (!name_free(c, d->name))
    return 0;
  if (ctor->field_count > ENTRY_PARAMS_MAX)
    return FAIL(c, "REFUSE_EVENT", "the event %s has %u fields, the limit is %u", d->name, (unsigned)ctor->field_count, ENTRY_PARAMS_MAX);
  for (j = 0; j < ctor->field_count; j++) {
    uint64_t level = 0;
    const Core *t = check_type(c, ctor->fields[j].type, &level);
    const Value *v = t == NULL ? NULL : here(c, t);
    if (v == NULL)
      return 0;
    if (!val_is(v, OP_NAT) && !val_is(v, OP_FLAG) && !val_is(v, OP_U256) && !val_is(v, OP_ADDR))
      return FAIL(c, "REFUSE_EVENT", "the event field %s must have an ABI word type: Nat, Flag, U256 or Addr, found %s", ctor->fields[j].name, show(c, v));
    for (size_t k = 0; k < j; k++)
      if (strcmp(ctor->fields[j].name, ctor->fields[k].name) == 0)
        return FAIL(c, "REFUSE_EVENT", "%s is already a field of the event %s", ctor->fields[j].name, d->name);
    fields[j].name = ctor->fields[j].name;
    fields[j].type = t;
    fields[j].recursive = 0;
    fields[j].dependent = 0;
    fields[j].over_recursive = 0;
    fields[j].path = 0;
    fields[j].ends[0] = 0u;
    fields[j].ends[1] = 0u;
    fields[j].history = 0;
  }
  ci->name = d->name;
  ci->family = fam;
  ci->fields = fields;
  ci->field_count = (uint32_t)ctor->field_count;
  ci->event = 1;
  ci->interval_count = 0u;
  ci->boundary = NULL;
  ci->result = NULL;
  m->ctor_count++;
  m->families[fam].ctor_count++;
  return 1;
}

/* The events of the program join the family Out of the contract prelude, in
   the order of the program (slice K4a1). Thus a definition can use an event
   that the program declares after it. */
static int check_events(Checker *c, uint32_t fam) {
  const char *def = c->m->def;
  size_t i;
  for (i = 0; i < c->decls->count; i++) {
    if (c->decls->items[i].is_event && !check_event(c, fam, &c->decls->items[i]))
      return 0;
  }
  c->m->def = def;
  return 1;
}

static int kernel_sig(Checker *c, const Decl *d);

/* T has a free variable NAME. A local binder can shadow a block sort. */
static int names_var(const Term *t, const char *name) {
  if (t == NULL)
    return 0;
  if (t->kind == TERM_VAR && t->name != NULL && strcmp(t->name, name) == 0)
    return 1;
  if (names_var(t->left, name))
    return 1;
  if ((t->kind == TERM_PI || t->kind == TERM_LAM || t->kind == TERM_SIGMA) && t->name != NULL && strcmp(t->name, name) == 0)
    return 0;
  return names_var(t->right, name);
}

/* The header of the sort S of the block FIRST of COUNT sorts names no
   sort from S on (S6): its parameters and its index telescope can name
   only the earlier sorts (BLOCK_ORDER). */
static int header_order(Checker *c, const Decl *first, uint32_t s, uint32_t count) {
  const Decl *d = &first[s];
  uint32_t t;
  size_t i;
  for (t = s; t < count; t++) {
    int named = 0;
    for (i = 0; i < d->param_count; i++) {
      named |= names_var(d->params[i].type, first[t].name);
      if (strcmp(d->params[i].name, first[t].name) == 0)
        break;
    }
    if (i == d->param_count)
      named |= names_var(d->index, first[t].name);
    if (named)
      return FAIL(c, "BLOCK_ORDER", "the header of %s names the block sort %s; a header can name only the earlier sorts of its block", d->name, first[t].name);
  }
  return 1;
}

/* The header of the block sort D as the family FAM of the block FIRST of
   COUNT sorts (S6): the parameters, then the index telescope `T1 -> ...
   -> Type 0`, in an empty context. Prop and SProp are not types there
   (NAME_UNKNOWN, rule R4). The constructor slots of D are reserved here,
   so the constructors of a sort stay contiguous. */
static int check_header(Checker *c, const Decl *d, uint32_t fam, uint32_t first, uint32_t count, uint64_t universe) {
  Machine *m = c->m;
  FamilyInfo *f = &m->families[fam];
  const Core **params = arena_alloc(m->arena, (d->param_count + 1u) * sizeof *params);
  const Core **indices;
  const Term *t;
  uint32_t n = 0;
  size_t i;
  for (t = d->index; t != NULL && t->kind == TERM_PI; t = t->right)
    n++;
  indices = arena_alloc(m->arena, ((size_t)n + 1u) * sizeof *indices);
  if (params == NULL || indices == NULL)
    return FAIL(c, "OOM", "out of memory");
  c->scope = NULL;
  c->env = NULL;
  c->level = 0;
  for (i = 0; i < d->param_count; i++) {
    uint64_t level = 0;
    params[i] = check_type(c, d->params[i].type, &level);
    /* Expose universe and higher-kind aliases before the cell check. */
    if (params[i] != NULL)
      params[i] = quote_value(m, c->level, here(c, params[i]));
    if (params[i] == NULL || !push(c, d->params[i].name, here(c, params[i])))
      return 0;
  }
  for (t = d->index, i = 0; t != NULL && t->kind == TERM_PI; t = t->right, i++) {
    uint64_t level = 0;
    indices[i] = check_type(c, t->left, &level);
    if (indices[i] != NULL)
      indices[i] = quote_value(m, c->level, here(c, indices[i]));
    if (indices[i] == NULL || !push(c, t->name == NULL ? "_" : t->name, here(c, indices[i])))
      return 0;
  }
  if (t != NULL && (t->kind != TERM_UNIV || t->nat != 0u))
    return FAIL(c, "TYPE_MISMATCH", "the index telescope of %s must end in Type 0", d->name);
  f->name = d->name;
  f->param_types = params;
  f->param_count = (uint32_t)d->param_count;
  f->index_types = indices;
  f->index_count = n;
  f->block_first = first;
  f->block_count = count;
  f->first_ctor = m->ctor_count;
  f->ctor_count = (uint32_t)d->ctor_count;
  f->universe = universe;
  f->origin = d->origin;
  for (i = 0; i < d->ctor_count; i++) {
    memset(&m->ctors[m->ctor_count + i], 0, sizeof m->ctors[0]);
    m->ctors[m->ctor_count + i].family = fam;
  }
  m->ctor_count += (uint32_t)d->ctor_count;
  m->family_count++;
  return 1;
}

/* A block of sorts D[0 .. block_count) (S6); a family outside a block is a
   block of one. All headers bind first, then the point constructors of
   all sorts, then the path constructors of all sorts, so a boundary can
   name a point constructor of a later sort. The sorts of the block are in
   scope in the fields, so a field can be recursive. */
static int check_family(Checker *c, const Decl *d) {
  Machine *m = c->m;
  uint32_t first = m->family_count;
  uint32_t count = d->block_count > 0u ? (uint32_t)d->block_count : 1u;
  uint32_t first_ctor = m->ctor_count;
  uint64_t *universes = arena_alloc(m->arena, (size_t)count * sizeof *universes);
  uint32_t s;
  int pass;
  if (universes == NULL)
    return FAIL(c, "OOM", "out of memory");
  memset(universes, 0, (size_t)count * sizeof *universes);
  c->self = first;
  c->self_count = count;
  /* A later sort can raise the universe of an earlier sort. Recheck
     headers and constructors until these monotone universe bounds settle,
     so no core retains an elaboration made at a provisional lower level. */
  for (;;) {
    int changed = 0;
    m->family_count = first;
    m->ctor_count = first_ctor;
    for (s = 0; s < count; s++) {
      m->def = d[s].name;
      if (s > 0u && (!kernel_sig(c, &d[s]) || !name_free(c, d[s].name)))
        return 0;
      if (!header_order(c, d, s, count) || !check_header(c, &d[s], first + s, first, count, universes[s]))
        return 0;
    }
    if (!cell_header(c, d, first, count))
      return 0;
    for (pass = 0; pass < 2; pass++) {
      for (s = 0; s < count; s++) {
        const FamilyInfo *f = &m->families[first + s];
        size_t i;
        m->def = d[s].name;
        c->scope = NULL;
        c->env = NULL;
        c->level = 0;
        for (i = 0; i < d[s].param_count; i++)
          if (!push(c, d[s].params[i].name, here(c, f->param_types[i])))
            return 0;
        for (i = 0; i < d[s].ctor_count; i++)
          if ((d[s].ctors[i].boundary != NULL) == (pass == 1) && !check_ctor(c, first + s, &d[s].ctors[i], f->first_ctor + (uint32_t)i))
            return 0;
      }
    }
    for (s = 0; s < count; s++) {
      changed |= universes[s] != m->families[first + s].universe;
      universes[s] = m->families[first + s].universe;
    }
    if (!changed)
      break;
  }
  if (count == 1u && has_type_param(&m->families[first]) && !cell_generators(c, d, first))
    return 0;
  /* A sealed family is a cell monad (S5, rule O7). */
  if (d->is_sealed && (count != 1u || !has_type_param(&m->families[first])))
    return FAIL(c, "REFUSE_CELL", "%s is sealed but is not a cell monad: a sealed family has a type parameter and no and block (rule O7)", d->name);
  for (s = 0; s < count; s++)
    m->families[first + s].sealed = d[s].is_sealed;
  return d->origin != ORIGIN_PROGRAM && count == 1u && strcmp(d->name, "Out") == 0 ? check_events(c, first) : 1;
}

/* Rule D-9 (slice K3c). The argument A of the history field K of the state
   constructor CTOR is `F t` or `ADD (F t) e`, with F the projection of the
   field. ADD is natAdd or u256Add. The type of the field selects one of
   the two, so this function does not compare them. */
static int history_arg_ok(uint32_t ctor, uint32_t k, const Core *a) {
  const Core *x = a->kind == CORE_OP && (a->op == OP_U256_ADD || a->op == OP_NAT_ADD) && a->argc == 2u ? a->args[0] : a;
  return x->kind == CORE_OP && x->op == OP_PROJ && x->inst == ctor && x->field == k;
}

static int is_family(const Value *t, uint32_t family);

/* Rule D-9 (USER ruling A). The rule does not check the body of init or of
   a definition with the type State after normalization. */
static int history_root(const Machine *m, uint32_t j) {
  return strcmp(m->defs[j].name, "init") == 0 || is_family(m->defs[j].type, m->state_family);
}

static int has_history(const Machine *m) {
  const CtorInfo *ci;
  uint32_t i;
  if (!m->has_state)
    return 0;
  ci = &m->ctors[m->families[m->state_family].first_ctor];
  for (i = 0; i < ci->field_count; i++) {
    if (ci->fields[i].history)
      return 1;
  }
  return 0;
}

/* 1 if the family State occurs in the type value V (USER ruling A). The
   codomain of a Pi or a Sigma opens at a fresh level, as conv_values does.
   A variable can stand for State, including the payload type of an
   existential package. A Lam, a stuck operation or a value that does not
   open also gives 1. */
static int mentions_state(Machine *m, uint32_t level, const Value *v) {
  uint32_t i;
  if (v == NULL)
    return 1;
  switch (v->kind) {
  case VAL_NAT:
  case VAL_WORD:
  case VAL_TRAP:
  case VAL_UNIV:
    return 0;
  case VAL_PI:
  case VAL_SIGMA:
    return mentions_state(m, level, v->dom) || mentions_state(m, level + 1u, closure_apply(m, v, val_var(m, level)));
  case VAL_APP:
  case VAL_PAPP:
    return mentions_state(m, level, v->dom) || mentions_state(m, level, v->arg);
  case VAL_PLAM:
    return mentions_state(m, level + 1u, closure_apply(m, v, val_ivar(m, level)));
  case VAL_PATHP:
    return mentions_state(m, level, v->args[0]) || mentions_state(m, level, v->args[1]) || mentions_state(m, level + 1u, closure_apply(m, v, val_ivar(m, level)));
  case VAL_IVAL:
    return 0;
  case VAL_LAM:
  case VAL_STUCK:
  case VAL_VAR:
    return 1;
  case VAL_OP:
    break;
  }
  if (is_family(v, m->state_family))
    return 1;
  for (i = 0; i < v->argc; i++) {
    if (mentions_state(m, level, v->args[i]))
      return 1;
  }
  return 0;
}

/* Rule D-9 (slice K3c) on the core term T of a definition. The names are
   resolved, so a binder with the name of a field is a local and not the
   field. Each use of the state constructor has all of its arguments and
   keeps or adds to each history field. TYPED is 1 when State occurs in the
   type of the definition: then T cannot refer to a definition that the rule
   does not check (history_root). */
static int history_ok(Checker *c, const Core *t, int typed) {
  const Machine *m = c->m;
  uint32_t i;
  if (t == NULL || !m->has_state)
    return 1;
  switch (t->kind) {
  case CORE_VAR:
  case CORE_NAT:
  case CORE_WORD:
  case CORE_TRAP:
  case CORE_UNIV:
  case CORE_IZERO:
  case CORE_IONE:
    return 1;
  case CORE_GLOBAL:
    if (typed && history_root(m, t->index))
      return FAIL(c, "REFUSE_HISTORY_WRITE", "a definition with State in its type cannot refer to %s, because the history rule does not check %s (rule D-9)", m->defs[t->index].name, m->defs[t->index].name);
    return 1;
  case CORE_LAM:
  case CORE_PI:
  case CORE_SIGMA:
  case CORE_APP:
  case CORE_PLAM:
  case CORE_INEG:
  case CORE_IMEET:
  case CORE_IJOIN:
    return history_ok(c, t->a, typed) && history_ok(c, t->b, typed);
  case CORE_PATHP:
  case CORE_PAPP:
    return history_ok(c, t->a, typed) && history_ok(c, t->b, typed) && history_ok(c, t->args[0], typed) && history_ok(c, t->args[1], typed);
  case CORE_OP:
    break;
  }
  if (t->op == OP_CTOR && m->ctors[t->inst].family == m->state_family) {
    const CtorInfo *ci = &m->ctors[t->inst];
    if (t->argc < ci->field_count)
      return FAIL(c, "REFUSE_HISTORY_WRITE", "%s must have all of its arguments (rule D-9)", ci->name);
    for (i = 0; i < ci->field_count; i++) {
      if (ci->fields[i].history && !history_arg_ok(t->inst, i, t->args[t->argc - ci->field_count + i]))
        return FAIL(c, "REFUSE_HISTORY_WRITE", "the history field %s must be (%s t) or ADD (%s t) e, with ADD u256Add or natAdd (rule D-9)", ci->fields[i].name, ci->fields[i].name, ci->fields[i].name);
    }
  }
  for (i = 0; i < t->argc; i++) {
    if (!history_ok(c, t->args[i], typed))
      return 0;
  }
  return 1;
}

/* The design signature (orbit-lang S3; PLAN section 3, O4): 11 types and 9
   operations. A slot is a program definition of the slot type. A defined or
   a primitive name is reserved until a later slice gives it (S4, S7, S8). */
typedef enum {
  SIG_TYPE,
  SIG_OP
} SigSort;

typedef enum {
  SIG_SLOT,
  SIG_DEFINED,
  SIG_PRIMITIVE
} SigKind;

typedef struct {
  const char *name;
  SigSort sort;
  SigKind kind;
} SigRow;

static const SigRow SIGNATURE[] = {
  {"Member", SIG_TYPE, SIG_SLOT},
  {"Choice", SIG_TYPE, SIG_SLOT},
  {"Outcome", SIG_TYPE, SIG_SLOT},
  {"Group", SIG_TYPE, SIG_SLOT},
  {"Config", SIG_TYPE, SIG_DEFINED},
  {"Orbit", SIG_TYPE, SIG_DEFINED},
  {"Constitution", SIG_TYPE, SIG_DEFINED},
  {"Aggregation", SIG_TYPE, SIG_DEFINED},
  {"ZKWitness", SIG_TYPE, SIG_PRIMITIVE},
  {"Tally", SIG_TYPE, SIG_DEFINED},
  {"Ballot_priv", SIG_TYPE, SIG_PRIMITIVE},
  {"orbitProj", SIG_OP, SIG_DEFINED},
  {"act", SIG_OP, SIG_SLOT},
  {"outcomeSet", SIG_OP, SIG_SLOT},
  {"menu", SIG_OP, SIG_SLOT},
  {"aggregate", SIG_OP, SIG_DEFINED},
  {"gov", SIG_OP, SIG_DEFINED},
  {"amend", SIG_OP, SIG_DEFINED},
  {"castPrivate", SIG_OP, SIG_PRIMITIVE},
  {"proveZK", SIG_OP, SIG_PRIMITIVE},
  {"verifyZK", SIG_OP, SIG_PRIMITIVE},
  {"project", SIG_OP, SIG_PRIMITIVE}
};

#define SIG_COUNT ((uint32_t)(sizeof SIGNATURE / sizeof SIGNATURE[0]))

/* The slot type of act: Group -> Config -> Config, Config = Member -> Choice. */
static const Term SIG_MEMBER = {TERM_VAR, 0, 0, "Member", 0, NULL, NULL, NULL};
static const Term SIG_CHOICE = {TERM_VAR, 0, 0, "Choice", 0, NULL, NULL, NULL};
static const Term SIG_GROUP = {TERM_VAR, 0, 0, "Group", 0, NULL, NULL, NULL};
static const Term SIG_CONFIG = {TERM_PI, 0, 0, NULL, 0, NULL, &SIG_MEMBER, &SIG_CHOICE};
static const Term SIG_STEP = {TERM_PI, 0, 0, NULL, 0, NULL, &SIG_CONFIG, &SIG_CONFIG};
static const Term SIG_ACT = {TERM_PI, 0, 0, NULL, 0, NULL, &SIG_GROUP, &SIG_STEP};

/* The slot types of outcomeSet (USER ruling S7 Q2) and menu (Q3). */
static const Term SIG_OUTCOME = {TERM_VAR, 0, 0, "Outcome", 0, NULL, NULL, NULL};
static const Term SIG_IS_SET = {TERM_VAR, 0, 0, "isSet", 0, NULL, NULL, NULL};
static const Term SIG_OUTCOME_SET = {TERM_APP, 0, 0, NULL, 0, NULL, &SIG_IS_SET, &SIG_OUTCOME};
static const Term SIG_CONSTITUTION = {TERM_VAR, 0, 0, "Constitution", 0, NULL, NULL, NULL};
static const Term SIG_MENU = {TERM_PI, 0, 0, NULL, 0, NULL, &SIG_OUTCOME, &SIG_CONSTITUTION};

/* An op slot: its slot type, the program slots and the kernel name that the
   type uses (a slot is checked only after them, so the check of its type
   meets no unknown name), and the type as the refusal shows it. */
typedef struct {
  const char *name;
  const Term *type;
  const char *program[3];
  const char *kernel;
  const char *shown;
} OpSlot;

static const OpSlot OP_SLOTS[] = {
  {"act", &SIG_ACT, {"Member", "Choice", "Group"}, NULL, "Group -> (Member -> Choice) -> Member -> Choice, after Member, Choice and Group"},
  {"outcomeSet", &SIG_OUTCOME_SET, {"Outcome", NULL, NULL}, NULL, "isSet Outcome, after Outcome"},
  {"menu", &SIG_MENU, {"Outcome", NULL, NULL}, "Constitution", "Outcome -> Constitution, after Outcome and the DAO prelude"}
};

#define OP_SLOT_COUNT ((uint32_t)(sizeof OP_SLOTS / sizeof OP_SLOTS[0]))

static uint32_t sig_row(const char *name) {
  uint32_t i;
  for (i = 0; i < SIG_COUNT && strcmp(SIGNATURE[i].name, name) != 0; i++)
    ;
  return i;
}

static const OpSlot *op_slot(const char *name) {
  uint32_t i;
  for (i = 0; i < OP_SLOT_COUNT && strcmp(OP_SLOTS[i].name, name) != 0; i++)
    ;
  return i == OP_SLOT_COUNT ? NULL : &OP_SLOTS[i];
}

static const char *slot_shown(const SigRow *row) {
  const OpSlot *s;
  switch (row->sort) {
  case SIG_TYPE:
    return "Type 0";
  case SIG_OP:
    break;
  }
  s = op_slot(row->name);
  return s == NULL ? "an op slot type" : s->shown;
}

static int program_def(const Machine *m, const char *name) {
  uint32_t i = find_def(m, name);
  return i != NO_DEF && m->defs[i].origin == ORIGIN_PROGRAM;
}

/* TV is the slot type of ROW: Type 0 for a type slot; for an op slot, the
   type of OP_SLOTS after the program slots and the kernel name it uses. */
static int slot_ok(Checker *c, const SigRow *row, const Value *tv) {
  uint64_t level = 0;
  const OpSlot *s;
  const Core *t;
  uint32_t i;
  switch (row->sort) {
  case SIG_TYPE:
    return conv_values(c->m, c->level, tv, val_univ(c->m, 0u));
  case SIG_OP:
    break;
  }
  s = op_slot(row->name);
  if (s == NULL)
    return 0;
  for (i = 0; i < 3u && s->program[i] != NULL; i++) {
    if (!program_def(c->m, s->program[i]))
      return 0;
  }
  if (s->kernel != NULL && find_def(c->m, s->kernel) == NO_DEF)
    return 0;
  t = check_type(c, s->type, &level);
  return t != NULL && conv_values(c->m, c->level, tv, here(c, t));
}

static int check_sig(Checker *c, const Decl *d, const Value *tv) {
  uint32_t i = sig_row(d->name);
  if (d->origin != ORIGIN_PROGRAM || i == SIG_COUNT)
    return 1;
  switch (SIGNATURE[i].kind) {
  case SIG_SLOT:
    return slot_ok(c, &SIGNATURE[i], tv) ? 1 : FAIL(c, "REFUSE_SIG", "the slot %s must have the type %s", d->name, slot_shown(&SIGNATURE[i]));
  case SIG_DEFINED:
  case SIG_PRIMITIVE:
    break;
  }
  return FAIL(c, "REFUSE_SIG", "%s is a %s name of the design signature (PLAN section 3), reserved until a later slice gives it", d->name, SIGNATURE[i].kind == SIG_DEFINED ? "defined" : "primitive");
}

/* The kernel gives each defined name of the design signature: Orbit and
   orbitProj after act (orbit-lang S4), the DAO prelude after the last of
   act and outcomeSet (S7, PLAN section 8), and each primitive name in the
   ZK splice after Member, Choice and the DAO prelude (S8 B2b). A program
   declaration of a defined or primitive name is REFUSE_SIG, before the
   name check, so the code is the same before and after the kernel gives
   it. */
static int kernel_sig(Checker *c, const Decl *d) {
  uint32_t i = sig_row(d->name);
  if (d->origin != ORIGIN_PROGRAM || i == SIG_COUNT)
    return 1;
  switch (SIGNATURE[i].kind) {
  case SIG_DEFINED:
    return FAIL(c, "REFUSE_SIG", "%s is a defined name of the design signature (PLAN section 3); the kernel gives it (Orbit and orbitProj after act, the DAO prelude after act and outcomeSet)", d->name);
  case SIG_PRIMITIVE:
    return FAIL(c, "REFUSE_SIG", "%s is a primitive name of the design signature (PLAN section 3); the kernel gives it (the ZK splice after Member, Choice and the DAO prelude)", d->name);
  case SIG_SLOT:
    break;
  }
  return 1;
}

static uint32_t find_family(const Machine *m, const char *name);

/* Rule O7 (S5, Q-S5-2 (a)). 1 if the core term T uses a law-only name: a
   law-only def, or an elim, a fold or a projection of a sealed family. The
   arguments of the type former TRUNC (Trunc1) are skipped: an end point
   under Trunc1 comes out only into a proposition. The argument of the
   domain def ZK (ZKProof := Trunc1, S7, Q-S7-1 (a)) is skipped too: the
   syntax type is walked before normalization, so ZKProof W is still an
   application of the def there. */
static int law_uses(const Machine *m, uint32_t trunc, uint32_t zk, const Core *t) {
  uint32_t i;
  if (t == NULL)
    return 0;
  if (t->kind == CORE_APP && t->a != NULL && t->a->kind == CORE_GLOBAL && t->a->index == zk)
    return 0;
  switch (t->kind) {
  case CORE_VAR:
  case CORE_NAT:
  case CORE_WORD:
  case CORE_TRAP:
  case CORE_UNIV:
  case CORE_IZERO:
  case CORE_IONE:
    return 0;
  case CORE_GLOBAL:
    return m->defs[t->index].law_only;
  case CORE_LAM:
  case CORE_PI:
  case CORE_SIGMA:
  case CORE_APP:
  case CORE_PLAM:
  case CORE_INEG:
  case CORE_IMEET:
  case CORE_IJOIN:
    return law_uses(m, trunc, zk, t->a) || law_uses(m, trunc, zk, t->b);
  case CORE_PATHP:
  case CORE_PAPP:
    return law_uses(m, trunc, zk, t->a) || law_uses(m, trunc, zk, t->b) || law_uses(m, trunc, zk, t->args[0]) || law_uses(m, trunc, zk, t->args[1]);
  case CORE_OP:
    break;
  }
  if (t->op == OP_FAMILY && t->inst == trunc)
    return 0;
  /* OP_ELIM keeps the family + 1 in FIELD (0 for Sum and Unit). */
  if ((t->op == OP_FOLD_FAMILY && m->families[t->inst].sealed) || (t->op == OP_ELIM && t->field != 0u && m->families[t->field - 1u].sealed) || (t->op == OP_PROJ && m->families[m->ctors[t->inst].family].sealed))
    return 1;
  for (i = 0; i < t->argc; i++) {
    if (law_uses(m, trunc, zk, t->args[i]))
      return 1;
  }
  return 0;
}

/* Rule O7: a law type is, after the Pis (the loop of entry_of), a Path type
   or Trunc1 X. */
static int law_type(Machine *m, uint32_t level, uint32_t trunc, const Value *t) {
  uint32_t n = level;
  while (t != NULL && t->kind == VAL_PI)
    t = closure_apply(m, t, val_var(m, n++));
  return t != NULL && (t->kind == VAL_PATHP || is_family(t, trunc));
}

/* Register a checked body, including the closed label cores retained by
   cocell_shape. Both callers obtain BODY from the term checker. */
static int check_def_body(Checker *c, const Decl *d, const Value *tv, const Core *syntax_type, const Core *body) {
  Machine *m = c->m;
  DefInfo *info = &m->defs[m->def_count];
  uint32_t trunc = find_family(m, "Trunc1");
  uint32_t zk = find_def(m, "ZKProof");
  int program = d->origin == ORIGIN_PROGRAM;
  const Core *tc;
  int law;
  if (body == NULL || !check_sig(c, d, tv))
    return 0;
  if (has_history(m) && strcmp(d->name, "init") != 0 && !is_family(tv, m->state_family) && !history_ok(c, body, mentions_state(m, c->level, tv)))
    return 0;
  /* Rule O7 (S5): the domain marks its law defs; a program def is law-only
     when its type or its body uses a law-only name. Kit mode sets the flag
     but does not refuse. */
  tc = program ? quote_value(m, c->level, tv) : NULL;
  if (program && tc == NULL)
    return FAIL(c, "OOM", "out of memory");
  law = program ? law_uses(m, trunc, zk, syntax_type) || law_uses(m, trunc, zk, tc) || law_uses(m, trunc, zk, body) : d->is_law;
  if (law && guest(c) && !law_type(m, c->level, trunc, tv))
    return FAIL(c, "REFUSE_LAW", "%s uses a law-only name (decrypt, observe, an elim, fold or projection of a sealed family) but its type is not a law (a Path or Trunc1 codomain); rule O7", d->name);
  info->name = d->name;
  info->origin = d->origin;
  info->body = body;
  info->type = tv;
  info->value = NULL;
  info->law_only = law;
  m->def_count++;
  return 1;
}

static int check_def(Checker *c, const Decl *d) {
  uint64_t level = 0;
  const Core *t = check_type(c, d->type, &level);
  const Value *tv = here(c, t);
  const Core *body = check(c, d->body, tv);
  return check_def_body(c, d, tv, t, body);
}

/* First order (slice K3a): Nat, Flag, U256, Addr, and Option, Prod and List
   of first-order types, after normalization. */
static int first_order(const Value *t) {
  uint32_t i;
  if (t == NULL || t->kind != VAL_OP)
    return 0;
  if (t->op == OP_NAT || t->op == OP_FLAG || t->op == OP_U256 || t->op == OP_ADDR)
    return 1;
  if (t->op != OP_OPTION && t->op != OP_PROD && t->op != OP_LIST)
    return 0;
  for (i = 0; i < t->argc; i++) {
    if (!first_order(t->args[i]))
      return 0;
  }
  return 1;
}

/* The state form (slice K3a): one `state State` with no parameters, one
   constructor and first-order fields. A history field is a Nat or a U256. */
static int check_state(Checker *c, const Decl *d) {
  Machine *m = c->m;
  CtorInfo *ci;
  FieldInfo *fields;
  const Env *env = c->env;
  uint32_t j;
  if (m->has_state)
    return FAIL(c, "REFUSE_STATE", "a program can declare one state only");
  if (strcmp(d->name, "State") != 0)
    return FAIL(c, "REFUSE_STATE", "the state type must have the name State, found %s", d->name);
  if (d->param_count != 0u || d->ctor_count != 1u || d->index != NULL)
    return FAIL(c, "REFUSE_STATE", "the state must have no parameters or index telescope and one constructor");
  if (!name_free(c, d->name) || !check_family(c, d))
    return 0;
  ci = &m->ctors[m->families[m->family_count - 1u].first_ctor];
  fields = arena_alloc(m->arena, ((size_t)ci->field_count + 1u) * sizeof *fields);
  if (fields == NULL)
    return FAIL(c, "OOM", "out of memory");
  for (j = 0; j < ci->field_count; j++) {
    const Value *t = eval_core(m, env, ci->fields[j].type);
    if (t == NULL)
      return 0;
    if (!first_order(t) && !val_is(t, OP_KMAP))
      return FAIL(c, "REFUSE_STATE", "the state field %s must have a first-order type: Nat, Flag, U256, Addr, Option, Prod or List of them, or a Map K V", ci->fields[j].name);
    if (ci->fields[j].history && t->op != OP_NAT && t->op != OP_U256)
      return FAIL(c, "REFUSE_STATE", "the history field %s must have the type Nat or U256", ci->fields[j].name);
    /* First-order state types are closed after normalization. Keep that
       form for lowering, which reads them without a field environment. */
    fields[j] = ci->fields[j];
    fields[j].type = quote_value(m, c->level, t);
    fields[j].dependent = 0;
    env = env_push(m, env, val_var(m, c->level + j));
    if (fields[j].type == NULL || env == NULL)
      return 0;
  }
  ci->fields = fields;
  m->has_state = 1;
  m->state_family = m->family_count - 1u;
  return 1;
}

/* The fields of a cocell signature (S4, rule R1): the one observation `A`,
   a cofield `D A`, or a labelled cofield `E -> D A` with E closed. */
typedef enum {
  COCELL_OBSERVATION,
  COCELL_COFIELD,
  COCELL_LABELLED
} CocellField;

/* T is the application of the variable of index F to the variable of
   index A. */
static int cocell_app(const Core *t, uint32_t f, uint32_t a) {
  return t->kind == CORE_APP && t->a->kind == CORE_VAR && t->a->index == f && t->b->kind == CORE_VAR && t->b->index == a;
}

/* The kind of FIELD J with the normal type T, in the context D, A, then the
   J earlier fields: A is the variable J and D is the variable J + 1. A
   labelled cofield is an arrow written in the field. */
static int cocell_kind(const Field *field, const Core *t, uint32_t j, CocellField *out) {
  if (t->kind == CORE_VAR && t->index == j) {
    *out = COCELL_OBSERVATION;
    return 1;
  }
  if (cocell_app(t, j + 1u, j)) {
    *out = COCELL_COFIELD;
    return 1;
  }
  if (field->type->kind == TERM_PI && t->kind == CORE_PI && cell_closed(t->a, 0u) && cocell_app(t->b, j + 2u, j + 1u)) {
    *out = COCELL_LABELLED;
    return 1;
  }
  return 0;
}

/* The shape check of `cofamily D (A : Type n) := c fields` (S4, rule R1):
   one declaration, one type parameter, no index, one clause, and each
   field a kind of CocellField, with exactly one observation. The fields
   are checked with D : Type n -> Type n and A as locals. The universe of
   the parameter goes to *N, the field kinds to KINDS and the universe of
   each label type to LABELS, with its closed normal form in LABEL_TYPES. */
static int cocell_shape(Checker *c, const Decl *d, uint64_t *n, CocellField *kinds, uint64_t *labels, const Core **label_types) {
  Machine *m = c->m;
  const Ctor *ctor = d->ctors;
  Term *arrow = arena_alloc(m->arena, sizeof *arrow);
  const Core *pt, *pn, *dt;
  uint64_t level = 0;
  uint32_t j, observations = 0;
  if (arrow == NULL)
    return FAIL(c, "OOM", "out of memory");
  if (d->block_count > 1u)
    return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: it is in an and block; a type constructor is one signature (rule R1)", d->name);
  if (d->param_count != 1u)
    return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: it has %u parameters; a cocell signature has one type parameter (rule R1)", d->name, (unsigned)d->param_count);
  if (d->index != NULL)
    return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: it has a type index (rule R1)", d->name);
  if (d->ctor_count != 1u || ctor->result != NULL || ctor->boundary != NULL)
    return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: it must have one clause, with no result and no boundary (rule R1)", d->name);
  pt = check_type(c, d->params[0].type, &level);
  if (pt == NULL)
    return 0;
  pn = quote_value(m, c->level, here(c, pt));
  if (pn == NULL)
    return FAIL(c, "OOM", "out of memory");
  if (pn->kind != CORE_UNIV)
    return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: the parameter %s is not a universe; a cocell signature has one type parameter (rule R1)", d->name, d->params[0].name);
  *n = pn->nat;
  memset(arrow, 0, sizeof *arrow);
  arrow->kind = TERM_PI;
  arrow->line = d->line;
  arrow->col = d->col;
  arrow->left = d->params[0].type;
  arrow->right = d->params[0].type;
  dt = check_type(c, arrow, &level);
  if (dt == NULL || !push(c, d->name, here(c, dt)) || !push(c, d->params[0].name, here(c, pt)))
    return 0;
  for (j = 0; j < ctor->field_count; j++) {
    const Field *field = &ctor->fields[j];
    const Core *k = check_type(c, field->type, &level);
    const Core *t = k == NULL ? NULL : quote_value(m, c->level, here(c, k));
    if (k == NULL)
      return 0;
    if (t == NULL)
      return FAIL(c, "OOM", "out of memory");
    if (!cocell_kind(field, t, j, &kinds[j]))
      return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: the field %s must be the observation %s, the cofield %s %s or a labelled cofield E -> %s %s with E closed (rule R1)",
                  d->name, field->name, d->params[0].name, d->name, d->params[0].name, d->name, d->params[0].name);
    labels[j] = 0;
    if (kinds[j] == COCELL_LABELLED && check_type(c, field->type->left, &labels[j]) == NULL)
      return 0;
    if (labels[j] > *n)
      return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: the label type of %s is in Type %llu, above Type %llu of %s (rule R1)",
                  d->name, field->name, (unsigned long long)labels[j], (unsigned long long)*n, d->params[0].name);
    /* cocell_kind proved this domain closed. Keep its checked normal form
       rather than resolving the original syntax in a different scope. */
    label_types[j] = kinds[j] == COCELL_LABELLED ? t->a : NULL;
    observations += kinds[j] == COCELL_OBSERVATION;
    if (!push(c, field->name, here(c, k)))
      return 0;
  }
  if (observations != 1u)
    return FAIL(c, "REFUSE_CELL", "%s is not a cocell signature: it has %u observations; a cocell signature has exactly one (rule R1)", d->name, (unsigned)observations);
  for (j = 0; j < ctor->field_count; j++)
    if (!name_free(c, ctor->fields[j].name))
      return 0;
  return name_free(c, ctor->name);
}

/* The text of the declarations of a cofamily (S4): a two-pass appender.
   With TEXT NULL it only measures; then it writes into CAP bytes. */
typedef struct {
  char *text;
  size_t len;
  size_t cap;
} Text;

static void text_add(Text *t, const char *format, ...) {
  va_list ap;
  int n;
  va_start(ap, format);
  n = vsnprintf(t->text == NULL ? NULL : t->text + t->len, t->text == NULL ? 0u : t->cap - t->len, format, ap);
  va_end(ap);
  t->len += n > 0 ? (size_t)n : 0u;
}

/* The position constructor of field F of the cofamily DN. */
static void cocell_pos(Text *t, const char *dn, const char *f, CocellField kind) {
  switch (kind) {
  case COCELL_OBSERVATION:
    break;
  case COCELL_COFIELD:
    text_add(t, " | %s.%s (%s.%s.rest : %s.pos)", dn, f, dn, f, dn);
    break;
  case COCELL_LABELLED:
    text_add(t, " | %s.%s (%s.%s.label : %s.%s.E) (%s.%s.rest : %s.pos)", dn, f, dn, f, dn, f, dn, f, dn);
    break;
  }
}

/* The destructor F: the observation reads the position here, a cofield
   moves one position down. */
static void cocell_dtor(Text *t, const char *dn, const char *f, CocellField kind, unsigned long long u) {
  switch (kind) {
  case COCELL_OBSERVATION:
    text_add(t, "def %s : (.A : Type %llu) -> %s .A -> .A := fun (.A : Type %llu) (.x : %s .A) => .x %s.here\n", f, u, dn, u, dn, dn);
    break;
  case COCELL_COFIELD:
    text_add(t, "def %s : (.A : Type %llu) -> %s .A -> %s .A := fun (.A : Type %llu) (.x : %s .A) (.p : %s.pos) => .x (%s.%s .p)\n",
             f, u, dn, dn, u, dn, dn, dn, f);
    break;
  case COCELL_LABELLED:
    text_add(t, "def %s : (.A : Type %llu) -> %s .A -> %s.%s.E -> %s .A := fun (.A : Type %llu) (.x : %s .A) (.e : %s.%s.E) (.p : %s.pos) => .x (%s.%s .e .p)\n",
             f, u, dn, dn, f, dn, u, dn, dn, f, dn, dn, f);
    break;
  }
}

/* The step binder of cofield F for the walk and the corecursor, then
   SUFFIX. */
static void cocell_step(Text *t, const char *dn, const char *f, CocellField kind, const char *suffix) {
  switch (kind) {
  case COCELL_OBSERVATION:
    break;
  case COCELL_COFIELD:
    text_add(t, " (.t.%s : .S -> .S)%s", f, suffix);
    break;
  case COCELL_LABELLED:
    text_add(t, " (.t.%s : .S -> %s.%s.E -> .S)%s", f, dn, f, suffix);
    break;
  }
}

/* The fold case of cofield F in the walk at the motive S -> S: the step
   first, then the rest of the walk. */
static void cocell_walk_case(Text *t, const char *dn, const char *f, CocellField kind) {
  switch (kind) {
  case COCELL_OBSERVATION:
    break;
  case COCELL_COFIELD:
    text_add(t, "\n    (fun (.ih : .S -> .S) (.s : .S) => .ih (.t.%s .s))", f);
    break;
  case COCELL_LABELLED:
    text_add(t, "\n    (fun (.e : %s.%s.E) (.ih : .S -> .S) (.s : .S) => .ih (.t.%s .s .e))", dn, f, f);
    break;
  }
}

static void cocell_walk_arg(Text *t, const char *f, CocellField kind) {
  switch (kind) {
  case COCELL_OBSERVATION:
    break;
  case COCELL_COFIELD:
  case COCELL_LABELLED:
    text_add(t, " .t.%s", f);
    break;
  }
}

/* The bisimulation motive at the position .q. */
static void cocell_motive(Text *t, const char *dn) {
  text_add(t, "(.u : %s .A) -> (.v : %s .A) -> .R .u .v -> Path .A (.u .q) (.v .q)", dn, dn);
}

/* The bisimulation hypothesis of field F, then SUFFIX. */
static void cocell_hyp(Text *t, const char *dn, const char *f, CocellField kind, const char *suffix) {
  text_add(t, "\n    (.h.%s : (.u : %s .A) -> (.v : %s .A) -> .R .u .v -> ", f, dn, dn);
  switch (kind) {
  case COCELL_OBSERVATION:
    text_add(t, "Path .A (%s .A .u) (%s .A .v))%s", f, f, suffix);
    break;
  case COCELL_COFIELD:
    text_add(t, ".R (%s .A .u) (%s .A .v))%s", f, f, suffix);
    break;
  case COCELL_LABELLED:
    text_add(t, "(.e : %s.%s.E) -> .R (%s .A .u .e) (%s .A .v .e))%s", dn, f, f, f, suffix);
    break;
  }
}

/* The elim case of cofield F in the bisimulation: the label, the rest,
   its induction hypothesis, then the two streams and the relation. */
static void cocell_bisim_case(Text *t, const char *dn, const char *f, CocellField kind) {
  switch (kind) {
  case COCELL_OBSERVATION:
    break;
  case COCELL_COFIELD:
    text_add(t, "\n    (fun (.q : %s.pos) (.ih : ", dn);
    cocell_motive(t, dn);
    text_add(t, ") (.u : %s .A) (.v : %s .A) (.w : .R .u .v) => .ih (%s .A .u) (%s .A .v) (.h.%s .u .v .w))", dn, dn, f, f, f);
    break;
  case COCELL_LABELLED:
    text_add(t, "\n    (fun (.e : %s.%s.E) (.q : %s.pos) (.ih : ", dn, f, dn);
    cocell_motive(t, dn);
    text_add(t, ") (.u : %s .A) (.v : %s .A) (.w : .R .u .v) => .ih (%s .A .u .e) (%s .A .v .e) (.h.%s .u .v .w .e))", dn, dn, f, f, f);
    break;
  }
}

/* The elaboration of `cofamily D (A : Type n) := c fields` (S4, choice
   (ii)): the positions D.pos, `D A := D.pos -> A`, one destructor per
   field, the walk, the corecursor c and the bisimulation principle
   DBisim. Generated binders start with `.`, so they never capture a user
   name. */
static void cocell_text(Text *t, const Decl *d, uint64_t n, const CocellField *kinds) {
  const Ctor *ctor = d->ctors;
  const char *dn = d->name;
  const char *obs = "";
  unsigned long long u = (unsigned long long)n;
  size_t j;
  for (j = 0; j < ctor->field_count; j++)
    obs = kinds[j] == COCELL_OBSERVATION ? ctor->fields[j].name : obs;
  text_add(t, "family %s.pos := %s.here", dn, dn);
  for (j = 0; j < ctor->field_count; j++)
    cocell_pos(t, dn, ctor->fields[j].name, kinds[j]);
  text_add(t, "\ndef %s : Type %llu -> Type %llu := fun (.A : Type %llu) => %s.pos -> .A\n", dn, u, u, u, dn);
  for (j = 0; j < ctor->field_count; j++)
    cocell_dtor(t, dn, ctor->fields[j].name, kinds[j], u);
  text_add(t, "def %s.walk : (.S : Type %llu) ->", dn, u);
  for (j = 0; j < ctor->field_count; j++)
    cocell_step(t, dn, ctor->fields[j].name, kinds[j], " ->");
  text_add(t, " %s.pos -> .S -> .S :=\n  fun (.S : Type %llu)", dn, u);
  for (j = 0; j < ctor->field_count; j++)
    cocell_step(t, dn, ctor->fields[j].name, kinds[j], "");
  text_add(t, " (.p : %s.pos) => fold (fun (.s : .S) => .s)", dn);
  for (j = 0; j < ctor->field_count; j++)
    cocell_walk_case(t, dn, ctor->fields[j].name, kinds[j]);
  text_add(t, " .p\ndef %s : (.A : Type %llu) -> (.S : Type %llu) -> (.o : .S -> .A) ->", ctor->name, u, u);
  for (j = 0; j < ctor->field_count; j++)
    cocell_step(t, dn, ctor->fields[j].name, kinds[j], " ->");
  text_add(t, " .S -> %s .A :=\n  fun (.A : Type %llu) (.S : Type %llu) (.o : .S -> .A)", dn, u, u);
  for (j = 0; j < ctor->field_count; j++)
    cocell_step(t, dn, ctor->fields[j].name, kinds[j], "");
  text_add(t, " (.seed : .S) (.p : %s.pos) => .o (%s.walk .S", dn, dn);
  for (j = 0; j < ctor->field_count; j++)
    cocell_walk_arg(t, ctor->fields[j].name, kinds[j]);
  text_add(t, " .p .seed)\ndef %sBisim : (.A : Type %llu) -> (.R : %s .A -> %s .A -> Type %llu) ->", dn, u, dn, dn, u);
  for (j = 0; j < ctor->field_count; j++)
    cocell_hyp(t, dn, ctor->fields[j].name, kinds[j], " ->");
  text_add(t, "\n    (.x : %s .A) -> (.y : %s .A) -> .R .x .y -> Path (%s .A) .x .y :=\n  fun (.A : Type %llu) (.R : %s .A -> %s .A -> Type %llu)", dn, dn, dn, u, dn, dn, u);
  for (j = 0; j < ctor->field_count; j++)
    cocell_hyp(t, dn, ctor->fields[j].name, kinds[j], "");
  text_add(t, "\n    (.x : %s .A) (.y : %s .A) (.r : .R .x .y) =>\n  funExt (fun (.p : %s.pos) => elim (fun (.q : %s.pos) => ", dn, dn, dn, dn);
  cocell_motive(t, dn);
  text_add(t, ") .h.%s", obs);
  for (j = 0; j < ctor->field_count; j++)
    cocell_bisim_case(t, dn, ctor->fields[j].name, kinds[j]);
  text_add(t, " .p .x .y .r)\n");
}

/* A fresh context for the declaration NAME at INDEX of the list. */
static void checker_reset(Checker *c, size_t index, const char *name) {
  c->decl_index = index;
  c->scope = NULL;
  c->env = NULL;
  c->level = 0;
  c->depth = 0;
  c->self = NO_FAMILY;
  c->self_count = 0u;
  c->fold_targets = NULL;
  c->elim_motive = NULL;
  c->m->def = name;
}

/* A declaration that a cofamily elaborates to: a fresh context, then rule
   R4 on its name (check_def does not check it). */
static int check_generated(Checker *c, const Decl *d) {
  checker_reset(c, c->decl_index, d->name);
  if (!name_free(c, d->name))
    return 0;
  return d->kind == DECL_FAMILY ? check_family(c, d) : check_def(c, d);
}

/* The label type E of the labelled cofield FIELD of D, at the universe K
   of the shape check: `def D.FIELD.E : Type K := E`. BODY is its checked,
   closed normal form, so erased local references cannot escape. */
static int check_label(Checker *c, const Decl *d, const Field *field, uint64_t k, const Core *body) {
  Arena *arena = c->m->arena;
  size_t size = strlen(d->name) + strlen(field->name) + 4u;
  char *name = arena_alloc(arena, size);
  Decl *e = arena_alloc(arena, sizeof *e);
  if (name == NULL || e == NULL)
    return FAIL(c, "OOM", "out of memory");
  snprintf(name, size, "%s.%s.E", d->name, field->name);
  memset(e, 0, sizeof *e);
  e->kind = DECL_DEF;
  e->origin = ORIGIN_DOMAIN;
  e->line = field->line;
  e->col = field->col;
  e->name = name;
  e->block_count = 1u;
  checker_reset(c, c->decl_index, name);
  return name_free(c, name) && check_def_body(c, e, val_univ(c->m, k), NULL, body);
}

/* A cofamily (S4, rule R1): the shape check, then the label types, then
   the generated text, lexed with `.` as a name character and checked
   declaration by declaration. */
static int check_cofamily(Checker *c, const Decl *d) {
  Machine *m = c->m;
  size_t count = d->ctor_count == 1u ? d->ctors[0].field_count : 0u;
  size_t size = strlen(d->name) + 10u;
  CocellField *kinds = arena_alloc(m->arena, (count + 1u) * sizeof *kinds);
  uint64_t *labels = arena_alloc(m->arena, (count + 1u) * sizeof *labels);
  const Core **label_types = arena_alloc(m->arena, (count + 1u) * sizeof *label_types);
  char *source = arena_alloc(m->arena, size);
  Text t = {NULL, 0, 0};
  TokenList toks = {0};
  DeclList list = {NULL, 0, 0};
  uint64_t n = 0;
  size_t i;
  if (kinds == NULL || labels == NULL || label_types == NULL || source == NULL)
    return FAIL(c, "OOM", "out of memory");
  if (!cocell_shape(c, d, &n, kinds, labels, label_types))
    return 0;
  snprintf(source, size, "cofamily %s", d->name);
  cocell_text(&t, d, n, kinds);
  t.cap = t.len + 1u;
  t.len = 0;
  t.text = arena_alloc(m->arena, t.cap);
  if (t.text == NULL)
    return FAIL(c, "OOM", "out of memory");
  cocell_text(&t, d, n, kinds);
  for (i = 0; i < count; i++)
    if (kinds[i] == COCELL_LABELLED && !check_label(c, d, &d->ctors[0].fields[i], labels[i], label_types[i]))
      return 0;
  if (!lex_internal(m->arena, source, t.text, t.len, &toks, m->diag) || !parse_source(m->arena, source, ORIGIN_DOMAIN, &toks, &list, m->diag))
    return 0;
  for (i = 0; i < list.count; i++)
    if (!check_generated(c, &list.items[i]))
      return 0;
  return 1;
}

static int check_decl(Checker *c, const Decl *d) {
  if ((d->is_event || d->is_state) && guest(c))
    return kit_refused(c, d->is_state ? "the state form" : "an event");
  if (d->is_event)
    return 1;
  if (d->is_state)
    return check_state(c, d);
  if (d->kind == DECL_FAMILY && d->origin == ORIGIN_PROGRAM)
    return FAIL(c, "REFUSE_DATA", "a program cannot declare a family (rule R1); only domain/domain.lang can");
  if (d->kind == DECL_COFAMILY && d->origin == ORIGIN_PROGRAM)
    return FAIL(c, "REFUSE_DATA", "a program cannot declare a cofamily (rule R1); only domain/domain.lang can");
  if (d->is_law && d->origin == ORIGIN_PROGRAM)
    return FAIL(c, "REFUSE_DATA", "a program cannot declare a law def (rule O7); only domain/domain.lang can");
  if (d->kind == DECL_AXIOM)
    return FAIL(c, "REFUSE_AXIOM", "an axiom has no proof (rule R2)");
  if (d->is_rec)
    return FAIL(c, "REFUSE_REC", "def rec is refused (rule R3); use fold or unfold");
  if (!kernel_sig(c, d) || !name_free(c, d->name))
    return 0;
  switch (d->kind) {
  case DECL_FAMILY:
    return check_family(c, d);
  case DECL_COFAMILY:
    return check_cofamily(c, d);
  case DECL_DEF:
  case DECL_AXIOM:
    break;
  }
  return check_def(c, d);
}

int check_program(Arena *arena, const DeclList *decls, Machine *m, Diag *diag, CheckMode mode) {
  Checker c;
  size_t ctors = 0;
  size_t extra = 0;
  size_t i;
  memset(m, 0, sizeof *m);
  m->arena = arena;
  m->diag = diag;
  m->fuel = EVAL_FUEL_STEPS;
  /* A cofamily with F fields elaborates to at most 2F + 4 definitions, one
     family and F + 1 constructors (S4). */
  for (i = 0; i < decls->count; i++) {
    size_t fields = decls->items[i].kind == DECL_COFAMILY && decls->items[i].ctor_count == 1u ? decls->items[i].ctors[0].field_count : 0u;
    int cofamily = decls->items[i].kind == DECL_COFAMILY;
    ctors += decls->items[i].ctor_count + (cofamily ? fields + 1u : 0u);
    extra += cofamily ? 2u * fields + 8u : 0u;
  }
  m->defs = arena_alloc(arena, (decls->count + extra + 1u) * sizeof *m->defs);
  m->families = arena_alloc(arena, (decls->count + extra + 1u) * sizeof *m->families);
  m->ctors = arena_alloc(arena, (ctors + 1u) * sizeof *m->ctors);
  if (m->defs == NULL || m->families == NULL || m->ctors == NULL)
    return diag_fail(diag, "OOM", NULL, "out of memory");
  m->def_cap = (uint32_t)(decls->count + extra);
  m->family_cap = (uint32_t)(decls->count + extra);
  m->ctor_cap = (uint32_t)ctors;
  memset(&c, 0, sizeof c);
  c.m = m;
  c.decls = decls;
  c.mode = mode;
  for (i = 0; i < decls->count; i++) {
    checker_reset(&c, i, decls->items[i].name);
    if (!check_decl(&c, &decls->items[i]))
      return 0;
    /* The other sorts of a block are checked with its first sort (S6). */
    i += decls->items[i].kind == DECL_FAMILY && decls->items[i].block_count > 1u ? decls->items[i].block_count - 1u : 0u;
  }
  m->def = NULL;
  return 1;
}

int entry_of(Machine *m, const Value *type, Entry *out) {
  const Value *t = type;
  memset(out, 0, sizeof *out);
  while (t != NULL && t->kind == VAL_PI && out->param_count < ENTRY_PARAMS_MAX) {
    if (!val_is(t->dom, OP_NAT) && !val_is(t->dom, OP_FLAG))
      return 0;
    out->param_flag[out->param_count] = val_is(t->dom, OP_FLAG);
    t = closure_apply(m, t, val_var(m, out->param_count));
    out->param_count++;
  }
  out->result_flag = val_is(t, OP_FLAG);
  return val_is(t, OP_NAT) || val_is(t, OP_FLAG);
}

static int parse_u64(const char *s, uint64_t *out) {
  uint64_t n = 0;
  size_t i;
  if (s[0] == '\0')
    return 0;
  for (i = 0; s[i] != '\0'; i++) {
    unsigned d = (unsigned)(unsigned char)s[i] - (unsigned)'0';
    if (d > 9u || n > (UINT64_MAX - d) / 10u)
      return 0;
    n = n * 10u + d;
  }
  *out = n;
  return 1;
}

/* The type of an entry argument (slice K3b adds U256 and Addr). */
typedef enum { ARG_TYPE_NAT, ARG_TYPE_FLAG, ARG_TYPE_U256, ARG_TYPE_ADDR } ArgType;

static const char *const ARG_TEXT[] = {
  "Nat (0 to 2^64-1)", "Flag (0 or 1)", "U256 (5u or 0x and hex digits, then u)", "Addr (0x and 40 hex digits)",
};

static int hex_digit(char ch, int hex) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (hex && ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (hex && ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

/* A word literal: an Addr is 0x and 40 hex digits; a U256 is decimal digits,
   or 0x and hex digits, then u. Returns 1 when TEXT is one. */
static int parse_word(const char *text, uint64_t sort, U256 *out) {
  size_t n = strlen(text);
  int hex = n > 2u && text[0] == '0' && text[1] == 'x';
  size_t start = hex ? 2u : 0u;
  size_t end = sort == WORD_U256 && n > 0u ? n - 1u : n;
  size_t i;
  if (end <= start || (sort == WORD_U256 && text[end] != 'u') || (sort == WORD_ADDR && (!hex || n != 42u)))
    return 0;
  u256_from_u64(out, 0);
  for (i = start; i < end; i++) {
    int d = hex_digit(text[i], hex);
    if (d < 0 || !u256_scale_add(out, hex ? 16u : 10u, (uint32_t)d))
      return 0;
  }
  return 1;
}

static const Value *word_arg(Machine *m, const char *text, uint64_t sort) {
  U256 *word = arena_alloc(m->arena, sizeof *word);
  if (word == NULL) {
    diag_fail(m->diag, "OOM", m->def, "out of memory");
    return NULL;
  }
  return parse_word(text, sort, word) ? val_word(m, sort, word) : NULL;
}

/* An entry argument: a Nat in decimal, a Flag as 0 or 1, a U256 or an Addr
   in the literal form of the language. */
static const Value *entry_arg(Machine *m, const char *text, ArgType type) {
  uint64_t n = 0;
  const Value *v = NULL;
  switch (type) {
  case ARG_TYPE_NAT:
    v = parse_u64(text, &n) ? val_nat(m, n) : NULL;
    break;
  case ARG_TYPE_FLAG:
    v = parse_u64(text, &n) && n <= 1u ? val_make(m, n == 1u ? OP_FLAG_YES : OP_FLAG_NO, 0, 0, NULL, NULL, NULL) : NULL;
    break;
  case ARG_TYPE_U256:
    v = word_arg(m, text, WORD_U256);
    break;
  case ARG_TYPE_ADDR:
    v = word_arg(m, text, WORD_ADDR);
    break;
  }
  if (v == NULL && m->diag->code == NULL)
    diag_fail(m->diag, "EVAL_ARGS", m->def, "'%s' is not a %s", text, ARG_TEXT[type]);
  return v;
}

static int print_result(Machine *m, const char *name, const Value *v, FILE *out, FILE *err) {
  switch (v->kind) {
  case VAL_NAT:
    fprintf(out, "%llu\n", (unsigned long long)v->nat);
    return 0;
  case VAL_TRAP:
    fputs("trap\n", out);
    fprintf(err, "langc: EVAL_OVERFLOW: %s: an operation trapped: a Nat or U256 overflow, or a U256 division by 0\n", name);
    return 0;
  case VAL_OP:
    if (val_is(v, OP_FLAG_YES) || val_is(v, OP_FLAG_NO)) {
      fprintf(out, "%d\n", val_is(v, OP_FLAG_YES));
      return 0;
    }
    break;
  case VAL_WORD:
  case VAL_UNIV:
  case VAL_LAM:
  case VAL_PI:
  case VAL_SIGMA:
  case VAL_VAR:
  case VAL_APP:
  case VAL_STUCK:
  case VAL_PLAM:
  case VAL_PATHP:
  case VAL_IVAL:
  case VAL_PAPP:
    break;
  }
  diag_fail(m->diag, "EVAL_ENTRY", name, "the entry did not compute a number");
  return 1;
}

int eval_command(Machine *m, const char *name, char *const *args, int arg_count, FILE *out, FILE *err) {
  uint32_t d = find_def(m, name);
  Entry entry;
  const Value *v;
  char *buf;
  uint32_t i;
  m->def = name;
  if (d == NO_DEF) {
    diag_fail(m->diag, "EVAL_ENTRY", name, "no definition has this name");
    return 1;
  }
  v = def_value(m, d);
  if (v == NULL)
    return 1;
  if (!entry_of(m, m->defs[d].type, &entry) && arg_count != 0) {
    diag_fail(m->diag, "EVAL_ARGS", name, "the definition is not an entry, so it takes no arguments");
    return 2;
  }
  if (!entry_of(m, m->defs[d].type, &entry)) {
    buf = arena_alloc(m->arena, PRINT_MAX);
    if (buf == NULL)
      return diag_fail(m->diag, "OOM", name, "out of memory") + 1;
    if (!value_print(m, NULL, 0, v, buf, PRINT_MAX)) {
      diag_fail(m->diag, "EVAL_PRINT", name, "the normal form cannot be printed as source syntax");
      return 1;
    }
    fprintf(out, "%s\n", buf);
    return 0;
  }
  if ((uint32_t)arg_count != entry.param_count) {
    diag_fail(m->diag, "EVAL_ARGS", name, "the entry takes %u arguments, found %d", entry.param_count, arg_count);
    return 2;
  }
  for (i = 0; i < entry.param_count && v != NULL; i++) {
    const Value *arg = entry_arg(m, args[i], entry.param_flag[i] ? ARG_TYPE_FLAG : ARG_TYPE_NAT);
    v = arg == NULL ? NULL : apply_value(m, v, arg);
  }
  if (v == NULL)
    return strcmp(m->diag->code != NULL ? m->diag->code : "", "EVAL_ARGS") == 0 ? 2 : 1;
  return print_result(m, name, v, out, err);
}

int prove_command(Machine *m, const char *name, const char *poll, FILE *out, FILE *err) {
  uint32_t d = find_def(m, name);
  const Value *v;
  const Value *arg;
  const U256 *poll_word;
  U256 zero;
  U256 choice;
  U256 sig[BLSAG_RING_MAX + 3u];
  uint32_t n = 0;
  uint32_t i;
  char buf[96];
  ZkWitnessVerdict verdict;
  m->def = name;
  if (d == NO_DEF) {
    diag_fail(m->diag, "EVAL_ENTRY", name, "no definition has this name");
    return 1;
  }
  v = def_value(m, d);
  if (v == NULL)
    return 1;
  u256_from_u64(&zero, 0u);
  poll_word = &zero;
  if (poll != NULL) {
    arg = entry_arg(m, poll, ARG_TYPE_U256);
    if (arg == NULL)
      return 2;
    poll_word = arg->word;
  }
  if (!val_is(v, OP_ZK_CAST)) {
    diag_fail(m->diag, "PROVE_BALLOT", name, "the definition is not a private ballot (castPrivate m c)");
    return 1;
  }
  verdict = zk_witness(m, v, poll_word, &choice, sig, &n);
  switch (verdict) {
  case ZK_WITNESS_OPEN:
    diag_fail(m->diag, "PROVE_OPEN", name, "the member or the choice of the ballot is open");
    return 1;
  case ZK_WITNESS_TRAP:
    fputs("trap\n", out);
    break;
  case ZK_WITNESS_OK:
    word_text(WORD_U256, &choice, buf, sizeof buf);
    fprintf(out, "%s\n", buf);
    for (i = 0; i < n + 3u; i++) {
      word_text(WORD_U256, &sig[i], buf, sizeof buf);
      fprintf(out, "%s\n", buf);
    }
    break;
  }
  if (fflush(out) != 0 || ferror(out)) {
    diag_fail(m->diag, "IO_WRITE", name, "cannot write proof output");
    return 2;
  }
  if (verdict == ZK_WITNESS_TRAP)
    fprintf(err, "langc: EVAL_OVERFLOW: %s: an operation trapped: a Nat or U256 overflow, or a U256 division by 0\n", name);
  return 0;
}

/* The role of a definition in a run (slice K3b, D-3), found by its type. */
typedef enum { ROLE_HELPER, ROLE_ENTRY, ROLE_VIEW } Role;

typedef struct {
  uint32_t param_count;
  ArgType params[ENTRY_PARAMS_MAX];
} RunCall;

/* A run: the script path, the prelude families, the calls so far, the
   current state and a print buffer of PRINT_MAX bytes. */
typedef struct {
  const char *path;
  uint32_t env;
  uint32_t out;
  uint32_t calls;
  const Value *state;
  char *buf;
} Run;

static uint32_t find_family(const Machine *m, const char *name) {
  uint32_t i;
  for (i = 0; i < m->family_count; i++) {
    if (strcmp(m->families[i].name, name) == 0)
      return i;
  }
  return NO_FAMILY;
}

static int is_family(const Value *t, uint32_t family) {
  return t != NULL && val_is(t, OP_FAMILY) && t->inst == family;
}

static int arg_type(const Value *t, ArgType *out) {
  *out = val_is(t, OP_FLAG) ? ARG_TYPE_FLAG : val_is(t, OP_U256) ? ARG_TYPE_U256 : val_is(t, OP_ADDR) ? ARG_TYPE_ADDR : ARG_TYPE_NAT;
  return t != NULL && (val_is(t, OP_NAT) || val_is(t, OP_FLAG) || val_is(t, OP_U256) || val_is(t, OP_ADDR));
}

/* Entry: (env : Env) -> (s : State) -> ARGS -> Option (Prod State (List Out)).
   View: the same parameters, with a Nat, Flag, U256 or Addr result. Each
   argument is a Nat, Flag, U256 or Addr. All other definitions are helpers. */
static Role role_of(Machine *m, const Run *r, const Value *type, RunCall *call) {
  const Value *t = type;
  const Value *p;
  ArgType kind;
  memset(call, 0, sizeof *call);
  if (t == NULL || t->kind != VAL_PI || !is_family(t->dom, r->env))
    return ROLE_HELPER;
  t = closure_apply(m, t, val_var(m, 0));
  if (t == NULL || t->kind != VAL_PI || !is_family(t->dom, m->state_family))
    return ROLE_HELPER;
  t = closure_apply(m, t, val_var(m, 1));
  while (t != NULL && t->kind == VAL_PI && call->param_count < ENTRY_PARAMS_MAX && arg_type(t->dom, &kind)) {
    call->params[call->param_count] = kind;
    t = closure_apply(m, t, val_var(m, call->param_count + 2u));
    call->param_count++;
  }
  if (arg_type(t, &kind))
    return ROLE_VIEW;
  p = val_is(t, OP_OPTION) ? t->args[0] : NULL;
  if (val_is(p, OP_PROD) && is_family(p->args[0], m->state_family) && val_is(p->args[1], OP_LIST) && is_family(p->args[1]->args[0], r->out))
    return ROLE_ENTRY;
  return ROLE_HELPER;
}

static int has_trap(const Value *v) {
  uint32_t i;
  if (v != NULL && v->kind == VAL_TRAP)
    return 1;
  for (i = 0; v != NULL && v->kind == VAL_OP && i < v->argc; i++) {
    if (has_trap(v->args[i]))
      return 1;
  }
  return 0;
}

static int print_value(Machine *m, const Value *v, char *buf) {
  return value_print(m, NULL, 0, v, buf, PRINT_MAX) ? 1 : diag_fail(m->diag, "EVAL_PRINT", m->def, "the normal form exceeds the printer limits");
}

/* The kit Env has one field, the caller (S3: no clock, PLAN section 4). */
static const Value *make_env(Machine *m, const Run *r, const U256 *caller) {
  const Value **args = arena_alloc(m->arena, sizeof *args);
  if (args == NULL) {
    diag_fail(m->diag, "OOM", m->def, "out of memory");
    return NULL;
  }
  args[0] = val_word(m, WORD_ADDR, caller);
  return args[0] == NULL ? NULL : val_op(m, VAL_OP, OP_CTOR, m->families[r->env].first_ctor, 0, args, 1u);
}

/* One call `CALLER NAME ARGS...` (D-6 to D-8). A trap is a revert: the
   state does not change. Returns 0, 1, or 2 after EVAL_ARGS. */
static int run_call(Machine *m, Run *r, uint32_t line, char *const *word, uint32_t count, FILE *out) {
  const Value *args[ENTRY_PARAMS_MAX];
  U256 *caller = arena_alloc(m->arena, sizeof *caller);
  const char *name = count > 1u ? word[1] : "";
  uint32_t d = count > 1u ? find_def(m, name) : NO_DEF;
  RunCall call;
  Role role;
  const Value *v;
  uint32_t i;
  if (caller == NULL)
    return diag_fail(m->diag, "OOM", NULL, "out of memory") + 1;
  if (count < 2u)
    return diag_fail(m->diag, "RUN_SCRIPT", NULL, "%s:%u: a call is CALLER NAME ARGS...", r->path, line) + 1;
  if (!parse_word(word[0], WORD_ADDR, caller))
    return diag_fail(m->diag, "RUN_SCRIPT", NULL, "%s:%u: CALLER '%s' is not an Addr (0x and 40 hex digits)", r->path, line, word[0]) + 1;
  m->fuel = EVAL_FUEL_STEPS;
  m->def = name;
  if (d == NO_DEF)
    return diag_fail(m->diag, "EVAL_ENTRY", name, "%s:%u: no definition has this name", r->path, line) + 1;
  role = role_of(m, r, m->defs[d].type, &call);
  if (role == ROLE_HELPER)
    return diag_fail(m->diag, "EVAL_ENTRY", name, "%s:%u: a helper is not an entry or a view", r->path, line) + 1;
  if (count - 2u != call.param_count)
    return diag_fail(m->diag, "EVAL_ARGS", name, "%s:%u: the call takes %u arguments, found %u", r->path, line, call.param_count, count - 2u) + 2;
  for (i = 0; i < call.param_count; i++) {
    args[i] = entry_arg(m, word[i + 2u], call.params[i]);
    if (args[i] == NULL)
      return strcmp(m->diag->code != NULL ? m->diag->code : "", "EVAL_ARGS") == 0 ? 2 : 1;
  }
  v = apply_value(m, apply_value(m, def_value(m, d), make_env(m, r, caller)), r->state);
  for (i = 0; i < call.param_count; i++)
    v = apply_value(m, v, args[i]);
  if (v == NULL)
    return 1;
  r->calls++;
  if (has_trap(v)) {
    fprintf(out, "%u %s trap\n", r->calls, name);
    return 0;
  }
  if (role == ROLE_VIEW) {
    if (!print_value(m, v, r->buf))
      return 1;
    fprintf(out, "%u %s = %s\n", r->calls, name, r->buf);
    return 0;
  }
  if (val_is(v, OP_NONE)) {
    fprintf(out, "%u %s revert\n", r->calls, name);
    return 0;
  }
  if (!val_is(v, OP_SOME) || !val_is(v->args[0], OP_PAIR))
    return diag_fail(m->diag, "EVAL_ENTRY", name, "%s:%u: the entry did not compute some or none", r->path, line) + 1;
  r->state = v->args[0]->args[0];
  fprintf(out, "%u %s ok\n", r->calls, name);
  for (v = v->args[0]->args[1]; val_is(v, OP_CONS); v = v->args[1]) {
    const Value *o = v->args[0];
    if (!print_value(m, o, r->buf))
      return 1;
    fprintf(out, "  %s%s\n", val_is(o, OP_CTOR) && o->inst < m->ctor_count && m->ctors[o->inst].event ? "event " : "", r->buf);
  }
  return 0;
}

/* Splits LINE at spaces and tabs. Returns the word count; keeps CAP words. */
static uint32_t split_words(char *line, char **word, uint32_t cap) {
  uint32_t count = 0;
  char *p = line;
  while (*p != '\0') {
    if (*p == ' ' || *p == '\t' || *p == '\r') {
      *p++ = '\0';
      continue;
    }
    if (count < cap)
      word[count] = p;
    count++;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r')
      p++;
  }
  return count;
}

int run_command(Machine *m, const char *path, const char *text, size_t len, FILE *out) {
  char *word[ENTRY_PARAMS_MAX + 4u];
  uint32_t init = find_def(m, "init");
  uint32_t line = 0;
  size_t pos = 0;
  int status = 0;
  Run r;
  memset(&r, 0, sizeof r);
  r.path = path;
  r.env = find_family(m, "Env");
  r.out = find_family(m, "Out");
  r.buf = arena_alloc(m->arena, PRINT_MAX);
  m->def = "init";
  if (!m->has_state || init == NO_DEF || !is_family(m->defs[init].type, m->state_family) || r.env == NO_FAMILY)
    return diag_fail(m->diag, "RUN_INIT", NULL, "a run needs a state and def init : State") + 1;
  if (r.buf == NULL)
    return diag_fail(m->diag, "OOM", NULL, "out of memory") + 1;
  m->fuel = EVAL_FUEL_STEPS;
  r.state = def_value(m, init);
  if (r.state == NULL)
    return 1;
  if (has_trap(r.state))
    return diag_fail(m->diag, "RUN_INIT", "init", "the start state traps") + 1;
  while (status == 0 && pos < len) {
    size_t end = pos;
    char *copy;
    uint32_t count;
    while (end < len && text[end] != '\n')
      end++;
    if (memchr(text + pos, '\0', end - pos) != NULL)
      return diag_fail(m->diag, "RUN_SCRIPT", NULL, "%s:%u: a script line contains a NUL byte", r.path, line + 1u) + 1;
    copy = arena_alloc(m->arena, end - pos + 1u);
    if (copy == NULL)
      return diag_fail(m->diag, "OOM", NULL, "out of memory") + 1;
    memcpy(copy, text + pos, end - pos);
    copy[end - pos] = '\0';
    line++;
    count = split_words(copy, word, ENTRY_PARAMS_MAX + 4u);
    if (count != 0u && strncmp(word[0], "--", 2) != 0)
      status = run_call(m, &r, line, word, count, out);
    pos = end + 1u;
  }
  if (status != 0)
    return status;
  m->def = NULL;
  if (!print_value(m, r.state, r.buf))
    return 1;
  fprintf(out, "state %s\n", r.buf);
  return 0;
}
