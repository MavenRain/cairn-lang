/* The Wasm back end. It writes a Wasm 1.0 (MVP) binary module directly.

   Module layout:
   - Function 0 is the internal allocator `alloc (bytes : i32) -> i64`. It
     bumps the mutable i64 global 0 (the heap pointer) over memory 0. When a
     block does not fit, it grows the memory. When the memory cannot grow (the
     maximum is WASM_MAX_PAGES), it traps.
   - Functions 1 to P are the prelude functions the module calls, in IrCall
     order. Function 1 + P + K is IR entry K, exported as its name. A Nat or
     Flag parameter or result is an i64. A U256 parameter is 4 i64, high limb
     first; the entry copies it to a new block. A U256 result is copied to
     address 0 (4 words, low limb first, as in a block), and the entry returns
     0. Each entry first resets the heap pointer to WASM_HEAP_START, after the
     result slot, and it traps on a Flag argument larger than 1. Memory 0 is
     exported as `$memory`, so a host reads the result slot.
   - A word is 8 bytes. Word K of the block at address A is at A + 8K.

   Each entry has three i64 scratch locals after the IR locals: S and S + 1
   hold the operands of SUB, ADD_CARRY and MUL_CARRY, and S + 2 holds the
   address of an ALLOC. An operator reads its operands into S and S + 1 only
   after both operand expressions are done, so a nested operator cannot
   overwrite them. When an entry has a U256 parameter, IR local I is Wasm
   local W + I (W = the Wasm parameters), and the entry copies each parameter
   in first; otherwise IR local I is Wasm local I.

   Each sized part (a section, a function body) is written in two passes: the
   first pass counts the bytes, the second pass writes them after the size.
   So the back end needs no buffer. */
#include <stdint.h>
#include <string.h>

#include "sha256.h"
#include "target.h"

#define WASM_MAX_PAGES 16384u /* 1 GiB, the arena limit of langc eval */
#define WASM_DEPTH_MAX 2000u
#define WASM_HEAP_START 32u /* after the U256 result slot at address 0 (Q-S8-3) */

enum {
  OP_UNREACHABLE = 0x00,
  OP_BLOCK = 0x02,
  OP_LOOP = 0x03,
  OP_IF = 0x04,
  OP_ELSE = 0x05,
  OP_END = 0x0B,
  OP_BR = 0x0C,
  OP_BR_IF = 0x0D,
  OP_BR_TABLE = 0x0E,
  OP_RETURN = 0x0F,
  OP_CALL = 0x10,
  OP_SELECT = 0x1B,
  OP_LOCAL_GET = 0x20,
  OP_LOCAL_SET = 0x21,
  OP_LOCAL_TEE = 0x22,
  OP_GLOBAL_GET = 0x23,
  OP_GLOBAL_SET = 0x24,
  OP_I64_LOAD = 0x29,
  OP_I64_STORE = 0x37,
  OP_MEMORY_SIZE = 0x3F,
  OP_MEMORY_GROW = 0x40,
  OP_I32_CONST = 0x41,
  OP_I64_CONST = 0x42,
  OP_I32_EQ = 0x46,
  OP_I64_EQZ = 0x50,
  OP_I64_EQ = 0x51,
  OP_I64_NE = 0x52,
  OP_I64_LT_S = 0x53,
  OP_I64_LT_U = 0x54,
  OP_I64_GT_U = 0x56,
  OP_I64_LE_S = 0x57,
  OP_I64_LE_U = 0x58,
  OP_I64_GE_U = 0x5A,
  OP_I32_ADD = 0x6A,
  OP_I32_AND = 0x71,
  OP_I32_OR = 0x72,
  OP_I32_XOR = 0x73,
  OP_I32_SHR_U = 0x76,
  OP_I32_ROTR = 0x78,
  OP_I64_ADD = 0x7C,
  OP_I64_SUB = 0x7D,
  OP_I64_MUL = 0x7E,
  OP_I64_DIV_U = 0x80,
  OP_I64_REM_U = 0x82,
  OP_I64_AND = 0x83,
  OP_I64_OR = 0x84,
  OP_I64_SHL = 0x86,
  OP_I64_SHR_U = 0x88,
  OP_I32_WRAP_I64 = 0xA7,
  OP_I64_EXTEND_I32_U = 0xAD,
  TYPE_I32 = 0x7F,
  TYPE_I64 = 0x7E,
  TYPE_FUNC = 0x60,
  TYPE_EMPTY = 0x40,
  ALIGN_WORD = 3
};

enum {
  SECTION_TYPE = 1,
  SECTION_FUNCTION = 3,
  SECTION_MEMORY = 5,
  SECTION_GLOBAL = 6,
  SECTION_EXPORT = 7,
  SECTION_CODE = 10
};

/* A byte sink. With out == NULL it only counts. */
typedef struct {
  FILE *out;
  size_t count;
  FILE *err;
  uint32_t base;  /* the Wasm local of IR local 0 (part_entry) */
  int wide;       /* the entry gives a U256: RETURN copies it to address 0 */
  unsigned calls; /* the prelude functions the module calls, one bit per IrCall */
} Sink;

typedef int (*Part)(Sink *s, const void *arg);

const char target_name[] = "wasm";

static int put_byte(Sink *s, unsigned v) {
  s->count++;
  return s->out == NULL || fputc((int)(v & 0xFFu), s->out) != EOF;
}

static int put_uleb(Sink *s, uint64_t v) {
  int ok = 1;
  for (; ok && v >= 0x80u; v >>= 7) ok = put_byte(s, (unsigned)(v & 0x7Fu) | 0x80u);
  return ok && put_byte(s, (unsigned)v);
}

/* Signed LEB128 of the two's complement value of V. */
static int put_sleb(Sink *s, uint64_t v) {
  uint64_t fill = (v >> 63) != 0 ? ~(~(uint64_t)0 >> 7) : 0;
  unsigned low = (unsigned)(v & 0x7Fu);
  uint64_t rest = (v >> 7) | fill;
  int last = (rest == 0 && (low & 0x40u) == 0) || (rest == ~(uint64_t)0 && (low & 0x40u) != 0);
  int ok = put_byte(s, last ? low : low | 0x80u);
  return ok && (last || put_sleb(s, rest));
}

static int put_name(Sink *s, const char *name) {
  size_t i;
  size_t len = strlen(name);
  int ok = put_uleb(s, len);
  for (i = 0; i < len && ok; i++) ok = put_byte(s, (unsigned char)name[i]);
  return ok;
}

/* Writes the byte size of PART, then PART. */
static int sized(Sink *s, Part part, const void *arg) {
  Sink count = *s;
  count.out = NULL;
  count.count = 0;
  return part(&count, arg) && put_uleb(s, count.count) && part(s, arg);
}

static int section(Sink *s, unsigned id, Part part, const void *arg) {
  return put_byte(s, id) && sized(s, part, arg);
}

static int op2(Sink *s, unsigned op, uint64_t imm) {
  return put_byte(s, op) && put_uleb(s, imm);
}

static int i64_const(Sink *s, uint64_t v) {
  return put_byte(s, OP_I64_CONST) && put_sleb(s, v);
}

static int i32_const(Sink *s, uint64_t v) {
  return put_byte(s, OP_I32_CONST) && put_sleb(s, v);
}

static int memarg(Sink *s, unsigned op, uint32_t word) {
  return put_byte(s, op) && put_uleb(s, ALIGN_WORD) && put_uleb(s, 8u * (uint64_t)word);
}

static int deep(Sink *s) {
  fprintf(s->err, "langc: WASM_DEPTH: -: the IR nests deeper than %u levels\n", WASM_DEPTH_MAX);
  return 0;
}

static int no_code(Sink *s, const char *what, unsigned v) {
  fprintf(s->err, "langc: WASM_EMIT: -: no Wasm code for the IR %s %u\n", what, v);
  return 0;
}

/* The prelude bodies (A5). The parameters are locals 0 and 1, each the i64
   address of a block (to_u256: the Nat); the body's own locals come next. */

/* The header: N more i64 locals after the parameters. */
static int body_locals(Sink *s, uint64_t n) {
  return put_uleb(s, 1) && put_uleb(s, n) && put_byte(s, TYPE_I64);
}

/* Wasm local DST := a new block. */
static int new_block(Sink *s, uint32_t dst) {
  return i32_const(s, 32) && op2(s, OP_CALL, 0) && op2(s, OP_LOCAL_SET, dst);
}

/* Pushes word WORD of the block at Wasm local BLOCK. */
static int load_word(Sink *s, uint32_t block, uint32_t word) {
  return op2(s, OP_LOCAL_GET, block) && put_byte(s, OP_I32_WRAP_I64) && memarg(s, OP_I64_LOAD, word);
}

/* Word WORD of the block at Wasm local BLOCK := Wasm local V. */
static int store_word(Sink *s, uint32_t block, uint32_t word, uint32_t v) {
  return op2(s, OP_LOCAL_GET, block) && put_byte(s, OP_I32_WRAP_I64) && op2(s, OP_LOCAL_GET, v) &&
         memarg(s, OP_I64_STORE, word);
}

/* Words FROM to 3 of the block at Wasm local BLOCK := 0. */
static int zero_words(Sink *s, uint32_t block, uint32_t from) {
  uint32_t j;
  int ok = 1;
  for (j = from; j < 4u && ok; j++) {
    ok = op2(s, OP_LOCAL_GET, block) && put_byte(s, OP_I32_WRAP_I64) && i64_const(s, 0) &&
         memarg(s, OP_I64_STORE, j);
  }
  return ok;
}

/* u256_cmp: Wasm local C := -1, 0 or 1 when a < b, a = b or a > b. From limb
   3 down, the first limb that differs sets C and leaves the block. X and Y
   hold the two limbs. */
static int cmp_code(Sink *s, uint32_t c, uint32_t x, uint32_t y) {
  uint32_t j;
  int ok = i64_const(s, 0) && op2(s, OP_LOCAL_SET, c) && put_byte(s, OP_BLOCK) && put_byte(s, TYPE_EMPTY);
  for (j = 0; j < 4u && ok; j++) {
    ok = load_word(s, 0, 3u - j) && op2(s, OP_LOCAL_TEE, x) && load_word(s, 1, 3u - j) && op2(s, OP_LOCAL_TEE, y) &&
         put_byte(s, OP_I64_NE) && put_byte(s, OP_IF) && put_byte(s, TYPE_EMPTY) && i64_const(s, ~(uint64_t)0) &&
         i64_const(s, 1) && op2(s, OP_LOCAL_GET, x) && op2(s, OP_LOCAL_GET, y) && put_byte(s, OP_I64_LT_U) &&
         put_byte(s, OP_SELECT) && op2(s, OP_LOCAL_SET, c) && op2(s, OP_BR, 1) && put_byte(s, OP_END);
  }
  return ok && put_byte(s, OP_END);
}

/* u256_add, locals 2 to 6 = r, carry, sum, c1, limb: per limb, sum := a +
   carry, c1 := sum < carry, limb := sum + b, carry := c1 | limb < sum. The
   result is the new block r, or 0 when the last carry is 1. */
static int part_u256_add(Sink *s, const void *arg) {
  uint32_t i;
  int ok;
  (void)arg;
  ok = body_locals(s, 5) && new_block(s, 2) && i64_const(s, 0) && op2(s, OP_LOCAL_SET, 3);
  for (i = 0; i < 4u && ok; i++) {
    ok = load_word(s, 0, i) && op2(s, OP_LOCAL_GET, 3) && put_byte(s, OP_I64_ADD) && op2(s, OP_LOCAL_TEE, 4) &&
         op2(s, OP_LOCAL_GET, 3) && put_byte(s, OP_I64_LT_U) && put_byte(s, OP_I64_EXTEND_I32_U) &&
         op2(s, OP_LOCAL_SET, 5) && op2(s, OP_LOCAL_GET, 4) && load_word(s, 1, i) && put_byte(s, OP_I64_ADD) &&
         op2(s, OP_LOCAL_SET, 6) && store_word(s, 2, i, 6) && op2(s, OP_LOCAL_GET, 5) && op2(s, OP_LOCAL_GET, 6) &&
         op2(s, OP_LOCAL_GET, 4) && put_byte(s, OP_I64_LT_U) && put_byte(s, OP_I64_EXTEND_I32_U) &&
         put_byte(s, OP_I64_OR) && op2(s, OP_LOCAL_SET, 3);
  }
  return ok && op2(s, OP_LOCAL_GET, 2) && i64_const(s, 0) && op2(s, OP_LOCAL_GET, 3) && put_byte(s, OP_I64_EQZ) &&
         put_byte(s, OP_SELECT) && put_byte(s, OP_END);
}

/* u256_sub, locals 2 to 7 = c, x, y, r, borrow, d: r := a new block, all 0
   when u256_cmp gives a < b, else sub_wrap: per limb, d := x - y, b1 := x < y,
   r := d - borrow, borrow := b1 | d < borrow. The result is r (no trap). */
static int part_u256_sub(Sink *s, const void *arg) {
  uint32_t i;
  int ok;
  (void)arg;
  ok = body_locals(s, 6) && cmp_code(s, 2, 3, 4) && new_block(s, 5) && op2(s, OP_LOCAL_GET, 2) && i64_const(s, 0) &&
       put_byte(s, OP_I64_LT_S) && put_byte(s, OP_IF) && put_byte(s, TYPE_EMPTY) && zero_words(s, 5, 0) &&
       put_byte(s, OP_ELSE) && i64_const(s, 0) && op2(s, OP_LOCAL_SET, 6);
  for (i = 0; i < 4u && ok; i++) {
    ok = load_word(s, 0, i) && op2(s, OP_LOCAL_TEE, 3) && load_word(s, 1, i) && op2(s, OP_LOCAL_TEE, 4) &&
         put_byte(s, OP_I64_SUB) && op2(s, OP_LOCAL_SET, 7) && op2(s, OP_LOCAL_GET, 5) &&
         put_byte(s, OP_I32_WRAP_I64) && op2(s, OP_LOCAL_GET, 7) && op2(s, OP_LOCAL_GET, 6) &&
         put_byte(s, OP_I64_SUB) && memarg(s, OP_I64_STORE, i) && op2(s, OP_LOCAL_GET, 3) &&
         op2(s, OP_LOCAL_GET, 4) && put_byte(s, OP_I64_LT_U) && put_byte(s, OP_I64_EXTEND_I32_U) &&
         op2(s, OP_LOCAL_GET, 7) && op2(s, OP_LOCAL_GET, 6) && put_byte(s, OP_I64_LT_U) &&
         put_byte(s, OP_I64_EXTEND_I32_U) && put_byte(s, OP_I64_OR) && op2(s, OP_LOCAL_SET, 6);
  }
  return ok && put_byte(s, OP_END) && op2(s, OP_LOCAL_GET, 5) && put_byte(s, OP_END);
}

/* u256_le: 1 when u256_cmp gives a <= b, else 0; locals 2 to 4 = c, x, y. */
static int part_u256_le(Sink *s, const void *arg) {
  (void)arg;
  return body_locals(s, 3) && cmp_code(s, 2, 3, 4) && op2(s, OP_LOCAL_GET, 2) && i64_const(s, 0) &&
         put_byte(s, OP_I64_LE_S) && put_byte(s, OP_I64_EXTEND_I32_U) && put_byte(s, OP_END);
}

/* u256_eq: 1 when u256_cmp gives a = b, else 0; locals 2 to 4 = c, x, y. */
static int part_u256_eq(Sink *s, const void *arg) {
  (void)arg;
  return body_locals(s, 3) && cmp_code(s, 2, 3, 4) && op2(s, OP_LOCAL_GET, 2) && put_byte(s, OP_I64_EQZ) &&
         put_byte(s, OP_I64_EXTEND_I32_U) && put_byte(s, OP_END);
}

/* to_u256 (u256_from_u64), local 1 = r: r := a new block of 0, word 0 := the
   Nat n. The result is r. */
static int part_to_u256(Sink *s, const void *arg) {
  (void)arg;
  return body_locals(s, 1) && new_block(s, 1) && zero_words(s, 1, 0) && store_word(s, 1, 0, 0) &&
         op2(s, OP_LOCAL_GET, 1) && put_byte(s, OP_END);
}

/* part_sha256 locals after a, b, r and q (i64): H the state, V the working
   words a..h, W the message schedule, then T1 and T2 (i32). */
enum { SHA_H = 4, SHA_V = 12, SHA_W = 20, SHA_T1 = 84, SHA_T2 = 85, SHA_I32 = 82 };

/* An i32.const of the 32-bit word V (the immediate is signed). */
static int i32_word(Sink *s, uint32_t v) {
  return i32_const(s, (v & 0x80000000u) != 0u ? (uint64_t)v | 0xFFFFFFFF00000000u : (uint64_t)v);
}

/* rotate(x, R1) ^ rotate(x, R2) ^ (x LAST R3) of sha256.c compress, x =
   Wasm local X: LAST is i32.rotr (the round sums) or i32.shr_u (the
   schedule sums). */
static int sigma(Sink *s, uint32_t x, unsigned r1, unsigned r2, unsigned r3, unsigned last) {
  return op2(s, OP_LOCAL_GET, x) && i32_const(s, r1) && put_byte(s, OP_I32_ROTR) && op2(s, OP_LOCAL_GET, x) &&
         i32_const(s, r2) && put_byte(s, OP_I32_ROTR) && put_byte(s, OP_I32_XOR) && op2(s, OP_LOCAL_GET, x) &&
         i32_const(s, r3) && put_byte(s, last) && put_byte(s, OP_I32_XOR);
}

/* W[M + 2k] and W[M + 2k + 1] := the high and low halves of word 3 - k of
   the block at Wasm local BLOCK (sha256_words: limb 3 first, big endian). */
static int message_words(Sink *s, uint32_t block, uint32_t m) {
  uint32_t k;
  int ok = 1;
  for (k = 0; k < 4u && ok; k++) {
    ok = load_word(s, block, 3u - k) && op2(s, OP_LOCAL_TEE, 3) && i64_const(s, 32) && put_byte(s, OP_I64_SHR_U) &&
         put_byte(s, OP_I32_WRAP_I64) && op2(s, OP_LOCAL_SET, SHA_W + m + 2u * k) && op2(s, OP_LOCAL_GET, 3) &&
         put_byte(s, OP_I32_WRAP_I64) && op2(s, OP_LOCAL_SET, SHA_W + m + 2u * k + 1u);
  }
  return ok;
}

/* Round T of compress: T1 := h + Sigma1(e) + ch + K[T] + W[T], T2 :=
   Sigma0(a) + maj, then h..a := g, f, e, d + T1, c, b, a, T1 + T2. */
static int round_code(Sink *s, uint32_t t) {
  uint32_t i;
  int ok = op2(s, OP_LOCAL_GET, SHA_V + 7u) && sigma(s, SHA_V + 4u, 6, 11, 25, OP_I32_ROTR) &&
           put_byte(s, OP_I32_ADD) && op2(s, OP_LOCAL_GET, SHA_V + 4u) && op2(s, OP_LOCAL_GET, SHA_V + 5u) &&
           put_byte(s, OP_I32_AND) && op2(s, OP_LOCAL_GET, SHA_V + 4u) && i32_word(s, 0xFFFFFFFFu) &&
           put_byte(s, OP_I32_XOR) && op2(s, OP_LOCAL_GET, SHA_V + 6u) && put_byte(s, OP_I32_AND) &&
           put_byte(s, OP_I32_XOR) && put_byte(s, OP_I32_ADD) && i32_word(s, sha256_round_constant[t]) &&
           put_byte(s, OP_I32_ADD) && op2(s, OP_LOCAL_GET, SHA_W + t) && put_byte(s, OP_I32_ADD) &&
           op2(s, OP_LOCAL_SET, SHA_T1) && sigma(s, SHA_V, 2, 13, 22, OP_I32_ROTR) && op2(s, OP_LOCAL_GET, SHA_V) &&
           op2(s, OP_LOCAL_GET, SHA_V + 1u) && put_byte(s, OP_I32_AND) && op2(s, OP_LOCAL_GET, SHA_V) &&
           op2(s, OP_LOCAL_GET, SHA_V + 2u) && put_byte(s, OP_I32_AND) && put_byte(s, OP_I32_XOR) &&
           op2(s, OP_LOCAL_GET, SHA_V + 1u) && op2(s, OP_LOCAL_GET, SHA_V + 2u) && put_byte(s, OP_I32_AND) &&
           put_byte(s, OP_I32_XOR) && put_byte(s, OP_I32_ADD) && op2(s, OP_LOCAL_SET, SHA_T2);
  for (i = 7; i > 0u && ok; i--) {
    if (i == 4u) {
      ok = op2(s, OP_LOCAL_GET, SHA_V + 3u) && op2(s, OP_LOCAL_GET, SHA_T1) && put_byte(s, OP_I32_ADD) &&
           op2(s, OP_LOCAL_SET, SHA_V + 4u);
    } else {
      ok = op2(s, OP_LOCAL_GET, SHA_V + i - 1u) && op2(s, OP_LOCAL_SET, SHA_V + i);
    }
  }
  return ok && op2(s, OP_LOCAL_GET, SHA_T1) && op2(s, OP_LOCAL_GET, SHA_T2) && put_byte(s, OP_I32_ADD) &&
         op2(s, OP_LOCAL_SET, SHA_V);
}

/* compress (sha256.c): W[16..63] from W[0..15], V := H, the 64 rounds, then
   H := H + V. */
static int compress_code(Sink *s) {
  uint32_t t;
  int ok = 1;
  for (t = 16; t < 64u && ok; t++) {
    ok = sigma(s, SHA_W + t - 2u, 17, 19, 10, OP_I32_SHR_U) && op2(s, OP_LOCAL_GET, SHA_W + t - 7u) &&
         put_byte(s, OP_I32_ADD) && sigma(s, SHA_W + t - 15u, 7, 18, 3, OP_I32_SHR_U) && put_byte(s, OP_I32_ADD) &&
         op2(s, OP_LOCAL_GET, SHA_W + t - 16u) && put_byte(s, OP_I32_ADD) && op2(s, OP_LOCAL_SET, SHA_W + t);
  }
  for (t = 0; t < 8u && ok; t++) {
    ok = op2(s, OP_LOCAL_GET, SHA_H + t) && op2(s, OP_LOCAL_SET, SHA_V + t);
  }
  for (t = 0; t < 64u && ok; t++) {
    ok = round_code(s, t);
  }
  for (t = 0; t < 8u && ok; t++) {
    ok = op2(s, OP_LOCAL_GET, SHA_H + t) && op2(s, OP_LOCAL_GET, SHA_V + t) && put_byte(s, OP_I32_ADD) &&
         op2(s, OP_LOCAL_SET, SHA_H + t);
  }
  return ok;
}

/* sha256 (sha256_words), locals 0 = a, 1 = b, 2 = r, 3 = q, then the SHA_
   locals: H := H(0); the 64 bytes of a then b are block 1 and the pad (0x80,
   zero bytes, the length 512 bits) is block 2; r := a new block with word
   3 - k := H[2k] H[2k + 1] (the digest, big endian). */
static int part_sha256(Sink *s, const void *arg) {
  uint32_t i;
  int ok;
  (void)arg;
  ok = put_uleb(s, 2) && put_uleb(s, 2) && put_byte(s, TYPE_I64) && put_uleb(s, SHA_I32) && put_byte(s, TYPE_I32);
  for (i = 0; i < 8u && ok; i++) {
    ok = i32_word(s, sha256_initial_hash[i]) && op2(s, OP_LOCAL_SET, SHA_H + i);
  }
  ok = ok && message_words(s, 0, 0) && message_words(s, 1, 8) && compress_code(s) && i32_word(s, 0x80000000u) &&
       op2(s, OP_LOCAL_SET, SHA_W) && i32_word(s, 512) && op2(s, OP_LOCAL_SET, SHA_W + 15u);
  for (i = 1; i < 15u && ok; i++) {
    ok = i32_word(s, 0) && op2(s, OP_LOCAL_SET, SHA_W + i);
  }
  ok = ok && compress_code(s) && new_block(s, 2);
  for (i = 0; i < 4u && ok; i++) {
    ok = op2(s, OP_LOCAL_GET, SHA_H + 2u * i) && put_byte(s, OP_I64_EXTEND_I32_U) && i64_const(s, 32) &&
         put_byte(s, OP_I64_SHL) && op2(s, OP_LOCAL_GET, SHA_H + 2u * i + 1u) && put_byte(s, OP_I64_EXTEND_I32_U) &&
         put_byte(s, OP_I64_OR) && op2(s, OP_LOCAL_SET, 3) && store_word(s, 2, 3u - i, 3);
  }
  return ok && op2(s, OP_LOCAL_GET, 2) && put_byte(s, OP_END);
}

/* The field prelude (S8 part B1): ports of src/front/field.c over a modulus
   block m, the last parameter. A field element is a U256 block below m. The
   code keeps words in i64 locals; the steps are the field.c steps on 64-bit
   words in place of u32 limbs (fe_mul keeps u32 limbs for the product). */
static uint64_t call_index(const Sink *s, IrCall c);

/* Wasm locals DST to DST + 3 := the words of the block at Wasm local BLOCK. */
static int get_words(Sink *s, uint32_t block, uint32_t dst) {
  uint32_t k;
  int ok = 1;
  for (k = 0; k < 4u && ok; k++) ok = load_word(s, block, k) && op2(s, OP_LOCAL_SET, dst + k);
  return ok;
}

/* The words of the block at Wasm local BLOCK := Wasm locals SRC to SRC + 3. */
static int put_words(Sink *s, uint32_t block, uint32_t src) {
  uint32_t k;
  int ok = 1;
  for (k = 0; k < 4u && ok; k++) ok = store_word(s, block, k, src + k);
  return ok;
}

/* add_limbs: OUT := X + Y and C := the carry out, with the u256_add steps (T
   and C1 are scratch). OUT can be X or Y. */
static int add_code(Sink *s, uint32_t out, uint32_t x, uint32_t y, uint32_t c, uint32_t t, uint32_t c1) {
  uint32_t k;
  int ok = i64_const(s, 0) && op2(s, OP_LOCAL_SET, c);
  for (k = 0; k < 4u && ok; k++) {
    ok = op2(s, OP_LOCAL_GET, x + k) && op2(s, OP_LOCAL_GET, c) && put_byte(s, OP_I64_ADD) && op2(s, OP_LOCAL_TEE, t) &&
         op2(s, OP_LOCAL_GET, c) && put_byte(s, OP_I64_LT_U) && put_byte(s, OP_I64_EXTEND_I32_U) &&
         op2(s, OP_LOCAL_SET, c1) && op2(s, OP_LOCAL_GET, t) && op2(s, OP_LOCAL_GET, y + k) && put_byte(s, OP_I64_ADD) &&
         op2(s, OP_LOCAL_TEE, out + k) && op2(s, OP_LOCAL_GET, t) && put_byte(s, OP_I64_LT_U) &&
         put_byte(s, OP_I64_EXTEND_I32_U) && op2(s, OP_LOCAL_GET, c1) && put_byte(s, OP_I64_OR) &&
         op2(s, OP_LOCAL_SET, c);
  }
  return ok;
}

/* sub_limbs: OUT := X - Y and B := the borrow out, with the u256_sub steps (T
   and B1 are scratch). OUT can be X or Y. */
static int sub_code(Sink *s, uint32_t out, uint32_t x, uint32_t y, uint32_t b, uint32_t t, uint32_t b1) {
  uint32_t k;
  int ok = i64_const(s, 0) && op2(s, OP_LOCAL_SET, b);
  for (k = 0; k < 4u && ok; k++) {
    ok = op2(s, OP_LOCAL_GET, x + k) && op2(s, OP_LOCAL_GET, y + k) && put_byte(s, OP_I64_LT_U) &&
         put_byte(s, OP_I64_EXTEND_I32_U) && op2(s, OP_LOCAL_SET, b1) && op2(s, OP_LOCAL_GET, x + k) &&
         op2(s, OP_LOCAL_GET, y + k) && put_byte(s, OP_I64_SUB) && op2(s, OP_LOCAL_TEE, t) && op2(s, OP_LOCAL_GET, b) &&
         put_byte(s, OP_I64_SUB) && op2(s, OP_LOCAL_SET, out + k) && op2(s, OP_LOCAL_GET, t) &&
         op2(s, OP_LOCAL_GET, b) && put_byte(s, OP_I64_LT_U) && put_byte(s, OP_I64_EXTEND_I32_U) &&
         op2(s, OP_LOCAL_GET, b1) && put_byte(s, OP_I64_OR) && op2(s, OP_LOCAL_SET, b);
  }
  return ok;
}

/* OUT := X when Wasm local C is 1, else Y (C holds 0 or 1). */
static int sel_code(Sink *s, uint32_t out, uint32_t x, uint32_t y, uint32_t c) {
  uint32_t k;
  int ok = 1;
  for (k = 0; k < 4u && ok; k++) {
    ok = op2(s, OP_LOCAL_GET, x + k) && op2(s, OP_LOCAL_GET, y + k) && op2(s, OP_LOCAL_GET, c) &&
         put_byte(s, OP_I32_WRAP_I64) && put_byte(s, OP_SELECT) && op2(s, OP_LOCAL_SET, out + k);
  }
  return ok;
}

/* Wasm local C := C | (B = 0), for the "carry out or no borrow" tests. */
static int or_no_borrow(Sink *s, uint32_t dst, uint32_t c, uint32_t b) {
  return op2(s, OP_LOCAL_GET, c) && op2(s, OP_LOCAL_GET, b) && put_byte(s, OP_I64_EQZ) &&
         put_byte(s, OP_I64_EXTEND_I32_U) && put_byte(s, OP_I64_OR) && op2(s, OP_LOCAL_SET, dst);
}

/* fe_add and fe_sub: parameters a, b, m; locals 3 to 24 = r, X (4 words), Y,
   M, D, C, T, C1, B, K. */
enum { FE_R = 3, FE_X = 4, FE_Y = 8, FE_M = 12, FE_D = 16, FE_C = 20, FE_T = 21, FE_C1 = 22, FE_B = 23, FE_K = 24 };

/* fe_add: X := a + b with the carry C, D := X - m with the borrow B, the
   result r := D when C = 1 or B = 0 (the sum is m or more), else X. */
static int part_fe_add(Sink *s, const void *arg) {
  (void)arg;
  return body_locals(s, 22) && get_words(s, 0, FE_X) && get_words(s, 1, FE_Y) && get_words(s, 2, FE_M) &&
         add_code(s, FE_X, FE_X, FE_Y, FE_C, FE_T, FE_C1) && sub_code(s, FE_D, FE_X, FE_M, FE_B, FE_T, FE_C1) &&
         or_no_borrow(s, FE_K, FE_C, FE_B) && sel_code(s, FE_X, FE_D, FE_X, FE_K) && new_block(s, FE_R) &&
         put_words(s, FE_R, FE_X) && op2(s, OP_LOCAL_GET, FE_R) && put_byte(s, OP_END);
}

/* fe_sub: X := a - b with the borrow B, D := X + m, the result r := D when
   B = 1, else X. */
static int part_fe_sub(Sink *s, const void *arg) {
  (void)arg;
  return body_locals(s, 22) && get_words(s, 0, FE_X) && get_words(s, 1, FE_Y) && get_words(s, 2, FE_M) &&
         sub_code(s, FE_X, FE_X, FE_Y, FE_B, FE_T, FE_C1) && add_code(s, FE_D, FE_X, FE_M, FE_C, FE_T, FE_C1) &&
         sel_code(s, FE_X, FE_D, FE_X, FE_B) && new_block(s, FE_R) && put_words(s, FE_R, FE_X) &&
         op2(s, OP_LOCAL_GET, FE_R) && put_byte(s, OP_END);
}

/* Wasm locals DST to DST + 7 := the u32 limbs of the block at Wasm local
   BLOCK, least significant first: limb k is word k / 2 shifted right by 32
   (k % 2). */
static int get_limbs(Sink *s, uint32_t block, uint32_t dst) {
  uint32_t k;
  int ok = 1;
  for (k = 0; k < 8u && ok; k++) {
    ok = load_word(s, block, k / 2u) && i64_const(s, 32u * (k % 2u)) && put_byte(s, OP_I64_SHR_U) &&
         i64_const(s, 0xFFFFFFFFu) && put_byte(s, OP_I64_AND) && op2(s, OP_LOCAL_SET, dst + k);
  }
  return ok;
}

/* fe_mul: parameters a, b, m; locals 3 to 56 = r, q, A (8 limbs), B (8
   limbs), P (16 limbs), carry, T, X (4 words), M, D, BW, T2, B1, top, i, K. */
enum {
  FM_R = 3, FM_Q = 4, FM_A = 5, FM_B = 13, FM_P = 21, FM_CY = 37, FM_T = 38, FM_X = 39,
  FM_M = 43, FM_D = 47, FM_BW = 51, FM_T2 = 52, FM_B1 = 53, FM_TOP = 54, FM_I = 55, FM_K = 56
};

/* fe_mul (field.c 100-131): the 16-limb schoolbook product P := a b (each t
   = a_i b_j + p_(i+j) + carry fits in 64 bits), stored in the 64-byte block
   q; then a loop of 512 steps from the top bit i of P: top := X bit 255, X
   := X << 1 | bit i, X := X - m when top = 1 or X >= m (no borrow). */
static int part_fe_mul(Sink *s, const void *arg) {
  uint32_t i;
  uint32_t j;
  int ok;
  (void)arg;
  ok = body_locals(s, 54) && get_limbs(s, 0, FM_A) && get_limbs(s, 1, FM_B) && get_words(s, 2, FM_M);
  for (i = 0; i < 16u && ok; i++) ok = i64_const(s, 0) && op2(s, OP_LOCAL_SET, FM_P + i);
  for (i = 0; i < 8u && ok; i++) {
    ok = i64_const(s, 0) && op2(s, OP_LOCAL_SET, FM_CY);
    for (j = 0; j < 8u && ok; j++) {
      ok = op2(s, OP_LOCAL_GET, FM_A + i) && op2(s, OP_LOCAL_GET, FM_B + j) && put_byte(s, OP_I64_MUL) &&
           op2(s, OP_LOCAL_GET, FM_P + i + j) && put_byte(s, OP_I64_ADD) && op2(s, OP_LOCAL_GET, FM_CY) &&
           put_byte(s, OP_I64_ADD) && op2(s, OP_LOCAL_TEE, FM_T) && i64_const(s, 0xFFFFFFFFu) &&
           put_byte(s, OP_I64_AND) && op2(s, OP_LOCAL_SET, FM_P + i + j) && op2(s, OP_LOCAL_GET, FM_T) &&
           i64_const(s, 32) && put_byte(s, OP_I64_SHR_U) && op2(s, OP_LOCAL_SET, FM_CY);
    }
    ok = ok && op2(s, OP_LOCAL_GET, FM_CY) && op2(s, OP_LOCAL_SET, FM_P + i + 8u);
  }
  ok = ok && i32_const(s, 64) && op2(s, OP_CALL, 0) && op2(s, OP_LOCAL_SET, FM_Q);
  for (i = 0; i < 8u && ok; i++) {
    ok = op2(s, OP_LOCAL_GET, FM_Q) && put_byte(s, OP_I32_WRAP_I64) && op2(s, OP_LOCAL_GET, FM_P + 2u * i) &&
         op2(s, OP_LOCAL_GET, FM_P + 2u * i + 1u) && i64_const(s, 32) && put_byte(s, OP_I64_SHL) &&
         put_byte(s, OP_I64_OR) && memarg(s, OP_I64_STORE, i);
  }
  for (i = 0; i < 4u && ok; i++) ok = i64_const(s, 0) && op2(s, OP_LOCAL_SET, FM_X + i);
  ok = ok && i64_const(s, 512) && op2(s, OP_LOCAL_SET, FM_I) && put_byte(s, OP_BLOCK) && put_byte(s, TYPE_EMPTY) &&
       put_byte(s, OP_LOOP) && put_byte(s, TYPE_EMPTY) && op2(s, OP_LOCAL_GET, FM_I) && put_byte(s, OP_I64_EQZ) &&
       op2(s, OP_BR_IF, 1) && op2(s, OP_LOCAL_GET, FM_I) && i64_const(s, 1) && put_byte(s, OP_I64_SUB) &&
       op2(s, OP_LOCAL_SET, FM_I) && op2(s, OP_LOCAL_GET, FM_X + 3u) && i64_const(s, 63) &&
       put_byte(s, OP_I64_SHR_U) && op2(s, OP_LOCAL_SET, FM_TOP);
  for (i = 3u; i > 0u && ok; i--) {
    ok = op2(s, OP_LOCAL_GET, FM_X + i) && i64_const(s, 1) && put_byte(s, OP_I64_SHL) &&
         op2(s, OP_LOCAL_GET, FM_X + i - 1u) && i64_const(s, 63) && put_byte(s, OP_I64_SHR_U) &&
         put_byte(s, OP_I64_OR) && op2(s, OP_LOCAL_SET, FM_X + i);
  }
  return ok && op2(s, OP_LOCAL_GET, FM_X) && i64_const(s, 1) && put_byte(s, OP_I64_SHL) &&
         op2(s, OP_LOCAL_GET, FM_Q) && op2(s, OP_LOCAL_GET, FM_I) && i64_const(s, 6) && put_byte(s, OP_I64_SHR_U) &&
         i64_const(s, 3) && put_byte(s, OP_I64_SHL) && put_byte(s, OP_I64_ADD) && put_byte(s, OP_I32_WRAP_I64) &&
         memarg(s, OP_I64_LOAD, 0) && op2(s, OP_LOCAL_GET, FM_I) && put_byte(s, OP_I64_SHR_U) && i64_const(s, 1) &&
         put_byte(s, OP_I64_AND) && put_byte(s, OP_I64_OR) && op2(s, OP_LOCAL_SET, FM_X) &&
         sub_code(s, FM_D, FM_X, FM_M, FM_BW, FM_T2, FM_B1) && or_no_borrow(s, FM_K, FM_TOP, FM_BW) &&
         sel_code(s, FM_X, FM_D, FM_X, FM_K) && op2(s, OP_BR, 0) && put_byte(s, OP_END) && put_byte(s, OP_END) &&
         new_block(s, FM_R) && put_words(s, FM_R, FM_X) && op2(s, OP_LOCAL_GET, FM_R) && put_byte(s, OP_END);
}

/* fe_inv (field.c 134-145, pow_limbs with e = m - 2): parameters a, m;
   locals 2 to 4 = acc, w, j. acc := 1, then per bit of e from the top, acc
   := acc acc, and acc := acc a on a 1 bit. Word k of e is word k of m, less
   2 for word 0 (word 0 of both moduli is above 2). */
static int part_fe_inv(Sink *s, const void *arg) {
  uint32_t k;
  uint64_t mul = call_index(s, IR_CALL_FE_MUL);
  int ok;
  (void)arg;
  ok = body_locals(s, 3) && new_block(s, 2) && i64_const(s, 1) && op2(s, OP_LOCAL_SET, 3) && store_word(s, 2, 0, 3) &&
       zero_words(s, 2, 1);
  for (k = 4u; k > 0u && ok; k--) {
    ok = load_word(s, 1, k - 1u) && (k != 1u || (i64_const(s, 2) && put_byte(s, OP_I64_SUB))) &&
         op2(s, OP_LOCAL_SET, 3) && i64_const(s, 64) && op2(s, OP_LOCAL_SET, 4) && put_byte(s, OP_BLOCK) &&
         put_byte(s, TYPE_EMPTY) && put_byte(s, OP_LOOP) && put_byte(s, TYPE_EMPTY) && op2(s, OP_LOCAL_GET, 4) &&
         put_byte(s, OP_I64_EQZ) && op2(s, OP_BR_IF, 1) && op2(s, OP_LOCAL_GET, 4) && i64_const(s, 1) &&
         put_byte(s, OP_I64_SUB) && op2(s, OP_LOCAL_SET, 4) && op2(s, OP_LOCAL_GET, 2) && op2(s, OP_LOCAL_GET, 2) &&
         op2(s, OP_LOCAL_GET, 1) && op2(s, OP_CALL, mul) && op2(s, OP_LOCAL_SET, 2) && op2(s, OP_LOCAL_GET, 3) &&
         op2(s, OP_LOCAL_GET, 4) && put_byte(s, OP_I64_SHR_U) && i64_const(s, 1) && put_byte(s, OP_I64_AND) &&
         put_byte(s, OP_I32_WRAP_I64) && put_byte(s, OP_IF) && put_byte(s, TYPE_EMPTY) && op2(s, OP_LOCAL_GET, 2) &&
         op2(s, OP_LOCAL_GET, 0) && op2(s, OP_LOCAL_GET, 1) && op2(s, OP_CALL, mul) && op2(s, OP_LOCAL_SET, 2) &&
         put_byte(s, OP_END) && op2(s, OP_BR, 0) && put_byte(s, OP_END) && put_byte(s, OP_END);
  }
  return ok && op2(s, OP_LOCAL_GET, 2) && put_byte(s, OP_END);
}

/* ec_add and ec_mul (src/front/secp256k1.c): parameters p and q (ec_add) or
   p and k (ec_mul); locals 2 to 23 = the modulus block M, the scratch block
   W, the affine AX, AY, BX, BY, the slope L, T, U, the flag C, the result R,
   the Jacobian X, Y, Z, the scratch YY, S, MM, H, HH, HHH, V and the bit
   counter I. A point is a pair block [0, x, 0, y, 0] (lower.c block_ops: a
   field is a word and its flag). */
enum {
  EC_M = 2, EC_W = 3, EC_AX = 4, EC_AY = 5, EC_BX = 6, EC_BY = 7, EC_L = 8, EC_T = 9, EC_U = 10, EC_C = 11, EC_R = 12,
  EC_X = 13, EC_Y = 14, EC_Z = 15, EC_YY = 16, EC_S = 17, EC_MM = 18, EC_H = 19, EC_HH = 20, EC_HHH = 21, EC_V = 22,
  EC_I = 23
};

/* The fe_* bits that ec_add and ec_mul call. */
#define EC_NEEDS ((1u << IR_CALL_FE_ADD) | (1u << IR_CALL_FE_SUB) | (1u << IR_CALL_FE_MUL) | (1u << IR_CALL_FE_INV))

/* The words of p, 0, 1 and 7, low word first. */
static const uint64_t ec_p[4] = {0xFFFFFFFEFFFFFC2Fu, ~(uint64_t)0, ~(uint64_t)0, ~(uint64_t)0};
static const uint64_t ec_zero[4] = {0u, 0u, 0u, 0u};
static const uint64_t ec_one[4] = {1u, 0u, 0u, 0u};
static const uint64_t ec_seven[4] = {7u, 0u, 0u, 0u};

/* Wasm local DST := a new block with the words W[0] to W[3]. */
static int const_block(Sink *s, uint32_t dst, const uint64_t *w) {
  uint32_t k;
  int ok = new_block(s, dst);
  for (k = 0; k < 4u && ok; k++)
    ok = op2(s, OP_LOCAL_GET, dst) && put_byte(s, OP_I32_WRAP_I64) && i64_const(s, w[k]) && memarg(s, OP_I64_STORE, k);
  return ok;
}

/* Wasm local DST := the prelude call C (fe_add, fe_sub or fe_mul) of the
   blocks at Wasm locals X and Y, mod the block at EC_M. */
static int fe_code(Sink *s, IrCall c, uint32_t dst, uint32_t x, uint32_t y) {
  return op2(s, OP_LOCAL_GET, x) && op2(s, OP_LOCAL_GET, y) && op2(s, OP_LOCAL_GET, EC_M) &&
         op2(s, OP_CALL, call_index(s, c)) && op2(s, OP_LOCAL_SET, dst);
}

static int fadd_code(Sink *s, uint32_t dst, uint32_t x, uint32_t y) {
  return fe_code(s, IR_CALL_FE_ADD, dst, x, y);
}

static int fsub_code(Sink *s, uint32_t dst, uint32_t x, uint32_t y) {
  return fe_code(s, IR_CALL_FE_SUB, dst, x, y);
}

static int fmul_code(Sink *s, uint32_t dst, uint32_t x, uint32_t y) {
  return fe_code(s, IR_CALL_FE_MUL, dst, x, y);
}

/* Wasm local DST := fe_inv of the block at Wasm local X, mod EC_M. */
static int finv_code(Sink *s, uint32_t dst, uint32_t x) {
  return op2(s, OP_LOCAL_GET, x) && op2(s, OP_LOCAL_GET, EC_M) && op2(s, OP_CALL, call_index(s, IR_CALL_FE_INV)) &&
         op2(s, OP_LOCAL_SET, dst);
}

/* Wasm local DST := Wasm local SRC. */
static int copy_local(Sink *s, uint32_t dst, uint32_t src) {
  return op2(s, OP_LOCAL_GET, src) && op2(s, OP_LOCAL_SET, dst);
}

/* The Jacobian X, Y, Z := Wasm locals A, B, C. */
static int set_jac(Sink *s, uint32_t a, uint32_t b, uint32_t c) {
  return copy_local(s, EC_X, a) && copy_local(s, EC_Y, b) && copy_local(s, EC_Z, c);
}

/* Opens an `if` with no result; the i32 test is on the stack. */
static int if_code(Sink *s) {
  return put_byte(s, OP_IF) && put_byte(s, TYPE_EMPTY);
}

/* Pushes the i32 1 when the blocks at Wasm locals X and Y hold the same
   words, else 0. */
static int push_eq(Sink *s, uint32_t x, uint32_t y) {
  uint32_t k;
  int ok = 1;
  for (k = 0; k < 4u && ok; k++)
    ok = load_word(s, x, k) && load_word(s, y, k) && put_byte(s, OP_I64_EQ) && (k == 0u || put_byte(s, OP_I32_AND));
  return ok;
}

/* Pushes the i32 1 when the block at Wasm local X is 0, else 0. */
static int push_zero(Sink *s, uint32_t x) {
  uint32_t k;
  int ok = load_word(s, x, 0);
  for (k = 1; k < 4u && ok; k++) ok = load_word(s, x, k) && put_byte(s, OP_I64_OR);
  return ok && put_byte(s, OP_I64_EQZ);
}

/* Pushes the i32 1 when the point at Wasm locals X, Y is (0, 0), the point
   at infinity (secp256k1.c is_infinity). */
static int push_inf(Sink *s, uint32_t x, uint32_t y) {
  return push_zero(s, x) && push_zero(s, y) && put_byte(s, OP_I32_AND);
}

/* Pushes the i32 1 when the block at Wasm local X is p or more: words 1 to 3
   of p are all 1 bits, so X >= p when they are and word 0 >= word 0 of p. */
static int push_big(Sink *s, uint32_t x) {
  return load_word(s, x, 1) && load_word(s, x, 2) && put_byte(s, OP_I64_AND) && load_word(s, x, 3) &&
         put_byte(s, OP_I64_AND) && i64_const(s, ~(uint64_t)0) && put_byte(s, OP_I64_EQ) && load_word(s, x, 0) &&
         i64_const(s, ec_p[0]) && put_byte(s, OP_I64_GE_U) && put_byte(s, OP_I32_AND);
}

/* group_from_words: returns the null block when a coordinate of the point at
   Wasm locals X, Y is p or more, or when the point is not (0, 0) and y^2 is
   not x^3 + 7 (eval.c ec_op: overflow). Uses T, U and W. */
static int point_code(Sink *s, uint32_t x, uint32_t y) {
  return push_big(s, x) && push_big(s, y) && put_byte(s, OP_I32_OR) && if_code(s) && i64_const(s, 0) && put_byte(s, OP_RETURN) &&
         put_byte(s, OP_END) && fmul_code(s, EC_T, y, y) && fmul_code(s, EC_U, x, x) && fmul_code(s, EC_U, EC_U, x) &&
         const_block(s, EC_W, ec_seven) && fadd_code(s, EC_U, EC_U, EC_W) && push_eq(s, EC_T, EC_U) &&
         push_inf(s, x, y) && put_byte(s, OP_I32_OR) && if_code(s) && put_byte(s, OP_ELSE) &&
         i64_const(s, 0) && put_byte(s, OP_RETURN) && put_byte(s, OP_END);
}

/* A trapped coordinate returns the null block too. The lowering turns that
   result into a lazy trap flag, so an unused curve result does not trap. */
static int point_flags(Sink *s, uint32_t p) {
  return load_word(s, p, 2) && load_word(s, p, 4) && put_byte(s, OP_I64_OR) &&
         put_byte(s, OP_I32_WRAP_I64) && if_code(s) && i64_const(s, 0) &&
         put_byte(s, OP_RETURN) && put_byte(s, OP_END);
}

/* R := a new pair block [0, X, 0, Y, 0]. */
static int pair_code(Sink *s, uint32_t x, uint32_t y) {
  uint32_t k;
  int ok = i32_const(s, 40) && op2(s, OP_CALL, 0) && op2(s, OP_LOCAL_SET, EC_R);
  for (k = 0; k < 5u && ok; k++) {
    ok = op2(s, OP_LOCAL_GET, EC_R) && put_byte(s, OP_I32_WRAP_I64) &&
         (k == 1u || k == 3u ? op2(s, OP_LOCAL_GET, k == 1u ? x : y) : i64_const(s, 0)) && memarg(s, OP_I64_STORE, k);
  }
  return ok;
}

/* R := the point at infinity (0, 0); W := a 0 block. */
static int inf_pair(Sink *s) {
  return const_block(s, EC_W, ec_zero) && pair_code(s, EC_W, EC_W);
}

/* point_double: L := 3 AX^2 / 2 AY and C := 1. */
static int double_slope(Sink *s) {
  return fmul_code(s, EC_T, EC_AX, EC_AX) && fadd_code(s, EC_U, EC_T, EC_T) && fadd_code(s, EC_T, EC_U, EC_T) &&
         fadd_code(s, EC_U, EC_AY, EC_AY) && finv_code(s, EC_U, EC_U) && fmul_code(s, EC_L, EC_T, EC_U) &&
         i64_const(s, 1) && op2(s, OP_LOCAL_SET, EC_C);
}

/* group_add: L := (BY - AY) / (BX - AX) and C := 1. */
static int add_slope(Sink *s) {
  return fsub_code(s, EC_T, EC_BY, EC_AY) && fsub_code(s, EC_U, EC_BX, EC_AX) && finv_code(s, EC_U, EC_U) &&
         fmul_code(s, EC_L, EC_T, EC_U) && i64_const(s, 1) && op2(s, OP_LOCAL_SET, EC_C);
}

/* chord: R := the pair x3 = L^2 - AX - BX, y3 = L (AX - x3) - AY (BX = AX
   for a double). */
static int chord_code(Sink *s) {
  return fmul_code(s, EC_T, EC_L, EC_L) && fsub_code(s, EC_T, EC_T, EC_AX) && fsub_code(s, EC_T, EC_T, EC_BX) &&
         fsub_code(s, EC_U, EC_AX, EC_T) && fmul_code(s, EC_U, EC_L, EC_U) && fsub_code(s, EC_U, EC_U, EC_AY) &&
         pair_code(s, EC_T, EC_U);
}

/* jac_double on the Jacobian X, Y, Z: (0, 0, 0) when Z or Y is 0 (W holds a
   0 block), else S = 4 X Y^2, MM = 3 X^2, X3 = MM^2 - 2 S, Y3 = MM (S - X3)
   - 8 Y^4, Z3 = 2 Y Z, in H, HH, HHH. */
static int jac_double_code(Sink *s) {
  return push_zero(s, EC_Z) && push_zero(s, EC_Y) && put_byte(s, OP_I32_OR) && if_code(s) &&
         set_jac(s, EC_W, EC_W, EC_W) && put_byte(s, OP_ELSE) && fmul_code(s, EC_YY, EC_Y, EC_Y) &&
         fmul_code(s, EC_S, EC_X, EC_YY) && fadd_code(s, EC_S, EC_S, EC_S) && fadd_code(s, EC_S, EC_S, EC_S) &&
         fmul_code(s, EC_MM, EC_X, EC_X) && fadd_code(s, EC_T, EC_MM, EC_MM) && fadd_code(s, EC_MM, EC_T, EC_MM) &&
         fmul_code(s, EC_H, EC_MM, EC_MM) && fsub_code(s, EC_H, EC_H, EC_S) && fsub_code(s, EC_H, EC_H, EC_S) &&
         fmul_code(s, EC_YY, EC_YY, EC_YY) && fadd_code(s, EC_YY, EC_YY, EC_YY) && fadd_code(s, EC_YY, EC_YY, EC_YY) &&
         fadd_code(s, EC_YY, EC_YY, EC_YY) && fsub_code(s, EC_T, EC_S, EC_H) && fmul_code(s, EC_HH, EC_MM, EC_T) &&
         fsub_code(s, EC_HH, EC_HH, EC_YY) && fmul_code(s, EC_HHH, EC_Y, EC_Z) && fadd_code(s, EC_HHH, EC_HHH, EC_HHH) &&
         set_jac(s, EC_H, EC_HH, EC_HHH) && put_byte(s, OP_END);
}

/* jac_add_affine with the affine point AX, AY (not (0, 0)): Z = 0 gives (AX,
   AY, 1); else U = AX Z^2, S = AY Z^3, H = U - X, R = S - Y (in BX, BY); H =
   0 gives the double (R = 0) or (0, 0, 0); else X3 = R^2 - H^3 - 2 X H^2,
   Y3 = R (X H^2 - X3) - Y H^3, Z3 = Z H, in U, YY, MM. */
static int jac_add_code(Sink *s) {
  return push_zero(s, EC_Z) && if_code(s) && const_block(s, EC_V, ec_one) && set_jac(s, EC_AX, EC_AY, EC_V) &&
         put_byte(s, OP_ELSE) && fmul_code(s, EC_L, EC_Z, EC_Z) && fmul_code(s, EC_BX, EC_AX, EC_L) &&
         fmul_code(s, EC_BY, EC_AY, EC_L) && fmul_code(s, EC_BY, EC_BY, EC_Z) && fsub_code(s, EC_H, EC_BX, EC_X) &&
         fsub_code(s, EC_BY, EC_BY, EC_Y) && push_zero(s, EC_H) && if_code(s) && push_zero(s, EC_BY) && if_code(s) &&
         jac_double_code(s) && put_byte(s, OP_ELSE) && set_jac(s, EC_W, EC_W, EC_W) && put_byte(s, OP_END) &&
         put_byte(s, OP_ELSE) && fmul_code(s, EC_HH, EC_H, EC_H) && fmul_code(s, EC_HHH, EC_H, EC_HH) &&
         fmul_code(s, EC_V, EC_X, EC_HH) && fmul_code(s, EC_U, EC_BY, EC_BY) && fsub_code(s, EC_U, EC_U, EC_HHH) &&
         fsub_code(s, EC_U, EC_U, EC_V) && fsub_code(s, EC_U, EC_U, EC_V) && fsub_code(s, EC_T, EC_V, EC_U) &&
         fmul_code(s, EC_YY, EC_BY, EC_T) && fmul_code(s, EC_T, EC_Y, EC_HHH) && fsub_code(s, EC_YY, EC_YY, EC_T) &&
         fmul_code(s, EC_MM, EC_Z, EC_H) && set_jac(s, EC_U, EC_YY, EC_MM) && put_byte(s, OP_END) && put_byte(s, OP_END);
}

/* ec_add (group_add): both points must be on the curve. (0, 0) + q = q, p +
   (0, 0) = p; equal x gives the double (y = 0 gives (0, 0)) or (0, 0) for q =
   minus p; else the chord. C is 1 when a slope L is set. */
static int part_ec_add(Sink *s, const void *arg) {
  (void)arg;
  return body_locals(s, 22) && point_flags(s, 0) && point_flags(s, 1) && const_block(s, EC_M, ec_p) && load_word(s, 0, 1) && op2(s, OP_LOCAL_SET, EC_AX) &&
         load_word(s, 0, 3) && op2(s, OP_LOCAL_SET, EC_AY) && load_word(s, 1, 1) && op2(s, OP_LOCAL_SET, EC_BX) &&
         load_word(s, 1, 3) && op2(s, OP_LOCAL_SET, EC_BY) && point_code(s, EC_AX, EC_AY) &&
         point_code(s, EC_BX, EC_BY) && push_inf(s, EC_AX, EC_AY) && if_code(s) && copy_local(s, EC_R, 1) &&
         put_byte(s, OP_ELSE) && push_inf(s, EC_BX, EC_BY) && if_code(s) && copy_local(s, EC_R, 0) &&
         put_byte(s, OP_ELSE) && i64_const(s, 0) && op2(s, OP_LOCAL_SET, EC_C) && push_eq(s, EC_AX, EC_BX) &&
         if_code(s) && push_eq(s, EC_AY, EC_BY) && if_code(s) && push_zero(s, EC_AY) && if_code(s) &&
         put_byte(s, OP_ELSE) && double_slope(s) && put_byte(s, OP_END) && put_byte(s, OP_END) && put_byte(s, OP_ELSE) &&
         add_slope(s) && put_byte(s, OP_END) && op2(s, OP_LOCAL_GET, EC_C) && put_byte(s, OP_I32_WRAP_I64) &&
         if_code(s) && chord_code(s) && put_byte(s, OP_ELSE) && inf_pair(s) && put_byte(s, OP_END) &&
         put_byte(s, OP_END) && put_byte(s, OP_END) && op2(s, OP_LOCAL_GET, EC_R) && put_byte(s, OP_END);
}

/* ec_mul (group_mul): p must be on the curve; (0, 0) k = (0, 0). Else
   double-and-add over the Jacobian from bit 255 of k (any U256), then x = X
   Z^-2, y = Y Z^-3, or (0, 0) for Z = 0. */
static int part_ec_mul(Sink *s, const void *arg) {
  (void)arg;
  return body_locals(s, 22) && point_flags(s, 0) && const_block(s, EC_M, ec_p) && load_word(s, 0, 1) && op2(s, OP_LOCAL_SET, EC_AX) &&
         load_word(s, 0, 3) && op2(s, OP_LOCAL_SET, EC_AY) && point_code(s, EC_AX, EC_AY) &&
         push_inf(s, EC_AX, EC_AY) && if_code(s) && copy_local(s, EC_R, 0) && put_byte(s, OP_ELSE) &&
         const_block(s, EC_W, ec_zero) && set_jac(s, EC_W, EC_W, EC_W) && i64_const(s, 256) &&
         op2(s, OP_LOCAL_SET, EC_I) && put_byte(s, OP_BLOCK) && put_byte(s, TYPE_EMPTY) && put_byte(s, OP_LOOP) &&
         put_byte(s, TYPE_EMPTY) && op2(s, OP_LOCAL_GET, EC_I) && put_byte(s, OP_I64_EQZ) && op2(s, OP_BR_IF, 1) &&
         op2(s, OP_LOCAL_GET, EC_I) && i64_const(s, 1) && put_byte(s, OP_I64_SUB) && op2(s, OP_LOCAL_SET, EC_I) &&
         jac_double_code(s) && op2(s, OP_LOCAL_GET, 1) && op2(s, OP_LOCAL_GET, EC_I) && i64_const(s, 6) &&
         put_byte(s, OP_I64_SHR_U) && i64_const(s, 3) && put_byte(s, OP_I64_SHL) && put_byte(s, OP_I64_ADD) &&
         put_byte(s, OP_I32_WRAP_I64) && memarg(s, OP_I64_LOAD, 0) && op2(s, OP_LOCAL_GET, EC_I) &&
         put_byte(s, OP_I64_SHR_U) && i64_const(s, 1) && put_byte(s, OP_I64_AND) && put_byte(s, OP_I32_WRAP_I64) &&
         if_code(s) && jac_add_code(s) && put_byte(s, OP_END) && op2(s, OP_BR, 0) && put_byte(s, OP_END) &&
         put_byte(s, OP_END) && push_zero(s, EC_Z) && if_code(s) && pair_code(s, EC_W, EC_W) && put_byte(s, OP_ELSE) &&
         finv_code(s, EC_U, EC_Z) && fmul_code(s, EC_T, EC_U, EC_U) && fmul_code(s, EC_BX, EC_X, EC_T) &&
         fmul_code(s, EC_T, EC_T, EC_U) && fmul_code(s, EC_BY, EC_Y, EC_T) && pair_code(s, EC_BX, EC_BY) &&
         put_byte(s, OP_END) && put_byte(s, OP_END) && op2(s, OP_LOCAL_GET, EC_R) && put_byte(s, OP_END);
}

/* schnorr_ok (src/front/frost.c schnorr_verify, S8 part B3): parameters
   key, rx, z, head and e, copied to SC_KEY .. SC_E first (the EC_* locals
   start at 2). C := 0 when z >= n, or the key or rx is p or more or has no
   lift (AUTH_REFUSED); else C := 1 when z G = R + c P, with P = (key, AY), R
   = (rx, BY) and c the frost_challenge hash chain in S. The chain is not
   reduced mod n: P has order n, so c P = (c mod n) P (part_ec_mul takes any
   U256). Y := the pair z G, Z := c P, then R + c P. */
enum { SC_KEY = 24, SC_RX = 25, SC_ZZ = 26, SC_HEAD = 27, SC_E = 28 };

/* The prelude bits that schnorr_ok calls. */
#define SC_NEEDS \
  (EC_NEEDS | (1u << IR_CALL_U256_LE) | (1u << IR_CALL_SHA256) | (1u << IR_CALL_EC_ADD) | (1u << IR_CALL_EC_MUL))

/* The words of n, of (p + 1) / 4, of AUTH_TAG and of G, low word first
   (secp256k1.c GROUP_N, SQRT_EXP, GX, GY; frost.c AUTH_TAG). */
static const uint64_t sc_n[4] = {0xBFD25E8CD0364141u, 0xBAAEDCE6AF48A03Bu, 0xFFFFFFFFFFFFFFFEu, ~(uint64_t)0};
static const uint64_t sc_sqrt[4] = {0xFFFFFFFFBFFFFF0Cu, ~(uint64_t)0, ~(uint64_t)0, 0x3FFFFFFFFFFFFFFFu};
static const uint64_t sc_tag[4] = {0x69726E2D61757468u, 0x6361u, 0u, 0u};
static const uint64_t sc_gx[4] = {0x59F2815B16F81798u, 0x029BFCDB2DCE28D9u, 0x55A06295CE870B07u, 0x79BE667EF9DCBBACu};
static const uint64_t sc_gy[4] = {0x9C47D08FFB10D4B8u, 0xFD17B448A6855419u, 0x5DA4FBFC0E1108A8u, 0x483ADA7726A3C465u};

/* Wasm local DST := the prelude call C (sha256, ec_add or ec_mul) of Wasm
   locals X and Y. */
static int call2_code(Sink *s, IrCall c, uint32_t dst, uint32_t x, uint32_t y) {
  return op2(s, OP_LOCAL_GET, x) && op2(s, OP_LOCAL_GET, y) && op2(s, OP_CALL, call_index(s, c)) &&
         op2(s, OP_LOCAL_SET, dst);
}

/* Pushes the i32 1 when the block at Wasm local X is at most the block at
   Wasm local Y (the prelude u256_le), else 0. */
static int push_le(Sink *s, uint32_t x, uint32_t y) {
  return op2(s, OP_LOCAL_GET, x) && op2(s, OP_LOCAL_GET, y) && op2(s, OP_CALL, call_index(s, IR_CALL_U256_LE)) &&
         put_byte(s, OP_I32_WRAP_I64);
}

/* Wasm local DST := the block at Wasm local X to the power (p + 1) / 4, mod
   EC_M: the pow loop of part_fe_inv, with H = a word of the exponent and I =
   the bit counter. */
static int sqrt_code(Sink *s, uint32_t dst, uint32_t x) {
  uint32_t k;
  int ok = const_block(s, dst, ec_one);
  for (k = 4u; k > 0u && ok; k--) {
    ok = i64_const(s, sc_sqrt[k - 1u]) && op2(s, OP_LOCAL_SET, EC_H) && i64_const(s, 64) &&
         op2(s, OP_LOCAL_SET, EC_I) && put_byte(s, OP_BLOCK) && put_byte(s, TYPE_EMPTY) && put_byte(s, OP_LOOP) &&
         put_byte(s, TYPE_EMPTY) && op2(s, OP_LOCAL_GET, EC_I) && put_byte(s, OP_I64_EQZ) && op2(s, OP_BR_IF, 1) &&
         op2(s, OP_LOCAL_GET, EC_I) && i64_const(s, 1) && put_byte(s, OP_I64_SUB) && op2(s, OP_LOCAL_SET, EC_I) &&
         fmul_code(s, dst, dst, dst) && op2(s, OP_LOCAL_GET, EC_H) && op2(s, OP_LOCAL_GET, EC_I) &&
         put_byte(s, OP_I64_SHR_U) && i64_const(s, 1) && put_byte(s, OP_I64_AND) && put_byte(s, OP_I32_WRAP_I64) &&
         if_code(s) && fmul_code(s, dst, dst, x) && put_byte(s, OP_END) && op2(s, OP_BR, 0) && put_byte(s, OP_END) &&
         put_byte(s, OP_END);
  }
  return ok;
}

/* group_lift_x of the block at Wasm local X (below p): Y := the smaller of y
   and p - y for y = (x^3 + 7)^((p + 1) / 4), and pushes the i32 1 when y^2 =
   x^3 + 7 (X has a lift), else 0. Uses T, U, V, W, H and I. */
static int lift_code(Sink *s, uint32_t x, uint32_t y) {
  return fmul_code(s, EC_U, x, x) && fmul_code(s, EC_U, EC_U, x) && const_block(s, EC_W, ec_seven) &&
         fadd_code(s, EC_U, EC_U, EC_W) && sqrt_code(s, y, EC_U) && fmul_code(s, EC_T, y, y) &&
         const_block(s, EC_W, ec_zero) && fsub_code(s, EC_V, EC_W, y) && push_le(s, y, EC_V) && if_code(s) &&
         put_byte(s, OP_ELSE) && copy_local(s, y, EC_V) && put_byte(s, OP_END) && push_eq(s, EC_T, EC_U);
}

static int part_schnorr_ok(Sink *s, const void *arg) {
  uint32_t k;
  int ok = body_locals(s, 24);
  (void)arg;
  for (k = 0; k < 5u && ok; k++) ok = copy_local(s, SC_KEY + k, k);
  return ok && const_block(s, EC_M, ec_p) && i64_const(s, 0) && op2(s, OP_LOCAL_SET, EC_C) &&
         const_block(s, EC_W, sc_n) && push_le(s, EC_W, SC_ZZ) && push_big(s, SC_KEY) && put_byte(s, OP_I32_OR) &&
         push_big(s, SC_RX) && put_byte(s, OP_I32_OR) && if_code(s) && put_byte(s, OP_ELSE) &&
         lift_code(s, SC_KEY, EC_AY) && lift_code(s, SC_RX, EC_BY) && put_byte(s, OP_I32_AND) && if_code(s) &&
         const_block(s, EC_T, sc_gx) && const_block(s, EC_U, sc_gy) && pair_code(s, EC_T, EC_U) &&
         call2_code(s, IR_CALL_EC_MUL, EC_Y, EC_R, SC_ZZ) && const_block(s, EC_W, sc_tag) &&
         call2_code(s, IR_CALL_SHA256, EC_S, EC_W, SC_KEY) && call2_code(s, IR_CALL_SHA256, EC_S, EC_S, SC_RX) &&
         call2_code(s, IR_CALL_SHA256, EC_S, EC_S, SC_HEAD) && call2_code(s, IR_CALL_SHA256, EC_S, EC_S, SC_E) &&
         pair_code(s, SC_KEY, EC_AY) && call2_code(s, IR_CALL_EC_MUL, EC_Z, EC_R, EC_S) &&
         pair_code(s, SC_RX, EC_BY) && call2_code(s, IR_CALL_EC_ADD, EC_Z, EC_R, EC_Z) && load_word(s, EC_Y, 1) &&
         op2(s, OP_LOCAL_SET, EC_AX) && load_word(s, EC_Y, 3) && op2(s, OP_LOCAL_SET, EC_AY) &&
         load_word(s, EC_Z, 1) && op2(s, OP_LOCAL_SET, EC_BX) && load_word(s, EC_Z, 3) &&
         op2(s, OP_LOCAL_SET, EC_BY) && push_eq(s, EC_AX, EC_BX) && push_eq(s, EC_AY, EC_BY) &&
         put_byte(s, OP_I32_AND) && put_byte(s, OP_I64_EXTEND_I32_U) && op2(s, OP_LOCAL_SET, EC_C) &&
         put_byte(s, OP_END) && put_byte(s, OP_END) && op2(s, OP_LOCAL_GET, EC_C) && put_byte(s, OP_END);
}

/* The prelude (Q-S8-4): function 1 + R is the R-th IrCall that the module
   calls, in IrCall order, after the allocator. Each body ports a C function
   of src/front/u256.c (to_u256: u256_from_u64; u256_le and u256_eq:
   u256_cmp), src/sha256.c (sha256: sha256_words), src/front/field.c (the
   fe_* parts) or src/front/secp256k1.c (ec_add: group_add, ec_mul:
   group_mul) or src/front/frost.c (schnorr_ok:
   schnorr_verify). */
typedef struct {
  const char *name;
  size_t param_count;
  Part body;
  unsigned needs; /* the IrCall bits of the prelude functions that the body calls */
} Prelude;

static const Prelude prelude[] = {
    {"u256_add", 2, part_u256_add, 0u}, {"u256_sub", 2, part_u256_sub, 0u}, {"u256_le", 2, part_u256_le, 0u},
    {"u256_eq", 2, part_u256_eq, 0u},   {"to_u256", 1, part_to_u256, 0u},   {"sha256", 2, part_sha256, 0u},
    {"fe_add", 3, part_fe_add, 0u},     {"fe_sub", 3, part_fe_sub, 0u},     {"fe_mul", 3, part_fe_mul, 0u},
    {"fe_inv", 2, part_fe_inv, 1u << IR_CALL_FE_MUL}, {"ec_add", 2, part_ec_add, EC_NEEDS}, {"ec_mul", 2, part_ec_mul, EC_NEEDS},
    {"schnorr_ok", 5, part_schnorr_ok, SC_NEEDS},
};

#define PRELUDE_COUNT (sizeof prelude / sizeof prelude[0])

static size_t ones(unsigned m) {
  size_t n = 0;
  for (; m != 0u; m &= m - 1u) n++;
  return n;
}

static uint64_t call_index(const Sink *s, IrCall c) {
  return 1u + ones(s->calls & ((1u << c) - 1u));
}

/* IR local LOCAL is Wasm local base + LOCAL. */
static int local_op(Sink *s, unsigned op, uint32_t local) {
  return op2(s, op, (uint64_t)s->base + local);
}

static int emit_expr(Sink *s, const IrExpr *e, uint32_t t, unsigned depth);

/* The operands are on the stack. Moves them to the scratch locals T, T + 1. */
static int operands(Sink *s, uint32_t t) {
  return op2(s, OP_LOCAL_SET, t + 1u) && op2(s, OP_LOCAL_SET, t);
}

static int emit_op(Sink *s, IrOp op, uint32_t t) {
  switch (op) {
    case IR_OP_ADD:
      return put_byte(s, OP_I64_ADD);
    case IR_OP_SUB: /* select(a - b, 0, a >= b) */
      return operands(s, t) && op2(s, OP_LOCAL_GET, t) && op2(s, OP_LOCAL_GET, t + 1u) && put_byte(s, OP_I64_SUB) &&
             i64_const(s, 0) && op2(s, OP_LOCAL_GET, t) && op2(s, OP_LOCAL_GET, t + 1u) && put_byte(s, OP_I64_GE_U) &&
             put_byte(s, OP_SELECT);
    case IR_OP_MUL:
      return put_byte(s, OP_I64_MUL);
    case IR_OP_EQ:
      return put_byte(s, OP_I64_EQ) && put_byte(s, OP_I64_EXTEND_I32_U);
    case IR_OP_LE:
      return put_byte(s, OP_I64_LE_U) && put_byte(s, OP_I64_EXTEND_I32_U);
    case IR_OP_ADD_CARRY: /* a + b < a, modulo 2^64 */
      return operands(s, t) && op2(s, OP_LOCAL_GET, t) && op2(s, OP_LOCAL_GET, t + 1u) && put_byte(s, OP_I64_ADD) &&
             op2(s, OP_LOCAL_GET, t) && put_byte(s, OP_I64_LT_U) && put_byte(s, OP_I64_EXTEND_I32_U);
    case IR_OP_MUL_CARRY:
      /* Exact: a * b > 2^64 - 1 if and only if b > floor((2^64 - 1) / a), for
         a > 0. For a = 0 the divisor is 1, and no b is larger than 2^64 - 1. */
      return operands(s, t) && op2(s, OP_LOCAL_GET, t + 1u) && i64_const(s, ~(uint64_t)0) && i64_const(s, 1) &&
             op2(s, OP_LOCAL_GET, t) && op2(s, OP_LOCAL_GET, t) && put_byte(s, OP_I64_EQZ) && put_byte(s, OP_SELECT) &&
             put_byte(s, OP_I64_DIV_U) && put_byte(s, OP_I64_GT_U) && put_byte(s, OP_I64_EXTEND_I32_U);
    case IR_OP_OR:
      return put_byte(s, OP_I64_OR);
    case IR_OP_LT:
      return put_byte(s, OP_I64_LT_U) && put_byte(s, OP_I64_EXTEND_I32_U);
    case IR_OP_DIV: /* i64.div_u traps on 0 by itself */
      return put_byte(s, OP_I64_DIV_U);
    case IR_OP_MOD:
      return put_byte(s, OP_I64_REM_U);
  }
  return no_code(s, "op", (unsigned)op);
}

/* The arguments, then a call of the prelude function. */
static int emit_call(Sink *s, const IrExpr *e, uint32_t t, unsigned depth) {
  size_t k;
  int ok = ((size_t)e->call < PRELUDE_COUNT && prelude[e->call].param_count == e->arg_count) ||
           no_code(s, "call", (unsigned)e->call);
  for (k = 0; k < e->arg_count && ok; k++) ok = emit_expr(s, e->args[k], t, depth + 1u);
  return ok && op2(s, OP_CALL, call_index(s, e->call));
}

static int emit_expr(Sink *s, const IrExpr *e, uint32_t t, unsigned depth) {
  if (depth > WASM_DEPTH_MAX) return deep(s);
  switch (e->kind) {
    case IR_EXPR_CONST:
      return i64_const(s, e->value);
    case IR_EXPR_LOCAL:
      return local_op(s, OP_LOCAL_GET, e->local);
    case IR_EXPR_BINARY:
      return emit_expr(s, e->left, t, depth + 1u) && emit_expr(s, e->right, t, depth + 1u) && emit_op(s, e->op, t);
    case IR_EXPR_LOAD:
      return emit_expr(s, e->left, t, depth + 1u) && put_byte(s, OP_I32_WRAP_I64) && memarg(s, OP_I64_LOAD, e->field);
    case IR_EXPR_CALL:
      return emit_call(s, e, t, depth);
  }
  return no_code(s, "expression", (unsigned)e->kind);
}

static int emit_block(Sink *s, const IrBlock *b, uint32_t t, unsigned depth);

/* local := alloc(8 * n); word k := fields[k]. The address waits in T + 2. */
static int emit_alloc(Sink *s, const IrStmt *st, uint32_t t, unsigned depth) {
  size_t k;
  int ok = i32_const(s, 8u * (uint64_t)st->field_count) && op2(s, OP_CALL, 0) && op2(s, OP_LOCAL_SET, t + 2u);
  for (k = 0; k < st->field_count && ok; k++) {
    ok = op2(s, OP_LOCAL_GET, t + 2u) && put_byte(s, OP_I32_WRAP_I64) && emit_expr(s, st->fields[k], t, depth + 1u) &&
         memarg(s, OP_I64_STORE, (uint32_t)k);
  }
  return ok && op2(s, OP_LOCAL_GET, t + 2u) && local_op(s, OP_LOCAL_SET, st->local);
}

/* A U256 result: the address is on the stack. Copies the 4 words to address
   0, then leaves 0 (Q-S8-3). */
static int result_block(Sink *s, uint32_t t) {
  uint32_t j;
  int ok = op2(s, OP_LOCAL_SET, t + 2u);
  for (j = 0; j < 4u && ok; j++) {
    ok = i32_const(s, 0) && op2(s, OP_LOCAL_GET, t + 2u) && put_byte(s, OP_I32_WRAP_I64) && memarg(s, OP_I64_LOAD, j) &&
         memarg(s, OP_I64_STORE, j);
  }
  return ok && i64_const(s, 0);
}

/* Blocks: exit, trap, then arm n - 1 down to arm 0 (the innermost). br_table
   label k ends the block of arm k, so the code of arm k follows. A value of
   n or more takes label n, the trap block. After arm k, label n - k is the
   exit block. */
static int emit_switch(Sink *s, const IrStmt *st, uint32_t t, unsigned depth) {
  size_t k;
  size_t n = st->arm_count;
  int ok = put_byte(s, OP_BLOCK) && put_byte(s, TYPE_EMPTY) && put_byte(s, OP_BLOCK) && put_byte(s, TYPE_EMPTY);
  for (k = 0; k < n && ok; k++) ok = put_byte(s, OP_BLOCK) && put_byte(s, TYPE_EMPTY);
  ok = ok && emit_expr(s, st->expr, t, depth + 1u) && op2(s, OP_LOCAL_TEE, t) && put_byte(s, OP_I32_WRAP_I64) &&
       i32_const(s, n) && op2(s, OP_LOCAL_GET, t) && i64_const(s, n) && put_byte(s, OP_I64_LT_U) &&
       put_byte(s, OP_SELECT) && op2(s, OP_BR_TABLE, n);
  for (k = 0; k <= n && ok; k++) ok = put_uleb(s, k);
  for (k = 0; k < n && ok; k++) {
    ok = put_byte(s, OP_END) && emit_block(s, &st->arms[k], t, depth + 1u) && op2(s, OP_BR, n - k);
  }
  return ok && put_byte(s, OP_END) && put_byte(s, OP_UNREACHABLE) && put_byte(s, OP_END);
}

/* counter := expr; then, at the top of each run, leave when counter = 0 or
   (for WHILE) local = 0. counter decreases by 1 after each run. */
static int emit_loop(Sink *s, const IrStmt *st, int is_while, uint32_t t, unsigned depth) {
  return emit_expr(s, st->expr, t, depth + 1u) && local_op(s, OP_LOCAL_SET, st->counter) && put_byte(s, OP_BLOCK) &&
         put_byte(s, TYPE_EMPTY) && put_byte(s, OP_LOOP) && put_byte(s, TYPE_EMPTY) &&
         local_op(s, OP_LOCAL_GET, st->counter) && put_byte(s, OP_I64_EQZ) && op2(s, OP_BR_IF, 1) &&
         (!is_while || (local_op(s, OP_LOCAL_GET, st->local) && put_byte(s, OP_I64_EQZ) && op2(s, OP_BR_IF, 1))) &&
         emit_block(s, &st->body, t, depth + 1u) && local_op(s, OP_LOCAL_GET, st->counter) && i64_const(s, 1) &&
         put_byte(s, OP_I64_SUB) && local_op(s, OP_LOCAL_SET, st->counter) && op2(s, OP_BR, 0) && put_byte(s, OP_END) &&
         put_byte(s, OP_END);
}

static int emit_stmt(Sink *s, const IrStmt *st, uint32_t t, unsigned depth) {
  if (depth > WASM_DEPTH_MAX) return deep(s);
  switch (st->kind) {
    case IR_STMT_SET:
      return emit_expr(s, st->expr, t, depth + 1u) && local_op(s, OP_LOCAL_SET, st->local);
    case IR_STMT_ALLOC:
      return emit_alloc(s, st, t, depth);
    case IR_STMT_IF: /* expr = 0: otherwise, else: body */
      return emit_expr(s, st->expr, t, depth + 1u) && put_byte(s, OP_I64_EQZ) && put_byte(s, OP_IF) &&
             put_byte(s, TYPE_EMPTY) && emit_block(s, &st->otherwise, t, depth + 1u) && put_byte(s, OP_ELSE) &&
             emit_block(s, &st->body, t, depth + 1u) && put_byte(s, OP_END);
    case IR_STMT_SWITCH:
      return emit_switch(s, st, t, depth);
    case IR_STMT_REPEAT:
      return emit_loop(s, st, 0, t, depth);
    case IR_STMT_WHILE:
      return emit_loop(s, st, 1, t, depth);
    case IR_STMT_TRAP:
      return put_byte(s, OP_UNREACHABLE);
    case IR_STMT_RETURN:
      return emit_expr(s, st->expr, t, depth + 1u) && (!s->wide || result_block(s, t)) && put_byte(s, OP_RETURN);
  }
  return no_code(s, "statement", (unsigned)st->kind);
}

static int emit_block(Sink *s, const IrBlock *b, uint32_t t, unsigned depth) {
  size_t i;
  int ok = depth <= WASM_DEPTH_MAX || deep(s);
  for (i = 0; i < b->count && ok; i++) ok = emit_stmt(s, b->items[i], t, depth + 1u);
  return ok;
}

/* alloc (bytes : i32) -> i64, local 1 = the new end:
   old := heap; end := old + bytes; if end > memory bytes { grow by
   ceil(end / 65536) - pages; trap on -1 }; heap := end; return old. */
static int part_alloc(Sink *s, const void *arg) {
  (void)arg;
  return put_uleb(s, 1) && put_uleb(s, 1) && put_byte(s, TYPE_I64) && op2(s, OP_GLOBAL_GET, 0) &&
         op2(s, OP_GLOBAL_GET, 0) && op2(s, OP_LOCAL_GET, 0) && put_byte(s, OP_I64_EXTEND_I32_U) &&
         put_byte(s, OP_I64_ADD) && op2(s, OP_LOCAL_TEE, 1) && op2(s, OP_MEMORY_SIZE, 0) &&
         put_byte(s, OP_I64_EXTEND_I32_U) && i64_const(s, 16) && put_byte(s, OP_I64_SHL) && put_byte(s, OP_I64_GT_U) &&
         put_byte(s, OP_IF) && put_byte(s, TYPE_EMPTY) && op2(s, OP_LOCAL_GET, 1) && i64_const(s, 65535) &&
         put_byte(s, OP_I64_ADD) && i64_const(s, 16) && put_byte(s, OP_I64_SHR_U) && op2(s, OP_MEMORY_SIZE, 0) &&
         put_byte(s, OP_I64_EXTEND_I32_U) && put_byte(s, OP_I64_SUB) && put_byte(s, OP_I32_WRAP_I64) &&
         op2(s, OP_MEMORY_GROW, 0) && i32_const(s, ~(uint64_t)0) && put_byte(s, OP_I32_EQ) && put_byte(s, OP_IF) &&
         put_byte(s, TYPE_EMPTY) && put_byte(s, OP_UNREACHABLE) && put_byte(s, OP_END) && put_byte(s, OP_END) &&
         op2(s, OP_LOCAL_GET, 1) && op2(s, OP_GLOBAL_SET, 0) && put_byte(s, OP_END);
}

/* The Wasm parameters of FN: 4 per U256, 1 per Nat or Flag. */
static uint32_t wasm_params(const IrFunc *fn) {
  uint32_t w = 0;
  size_t k;
  for (k = 0; k < fn->param_count; k++) w += fn->params[k] == IR_SCALAR_U256 ? 4u : 1u;
  return w;
}

/* Traps when the Flag parameter K (Wasm local POS) is larger than 1. */
static int flag_guard(Sink *s, const IrFunc *fn, size_t k, uint32_t pos) {
  return fn->params[k] != IR_SCALAR_FLAG ||
         (op2(s, OP_LOCAL_GET, pos) && i64_const(s, 1) && put_byte(s, OP_I64_GT_U) && put_byte(s, OP_IF) &&
          put_byte(s, TYPE_EMPTY) && put_byte(s, OP_UNREACHABLE) && put_byte(s, OP_END));
}

/* Wasm local DST := a new block; word 3 - J := Wasm parameter POS + J (high
   limb first in the parameters, low limb first in the block). */
static int param_block(Sink *s, uint32_t dst, uint32_t pos) {
  uint32_t j;
  int ok = new_block(s, dst);
  for (j = 0; j < 4u && ok; j++) {
    ok = op2(s, OP_LOCAL_GET, dst) && put_byte(s, OP_I32_WRAP_I64) && op2(s, OP_LOCAL_GET, pos + j) &&
         memarg(s, OP_I64_STORE, 3u - j);
  }
  return ok;
}

/* The guard, then (with a base) parameter K moves from Wasm local POS to IR
   local K. */
static int entry_param(Sink *s, const IrFunc *fn, size_t k, uint32_t pos) {
  uint32_t dst = s->base + (uint32_t)k;
  return flag_guard(s, fn, k, pos) &&
         (s->base == 0u || (fn->params[k] == IR_SCALAR_U256
                                ? param_block(s, dst, pos)
                                : (op2(s, OP_LOCAL_GET, pos) && op2(s, OP_LOCAL_SET, dst))));
}

static int part_entry(Sink *s, const void *arg) {
  const IrFunc *fn = arg;
  uint32_t w = wasm_params(fn);
  uint32_t pos = 0;
  size_t k;
  int ok;
  s->base = w == fn->param_count ? 0u : w;
  s->wide = fn->result == IR_SCALAR_U256;
  ok = put_uleb(s, 1) && put_uleb(s, (uint64_t)s->base + fn->local_count + 3u - w) && put_byte(s, TYPE_I64) &&
       i64_const(s, WASM_HEAP_START) && op2(s, OP_GLOBAL_SET, 0);
  for (k = 0; k < fn->param_count && ok; k++) {
    ok = entry_param(s, fn, k, pos);
    pos += fn->params[k] == IR_SCALAR_U256 ? 4u : 1u;
  }
  return ok && emit_block(s, &fn->body, s->base + fn->local_count, 0) && put_byte(s, OP_UNREACHABLE) &&
         put_byte(s, OP_END);
}

/* The prelude functions the IR calls, one bit per IrCall. */
static unsigned calls_block(const IrBlock *b, unsigned depth);

static unsigned calls_expr(const IrExpr *e, unsigned depth) {
  size_t k;
  unsigned m = 0;
  if (depth > WASM_DEPTH_MAX) return 0;
  switch (e->kind) {
    case IR_EXPR_CONST:
    case IR_EXPR_LOCAL:
      return 0;
    case IR_EXPR_BINARY:
      return calls_expr(e->left, depth + 1u) | calls_expr(e->right, depth + 1u);
    case IR_EXPR_LOAD:
      return calls_expr(e->left, depth + 1u);
    case IR_EXPR_CALL:
      for (k = 0; k < e->arg_count; k++) m |= calls_expr(e->args[k], depth + 1u);
      return (size_t)e->call < PRELUDE_COUNT ? m | (1u << e->call) : m;
  }
  return 0;
}

static unsigned calls_stmt(const IrStmt *st, unsigned depth) {
  size_t k;
  unsigned m = 0;
  if (depth > WASM_DEPTH_MAX) return 0;
  switch (st->kind) {
    case IR_STMT_SET:
    case IR_STMT_RETURN:
      return calls_expr(st->expr, depth + 1u);
    case IR_STMT_ALLOC:
      for (k = 0; k < st->field_count; k++) m |= calls_expr(st->fields[k], depth + 1u);
      return m;
    case IR_STMT_IF:
      return calls_expr(st->expr, depth + 1u) | calls_block(&st->body, depth + 1u) |
             calls_block(&st->otherwise, depth + 1u);
    case IR_STMT_SWITCH:
      for (k = 0; k < st->arm_count; k++) m |= calls_block(&st->arms[k], depth + 1u);
      return m | calls_expr(st->expr, depth + 1u);
    case IR_STMT_REPEAT:
    case IR_STMT_WHILE:
      return calls_expr(st->expr, depth + 1u) | calls_block(&st->body, depth + 1u);
    case IR_STMT_TRAP:
      return 0;
  }
  return 0;
}

static unsigned calls_block(const IrBlock *b, unsigned depth) {
  size_t i;
  unsigned m = 0;
  for (i = 0; i < b->count && depth <= WASM_DEPTH_MAX; i++) m |= calls_stmt(b->items[i], depth + 1u);
  return m;
}

static unsigned program_calls(const IrProgram *prog) {
  size_t i;
  size_t r;
  unsigned m = 0;
  for (i = 0; i < prog->func_count; i++) m |= calls_block(&prog->funcs[i].body, 0);
  /* The closure: each called prelude body adds the bits it calls (needs). */
  for (r = 0; r < PRELUDE_COUNT; r++)
    for (i = 0; i < PRELUDE_COUNT; i++) m |= ((m >> i) & 1u) ? prelude[i].needs : 0u;
  return m;
}

/* (i64 x N) -> i64 */
static int func_type(Sink *s, uint64_t n) {
  uint64_t k;
  int ok = put_byte(s, TYPE_FUNC) && put_uleb(s, n);
  for (k = 0; k < n && ok; k++) ok = put_byte(s, TYPE_I64);
  return ok && put_uleb(s, 1) && put_byte(s, TYPE_I64);
}

/* Function I has type I: the allocator, the prelude functions, the entries. */
static int part_types(Sink *s, const void *arg) {
  const IrProgram *prog = arg;
  size_t i;
  int ok = put_uleb(s, 1u + ones(s->calls) + prog->func_count) && put_byte(s, TYPE_FUNC) && put_uleb(s, 1) &&
           put_byte(s, TYPE_I32) && put_uleb(s, 1) && put_byte(s, TYPE_I64);
  for (i = 0; i < PRELUDE_COUNT && ok; i++) ok = !((s->calls >> i) & 1u) || func_type(s, prelude[i].param_count);
  for (i = 0; i < prog->func_count && ok; i++) ok = func_type(s, wasm_params(&prog->funcs[i]));
  return ok;
}

static int part_functions(Sink *s, const void *arg) {
  const IrProgram *prog = arg;
  size_t n = 1u + ones(s->calls) + prog->func_count;
  size_t i;
  int ok = put_uleb(s, n);
  for (i = 0; i < n && ok; i++) ok = put_uleb(s, i);
  return ok;
}

static int part_memory(Sink *s, const void *arg) {
  (void)arg;
  return put_uleb(s, 1) && put_byte(s, 1) && put_uleb(s, 1) && put_uleb(s, WASM_MAX_PAGES);
}

static int part_global(Sink *s, const void *arg) {
  (void)arg;
  return put_uleb(s, 1) && put_byte(s, TYPE_I64) && put_byte(s, 1) && i64_const(s, 0) && put_byte(s, OP_END);
}

static int part_exports(Sink *s, const void *arg) {
  const IrProgram *prog = arg;
  size_t i;
  int ok = put_uleb(s, prog->func_count + 1u);
  for (i = 0; i < prog->func_count && ok; i++) {
    ok = put_name(s, prog->funcs[i].name) && put_byte(s, 0) && put_uleb(s, 1u + ones(s->calls) + i);
  }
  /* Memory 0, so a host reads a U256 result at 0..31 (Q-S8-3, M8). The name is
     not a cairn name, so no entry export can take it (item 4). */
  return ok && put_name(s, "$memory") && put_byte(s, 2) && put_uleb(s, 0);
}

static int part_prelude(Sink *s, const void *arg) {
  const Prelude *p = arg;
  s->base = 0;
  s->wide = 0;
  if (p->body != NULL) return p->body(s, NULL);
  fprintf(s->err, "langc: WASM_PRELUDE: -: the prelude function %s has no body\n", p->name);
  return 0;
}

static int part_code(Sink *s, const void *arg) {
  const IrProgram *prog = arg;
  size_t i;
  int ok = put_uleb(s, 1u + ones(s->calls) + prog->func_count) && sized(s, part_alloc, NULL);
  for (i = 0; i < PRELUDE_COUNT && ok; i++) ok = !((s->calls >> i) & 1u) || sized(s, part_prelude, &prelude[i]);
  for (i = 0; i < prog->func_count && ok; i++) ok = sized(s, part_entry, &prog->funcs[i]);
  return ok;
}

static const char *abi_shape(IrScalar sc, int is_result) {
  switch (sc) {
    case IR_SCALAR_NAT:
      return "nat";
    case IR_SCALAR_FLAG:
      return "flag";
    case IR_SCALAR_U256:
      return is_result ? "u256[memory 0..31, low limb first]" : "u256[4 i64, high limb first]";
  }
  return "?";
}

/* NAME (PARAM, ...) -> RESULT, with the Wasm shape of each U256 (Q-S8-3). */
int target_abi_line(const IrFunc *fn, FILE *out, FILE *err) {
  size_t k;
  (void)err;
  fprintf(out, "%s (", fn->name);
  for (k = 0; k < fn->param_count; k++) fprintf(out, "%s%s", k == 0 ? "" : ", ", abi_shape(fn->params[k], 0));
  fprintf(out, ") -> %s\n", abi_shape(fn->result, 1));
  return 1;
}

/* Both parts are the module bytes. */
int target_write(const IrProgram *prog, TargetPart part, FILE *out, FILE *err) {
  static const unsigned char header[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
  Sink s = {out, 0, err, 0, 0, 0};
  size_t i;
  int ok = 1;
  (void)part;
  s.calls = program_calls(prog);
  for (i = 0; i < sizeof header && ok; i++) ok = put_byte(&s, header[i]);
  ok = ok && section(&s, SECTION_TYPE, part_types, prog) && section(&s, SECTION_FUNCTION, part_functions, prog) &&
       section(&s, SECTION_MEMORY, part_memory, NULL) && section(&s, SECTION_GLOBAL, part_global, NULL) &&
       section(&s, SECTION_EXPORT, part_exports, prog) && section(&s, SECTION_CODE, part_code, prog);
  if (!ok && ferror(out)) fputs("langc: IO: -: cannot write the wasm module\n", err);
  return ok;
}
