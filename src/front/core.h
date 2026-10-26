/* Core terms, values and the evaluator. SHARED by the tcc kits.

   The checker (front/check.c) writes core terms. Evaluation is normalization
   by evaluation: each value is a normal form. A neutral value (VAL_VAR,
   VAL_APP, VAL_STUCK) is stuck on a variable. A trap (an overflow or a division by 0) is a
   value too. It goes up through each strict position (an arithmetic operation, the
   scrutinee of an eliminator, the function of an application) and nowhere
   else. So the evaluator and the residual program agree: a trap that no
   strict position reads does not stop the program. */
#ifndef LANG_FRONT_CORE_H
#define LANG_FRONT_CORE_H

#include "front/ast.h"
#include "front/base.h"

#define EVAL_DEPTH_LIMIT 2000u
#define EVAL_FUEL_STEPS 20000000u

/* The core names: type formers, constructors and operations. */
typedef enum {
  OP_NAT,
  OP_FLAG,
  OP_U256,
  OP_ADDR,
  OP_UNIT,
  OP_PROD,
  OP_SUM,
  OP_OPTION,
  OP_LIST,
  OP_FAMILY,
  OP_UNIT_VAL,
  OP_FLAG_YES,
  OP_FLAG_NO,
  OP_PAIR,
  OP_INL,
  OP_INR,
  OP_NONE,
  OP_SOME,
  OP_NIL,
  OP_CONS,
  OP_PACK,
  OP_CTOR,
  OP_FIRST,
  OP_SECOND,
  OP_EITHER,
  OP_OPTION_ELIM,
  OP_ELIM,
  OP_PURE,
  OP_MAP,
  OP_BIND,
  OP_FOLD_NAT,
  OP_FOLD_LIST,
  OP_FOLD_FAMILY,
  OP_UNFOLD,
  OP_FILTER,
  OP_WITNESS,
  OP_PAYLOAD,
  OP_NAT_ADD,
  OP_NAT_SUB,
  OP_NAT_EQ,
  OP_NAT_LE,
  OP_FLAG_IF,
  OP_U256_ADD,
  OP_U256_SUB,
  OP_U256_LE,
  OP_U256_EQ,
  OP_TO_U256,
  OP_ADDR_EQ,
  /* sealed-lang S2 (R14, R16 to R18, R20): the base span. EC_ADD and EC_MUL
     compute on secp256k1 in the host (slice S4 removed ECRECOVER and
     EC_PAIRING). */
  OP_NAT_DIV,
  OP_NAT_MOD,
  OP_FM_LEN, /* S3: len A r, the op count of a FreeMon trace (survey section 7) */
  OP_HASH,
  OP_EC_ADD,
  OP_EC_MUL,
  OP_ADDR_TO_U256,
  OP_PROJ,
  /* Slice K4a0: Map K V, the map value (k0 v0 k1 v1 ... with the keys in
     order and no zero value), mapGet V m k and mapSet V m k v. */
  OP_KMAP,
  OP_KMAP_OF,
  OP_KMAP_GET,
  OP_KMAP_SET,
  /* Slice S8 B2a: bLSAG over secp256k1 in the host (front/blsag.c). Kit only,
     `langc eval` only: zkHp, zkKey, zkDigest, zkSign and zkVerify. */
  OP_ZK_HP,
  OP_ZK_KEY,
  OP_ZK_DIGEST,
  OP_ZK_SIGN,
  OP_ZK_VERIFY,
  OP_ZK_BALLOT,  /* ZkBallot C: the private ballot carrier for choices C (S8 B2b) */
  OP_ZK_WITNESS, /* ZkWitness: the witness carrier; OP_WITNESS is the Sigma projection */
  OP_ZK_CAST,    /* zkCast M C m c: a canonical ballot, the arguments kept */
  OP_ZK_PROVE,   /* zkProve b: the witness word list, poll id first */
  OP_ZK_PROJECT, /* zkProject C b: the choice of a ballot */
  OP_ZK_CHECK,   /* zkCheck M C w c: the type Unit on a host pass, else neutral */
  OP_INTERVAL, /* the sort I: never a type, only the type of an interval term */
  OP_TRANSP,   /* transp L phi a: L a line (a path value in U), phi a face */
  OP_SYSTEM,   /* a system: key0, branch0, key1, branch1, ...; each branch a PLAM */
  OP_HCOMP,    /* hcomp A phi sys u0: phi a face, sys an OP_SYSTEM value */
  OP_FIN,      /* Fin n: the numbers below n; a value is a VAL_NAT literal */
  OP_FIN_ADD,  /* finAdd n a b: (a + b) mod n */
  OP_FIN_NEG,  /* finNeg n a: (n - a) mod n */
  OP_FIN_CASE, /* finAll case: k, body0, ..., body(n-1); body j at the literal j */
  OP_GLUE,     /* Glue A phi sys: a type; sys as in OP_HCOMP, each branch pack T e */
  OP_GLUE_ELEM, /* glue: phi, sys, a; sys as in OP_HCOMP, each branch t */
  OP_UNGLUE    /* unglue: A, phi, sys, g; stuck when no rule applies */
} Op;

/* The instance of pure, map, bind and filter (formers F5 and F8). */
typedef enum {
  CARRIER_OPTION,
  CARRIER_LIST,
  CARRIER_SUM
} Carrier;

typedef enum {
  CORE_VAR,    /* index: de Bruijn index */
  CORE_GLOBAL, /* index: definition number */
  CORE_NAT,    /* nat */
  CORE_WORD,   /* word; nat is WORD_U256 or WORD_ADDR */
  CORE_TRAP,   /* an overflow or a division by 0; only quote writes it */
  CORE_UNIV,   /* Type nat */
  CORE_LAM,    /* fun name => b; a retains an explicit annotation for checks */
  CORE_PI,     /* (name : a) -> b */
  CORE_SIGMA,  /* Sigma (name : a) b */
  CORE_APP,    /* a b */
  CORE_OP,     /* a core name with all of its arguments */
  CORE_PLAM,   /* fun name => b, name an interval variable */
  CORE_PAPP,   /* a @ b; args[0], args[1]: the two ends of the path a */
  CORE_PATHP,  /* PathP (fun name => b) args[0] args[1] */
  CORE_IZERO,  /* i0 */
  CORE_IONE,   /* i1 */
  CORE_INEG,   /* ~a */
  CORE_IMEET,  /* a /\ b */
  CORE_IJOIN   /* a \/ b */
} CoreKind;

typedef struct Core Core;
struct Core {
  CoreKind kind;
  uint32_t index;
  uint64_t nat;
  const U256 *word; /* WORD: the limbs */
  Op op;
  /* OP_FAMILY, OP_FOLD_FAMILY: the family. OP_CTOR, OP_PROJ: the
     constructor. OP_PURE, OP_MAP, OP_BIND, OP_FILTER: the Carrier. */
  uint32_t inst;
  uint32_t field; /* OP_PROJ: the field number */
  const char *name;
  const Core *a;
  const Core *b;
  const Core *const *args;
  uint32_t argc;
};

typedef enum {
  VAL_NAT,
  VAL_WORD, /* nat is WORD_U256 or WORD_ADDR */
  VAL_TRAP,
  VAL_UNIV,
  VAL_LAM,
  VAL_PI,
  VAL_SIGMA,
  VAL_OP,   /* a canonical value or a type */
  VAL_VAR,  /* neutral: a variable at a de Bruijn level */
  VAL_APP,  /* neutral: a neutral function applied to a value */
  VAL_STUCK, /* neutral: an operation with a neutral scrutinee */
  VAL_PLAM,  /* a path abstraction (a closure on an interval variable) */
  VAL_PATHP, /* a path type: the line is the closure, args[0..1] the ends */
  VAL_IVAL,  /* an interval term in normal form */
  VAL_PAPP   /* neutral: a neutral path at an interval value */
} ValKind;

/* An interval literal is level * 2 + 1 for a negated variable, else
   level * 2. A conjunction is a sorted array of distinct literals. */
typedef struct {
  const uint32_t *lits;
  uint32_t count;
} IConj;

/* The closure of a LAM or PLAM value (R11 (a)). CLOS_CORE is the S1 shape
   (env + body). Each other tag is a closure of a Kan rule: ARGS holds its
   values, L is the line and phi the face. */
typedef enum {
  CLOS_CORE,
  CLOS_LINE_PART, /* PLAM: r |-> the type argument FIELD of L(r); args: L */
  CLOS_PI_DOM,    /* PLAM: j |-> the domain of L(r \/ ~j); args: L, r */
  CLOS_PI_COD,    /* PLAM: i |-> B(i) (x'(i)); args: L, phi, x */
  CLOS_TRANSP_PI, /* LAM: x |-> transp B' phi (f (x'(i0))); args: L, phi, f */
  CLOS_SIGMA_DOM, /* PLAM: j |-> the domain of L(r /\ j); args: L, r */
  CLOS_SIGMA_COD, /* PLAM: i |-> B(i) (a'(i)); args: L, phi, u */
  /* S2b. A SYS tag is the map of one branch b of a system. */
  CLOS_CONST,          /* PLAM: _ |-> v; args: v */
  CLOS_SYS_APP,        /* PLAM: i |-> b i x; args: b, x */
  CLOS_SYS_PAPP,       /* PLAM: i |-> b i @ j; args: b, j, a, b1 (the ends) */
  CLOS_SYS_SIGMA,      /* PLAM: i |-> the part FIELD of b i; args: b */
  CLOS_SYS_ARG,        /* PLAM: i |-> the argument FIELD of b i; args: b */
  CLOS_SYS_MEET,       /* PLAM: j |-> b (i /\ j); args: b, i */
  CLOS_COMP_BRANCH,    /* PLAM: i |-> transp (fun j => L (i \/ j)) i (b i); args: b, L */
  CLOS_LINE_JOIN,      /* PLAM: j |-> L (i \/ j); args: L, i */
  CLOS_HCOMP_PI,       /* LAM: x |-> hcomp (B x) phi (sys x) (u0 x); args: A, phi, sys, u0 */
  CLOS_HCOMP_PATHP,    /* PLAM: j |-> hcomp (A j) ...; args: A, phi, sys, u0 */
  CLOS_SIGMA_FILL_COD, /* PLAM: i |-> B (hfill A phi sys u i); args: A, phi, sys, u */
  CLOS_TRANSP_PATHP,   /* PLAM: r |-> comp of p0 @ r (rule 7); args: L, phi, p0 */
  CLOS_PATHP_LINE,     /* PLAM: i |-> L(i) at r; args: L, r */
  CLOS_PATHP_END,      /* PLAM: i |-> the end FIELD of L(i); args: L */
  CLOS_SYS_FOLD,       /* PLAM: j |-> fold (b j) with the cases and RES of F; args: b, F (a stuck fold); field: the family */
  /* transp at a family with parameters (S4 item 11). */
  CLOS_FAMILY_FIELD,   /* PLAM: r |-> the type of the point field FIELD of the constructor of v at the parameters of L(r) and the fills at r of the earlier fields (item 13); args: L, v, phi */
  CLOS_SYS_TRANSP,     /* PLAM: j |-> transp L phi (b j); args: b, L, phi */
  CLOS_LINE_MEET,      /* PLAM: j |-> L (i /\ j); args: L, i */
  /* S4 item 14. */
  CLOS_FOLD_PATH,      /* PLAM: k |-> fold (p @ k) with the cases and RES of F; args: p, a, b (the ends of p), F (a stuck fold); field: the family */
  CLOS_FOLD_FACE,      /* PLAM: k |-> fold (X at the first interval E, the second k) with the cases and RES of F; args: X, E, F (a stuck fold); field: the family */
  /* transp at Glue (S3b). FIELD is the fresh level of the key KEYF. */
  CLOS_GLUE_BASE,      /* PLAM: r |-> A(r) of the Glue line L; args: L */
  CLOS_GLUE_T,         /* PLAM: j |-> T(r /\ j) of the branch KEYF; args: L, keyf, r */
  CLOS_GLUE_FILL,      /* PLAM: r |-> witness (e r) (transpFill T psi u0 r); args: L, keyf, psi, u0 */
  CLOS_CONTR_AT,       /* PLAM: j |-> c x @ j, c a contraction; args: c, x, center */
  CLOS_PATH_REV,       /* PLAM: j |-> w @ ~j; args: w, a, b (the ends of w) */
  CLOS_SYS_UNGLUE,     /* PLAM: j |-> unglue (b j) at the Glue type G; args: b, G */
  CLOS_GLUE_HFILL,     /* PLAM: j |-> witness e (hfill T psi u u0 j); args: pack T e, psi, u, u0 */
  CLOS_SYS_RESTRICT,   /* PLAM: j |-> b j restricted to the Glue key K; args: b, K */
  CLOS_EQUIV_LINE      /* PLAM: j |-> Equiv T (b (~j)) (hcomp in U); args: b, T */
} ClosTag;

typedef struct Value Value;
typedef struct Env Env;

/* The head is de Bruijn index 0. */
struct Env {
  const Value *value;
  const Env *next;
};

struct Value {
  ValKind kind;
  uint64_t nat; /* NAT: the number. WORD: the sort. UNIV: the level. VAR: the level. */
  const U256 *word; /* WORD: the limbs */
  Op op;        /* OP, STUCK */
  uint32_t inst;
  uint32_t field;
  const Value *const *args;
  uint32_t argc;
  const char *name; /* LAM, PI, SIGMA: the binder */
  const Value *dom; /* PI, SIGMA: the domain. APP, PAPP: the function. */
  const Value *arg; /* APP, PAPP: the argument. A PI with no body: the codomain. */
  const Env *env;   /* LAM, PI, SIGMA, PLAM, PATHP: the closure */
  const Core *body;
  ClosTag tag;      /* LAM, PLAM: CLOS_CORE, or a Kan rule closure */
  /* IVAL: the join of CONJ_COUNT conjunctions, in a canonical order, with
     no conjunction that contains another. i0 has no conjunction; i1 has
     one empty conjunction. */
  const IConj *conj;
  uint32_t conj_count;
};

typedef struct {
  const char *name;
  /* In the context of the family parameters and the earlier point fields
     (S4 item 13): index 0 is the field before this one. */
  const Core *type;
  int recursive;      /* the field type is the family itself */
  int dependent;      /* the field type uses an earlier point field */
  int over_recursive; /* the field type uses an earlier recursive field */
  /* A path field `Path F x y` (S4 item 14): x and y are the earlier
     recursive fields ENDS[0] and ENDS[1]. The fold gives it the type
     `Path RES x' y'`. */
  int path;
  uint32_t ends[2];
  int history;        /* a history field of the state (slice K3a) */
} FieldInfo;

typedef struct {
  const char *name;
  uint32_t family;
  const FieldInfo *fields;
  uint32_t field_count; /* the point fields only */
  int event; /* an event of the program, a constructor of Out (slice K4a1) */
  /* A path constructor (S4 R1 (a)): 1 or 2 interval fields after the
     point fields, and the boundary, an OP_SYSTEM core over the
     parameters, the point fields and the interval fields. A point
     constructor has 0 and NULL. */
  uint32_t interval_count;
  const Core *boundary;
  /* The result indices (S6): one core for each index of the family, in the
     context of the parameters and the point fields. NULL for a uniform
     family. */
  const Core *const *result;
} CtorInfo;

/* A domain family. Its constructors are ctors[first_ctor ...]. A family
   with one constructor has one projection for each field. */
typedef struct {
  const char *name;
  const Core *const *param_types;
  uint32_t param_count;
  uint32_t first_ctor;
  uint32_t ctor_count;
  uint64_t universe; /* the largest universe of a constructor field */
  Origin origin;     /* ORIGIN_KIT for Env and Out, the kit prelude of front.c */
  /* The index telescope (S6): index_count cores, index k in the context of
     the parameters and the indices before it. 0 for a uniform family. */
  const Core *const *index_types;
  uint32_t index_count;
  /* The block (S6): the families block_first up to block_first +
     block_count. A family outside a block is a block of one. */
  uint32_t block_first;
  uint32_t block_count;
  /* A sealed family (S5, rule O7): a guest elim or fold of it is law-only. */
  int sealed;
} FamilyInfo;

typedef struct {
  const char *name;
  Origin origin;
  const Core *body;
  const Value *type;
  const Value *value; /* computed when first used */
  /* A law-only def (S5, rule O7): a `law def` of the domain, or a guest def
     that uses one outside a Trunc1 argument. */
  int law_only;
} DefInfo;

/* The checked program and the evaluation state. No global state: every
   function gets this context. */
typedef struct {
  Arena *arena;
  Diag *diag;
  const char *def; /* the current definition, for diagnostics */
  DefInfo *defs;
  uint32_t def_count;
  uint32_t def_cap;
  FamilyInfo *families;
  uint32_t family_count;
  uint32_t family_cap;
  CtorInfo *ctors;
  uint32_t ctor_count;
  uint32_t ctor_cap;
  unsigned depth;
  uint64_t fuel;
  int overflowed;
  int has_state;          /* the program declares the state (slice K3a) */
  uint32_t state_family; /* the state family, when has_state is 1 */
  uint32_t fresh; /* the count of fresh levels for the Kan head test */
  /* Each Glue that a true key reduced, newest first. transp at Glue reads
     them to get A, phi and sys at an end. */
  const struct GlueRec *glue_recs;
} Machine;

/* One Glue that a true key reduced. OUT is the result, a fresh copy of the
   branch type T, so that its pointer is unique. RAW is the OP_GLUE value. */
typedef struct GlueRec {
  const Value *out;
  const Value *raw;
  const struct GlueRec *next;
} GlueRec;

/* Each function returns NULL (or 0) after a diagnostic. A NULL input gives a
   NULL result, so a caller can test once at the end. */
const Value *val_nat(Machine *m, uint64_t n);
const Value *val_word(Machine *m, uint64_t sort, const U256 *word);
/* The ZK witness of the canonical ballot B (castPrivate m c) under the poll
   id POLL (S8 B2c). OK sets CHOICE to the constructor index of c as a word,
   SIG[0] to SIG[N + 2] to c_1, s_1 to s_n, I.x and I.y, and N to the ring
   size. OPEN: B is not a canonical ballot, the member type is not finite,
   or m or c has no constructor index. TRAP: the ring or the signing fails. */
typedef enum { ZK_WITNESS_OK, ZK_WITNESS_OPEN, ZK_WITNESS_TRAP } ZkWitnessVerdict;
ZkWitnessVerdict zk_witness(Machine *m, const Value *b, const U256 *poll, U256 *choice, U256 *sig, uint32_t *n);
/* The constructor index of the closed value V in the finite type T (unit 0,
   inl and inr by cardinality, Fin, Pos). Returns 0 on an open value. Shared
   by zk_witness and the contract writer (S9 B1). */
int zk_index(const Machine *m, const Value *t, const Value *v, uint64_t *i);
const Value *val_var(Machine *m, uint32_t level);
/* The interval variable at LEVEL. */
const Value *val_ivar(Machine *m, uint32_t level);
/* i0 when SIDE is 0, else i1. */
const Value *val_iend(Machine *m, int side);
/* 1 when V is the interval value i0 (SIDE 0) or i1 (SIDE 1). */
int ival_is_end(const Value *v, int side);
/* The face normal form of PHI (R1 (a)): each conjunction that holds a
   literal and its negation goes, as the face of such a conjunction is
   empty. Face equality is then interval value equality. */
const Value *face_norm(Machine *m, const Value *phi);
/* The face normal form of A \/ B and of A /\ B. */
const Value *face_join(Machine *m, const Value *a, const Value *b);
const Value *face_meet(Machine *m, const Value *a, const Value *b);
/* The face map: R with the interval variable at LEVEL set to the end SIDE. */
const Value *ival_face(Machine *m, const Value *r, uint32_t level, int side);
/* The face map of the conjunction C on each interval value of ENV. The
   checker context binds only variables, so this is the act of C on the
   context (S1 R5 (a)). Returns 0 after a diagnostic. */
int env_face(Machine *m, const Env *env, const IConj *c, const Env **out);
/* P @ R. A and B are the ends of P (NULL only in a value that conversion
   alone reads). A neutral P at i0 gives A, at i1 gives B. */
const Value *path_apply(Machine *m, const Value *p, const Value *r, const Value *a, const Value *b);
const Value *val_univ(Machine *m, uint64_t level);
const Value *val_op(Machine *m, ValKind kind, Op op, uint32_t inst, uint32_t field, const Value *const *args, uint32_t argc);
const Value *val_make(Machine *m, Op op, uint32_t inst, uint32_t argc, const Value *a, const Value *b, const Value *c);
/* The constructor INST at the arguments A (S4 R2 (a)): a path constructor
   at a true face of its boundary is the branch. */
const Value *val_ctor(Machine *m, uint32_t inst, const Value *const *a, uint32_t n);
/* A non-dependent function type. */
const Value *val_arrow(Machine *m, const Value *dom, const Value *cod);
const Value *val_closure(Machine *m, ValKind kind, const char *name, const Value *dom, const Env *env, const Core *body);
int val_is(const Value *v, Op op);
const Env *env_push(Machine *m, const Env *env, const Value *value);
const Value *def_value(Machine *m, uint32_t index);
const Value *eval_core(Machine *m, const Env *env, const Core *core);
const Value *apply_value(Machine *m, const Value *fn, const Value *arg);
/* transp LINE PHI BASE, for JSON rule 1 (S3b). LINE is a path value in U. */
const Value *val_transp(Machine *m, const Value *line, const Value *phi, const Value *base);
/* The body of a LAM, PI, SIGMA, PLAM or PATHP value at ARG. */
const Value *closure_apply(Machine *m, const Value *binder, const Value *arg);
/* 1 when A and B have the same normal form. LEVEL is the next free level. */
int conv_values(Machine *m, uint32_t level, const Value *a, const Value *b);
const Core *quote_value(Machine *m, uint32_t level, const Value *v);
/* Writes V in the surface syntax. NAMES gives the names of the levels below
   NAME_COUNT. */
int value_print(Machine *m, const char *const *names, uint32_t name_count, const Value *v, char *buf, size_t cap);
const char *op_name(const Machine *m, Op op, uint32_t inst, uint32_t field);

#endif
