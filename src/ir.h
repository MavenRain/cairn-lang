/* The first-order IR between the front end and a target. SHARED by the tcc
   kits.

   Each value is one unsigned 64-bit word: a Nat, a Flag (0 or 1), a
   constructor tag, or a heap address. Locals are numbered from 0. The
   parameters of an entry are locals 0 to param_count - 1. A heap block is a
   run of words. A target sets its own word width in memory (Wasm: 8 bytes,
   EVM: 32 bytes) and resets its bump allocator at each call.

   Traps are lazy (the H3 ruling, see the kit README): no expression traps.
   The lowering gives each value a word and a trap flag (0 or 1). ADD and MUL
   wrap; ADD_CARRY and MUL_CARRY give the flag of an overflow. A heap block
   is word 0 = the tag, then for field K the word at 1 + 2K and its flag at
   2 + 2K. Only an entry result is a strict position at run time: when its
   flag is 1, the entry runs TRAP. DIV and MOD stop the call when right is
   0, so the lowering never gives them 0: it puts a zero divisor in the trap
   flag and divides by 1.

   A U256 (and a Digest) is a heap block of 4 words, the u64 limbs with the
   low limb first; the value is the block address. A CALL runs a prelude
   function: target code, not IR (cairn-lang S8, Q-S8-2). */
#ifndef LANG_IR_H
#define LANG_IR_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  IR_SCALAR_NAT,
  IR_SCALAR_FLAG,
  IR_SCALAR_U256 /* the address of a 4-word block */
} IrScalar;

typedef enum {
  IR_OP_ADD,       /* the sum modulo 2^64 */
  IR_OP_SUB,       /* truncates at 0 */
  IR_OP_MUL,       /* the product modulo 2^64 */
  IR_OP_EQ,        /* 1 when equal, else 0 */
  IR_OP_LE,        /* 1 when left <= right, else 0 */
  IR_OP_ADD_CARRY, /* 1 when the sum is larger than 2^64 - 1, else 0 */
  IR_OP_MUL_CARRY, /* 1 when the product is larger than 2^64 - 1, else 0 */
  IR_OP_OR,        /* bitwise or (the lowering gives it flags only) */
  IR_OP_LT,        /* 1 when left < right, else 0 */
  IR_OP_DIV,       /* the floor of left / right; traps when right is 0 */
  IR_OP_MOD        /* left modulo right; traps when right is 0 */
} IrOp;

/* The prelude functions. Each takes and gives words; a U256 is a block
   address. A new block comes from the allocator. */
typedef enum {
  IR_CALL_U256_ADD, /* a new block a + b, or 0 when the sum is 2^256 or more */
  IR_CALL_U256_SUB, /* a new block a - b, all 0 when b > a (no trap) */
  IR_CALL_U256_LE,  /* 1 when a <= b, else 0 */
  IR_CALL_U256_EQ,  /* 1 when a = b, else 0 */
  IR_CALL_TO_U256,  /* a new block for the Nat n */
  IR_CALL_SHA256,   /* a new block: SHA-256 of the 64 bytes a then b, each
                       big-endian (OP_HASH, eval.c hash_words) */
  IR_CALL_FE_ADD,   /* a new block (a + b) mod m, for a, b < m (field.c fe_add) */
  IR_CALL_FE_SUB,   /* a new block (a - b) mod m, for a, b < m (field.c fe_sub) */
  IR_CALL_FE_MUL,   /* a new block a b mod m (field.c fe_mul) */
  IR_CALL_FE_INV,   /* a new block a^(m - 2) mod m (field.c fe_inv) */
  IR_CALL_EC_ADD,   /* a new pair block p + q on secp256k1 (secp256k1.c group_add); 0 for invalid or trapped coordinates */
  IR_CALL_EC_MUL,   /* a new pair block k p on secp256k1 (secp256k1.c group_mul); 0 for invalid or trapped coordinates */
  IR_CALL_SCHNORR_OK /* 1 when z G = R + c P for the x-only key and R.x (frost.c schnorr_verify), else 0 */
} IrCall;

typedef enum {
  IR_EXPR_CONST,  /* value */
  IR_EXPR_LOCAL,  /* local */
  IR_EXPR_BINARY, /* op left right */
  IR_EXPR_LOAD,   /* word number field of the block at address left */
  IR_EXPR_CALL    /* the prelude function call applied to args */
} IrExprKind;

typedef struct IrExpr IrExpr;
struct IrExpr {
  IrExprKind kind;
  uint64_t value;
  uint32_t local;
  IrOp op;
  uint32_t field;
  const IrExpr *left;
  const IrExpr *right;
  IrCall call;
  const IrExpr *const *args;
  size_t arg_count;
};

typedef struct IrStmt IrStmt;

typedef struct {
  const IrStmt *const *items;
  size_t count;
} IrBlock;

typedef enum {
  IR_STMT_SET,    /* local := expr */
  IR_STMT_ALLOC,  /* local := address of a new block; word k := fields[k] */
  IR_STMT_IF,     /* expr != 0: body, else: otherwise */
  IR_STMT_SWITCH, /* run arms[expr]; a value outside 0 to arm_count - 1 traps */
  IR_STMT_REPEAT, /* counter := expr, then run body counter times */
  IR_STMT_WHILE,  /* counter := expr; run body while counter > 0 and local != 0,
                     counter decreasing by 1 at each run; body sets local */
  IR_STMT_TRAP,   /* stop the call: Wasm unreachable, EVM REVERT */
  IR_STMT_RETURN  /* return expr */
} IrStmtKind;

struct IrStmt {
  IrStmtKind kind;
  uint32_t local;
  uint32_t counter;
  const IrExpr *expr;
  const IrExpr *const *fields;
  size_t field_count;
  IrBlock body;
  IrBlock otherwise;
  const IrBlock *arms;
  size_t arm_count;
};

/* One exported entry. The body block ends in RETURN or TRAP on each path. */
typedef struct {
  const char *name;
  const IrScalar *params;
  size_t param_count;
  IrScalar result;
  uint32_t local_count; /* includes the parameters */
  IrBlock body;
} IrFunc;

typedef struct {
  const IrFunc *funcs;
  size_t func_count;
} IrProgram;

#endif
