/* Ported from glue-lang src/front/fr.c at cded21d (the field part). The G1
   part is new (slice S8 B1, USER ruling Q1).

   BN254 field and G1 arithmetic for the host ecAdd and ecMul (docs/PLAN.md
   section 8, builder call S8).

   p = 0x30644e72e131a029b85045b68181585d97816a916871ca8d3c208c16d87cfd47 (Fp)
   r = 0x30644e72e131a029b85045b68181585d2833e84879b9709143e1f593f0000001 (Fr)

   An element is 8 limbs of 32 bits, least significant first, with a value in
   [0, m) for its modulus m. The arithmetic uses uint64_t intermediates only
   (no __int128), so that TinyCC builds it. */
#ifndef LANG_FRONT_BN254_H
#define LANG_FRONT_BN254_H

#include <stdint.h>

#include "front/u256.h"

#define BN_LIMBS 8u

/* A modulus: p for Fp, r for Fr. */
typedef struct {
  uint32_t w[BN_LIMBS];
} Modulus;

typedef struct {
  uint32_t w[BN_LIMBS];
} Fe;

extern const Modulus BN254_FP;
extern const Modulus BN254_FR;

/* Returns 1, or 0 when X is F or more. */
int fe_from_u256(Fe *out, const U256 *x, const Modulus *f);
void fe_to_u256(U256 *out, const Fe *x);
void fe_from_u64(Fe *out, uint64_t n);
void fe_add(Fe *out, const Fe *a, const Fe *b, const Modulus *f);
void fe_sub(Fe *out, const Fe *a, const Fe *b, const Modulus *f);
void fe_neg(Fe *out, const Fe *a, const Modulus *f);
void fe_mul(Fe *out, const Fe *a, const Fe *b, const Modulus *f);
/* A^E for any 256-bit E. */
void fe_pow(Fe *out, const Fe *a, const U256 *e, const Modulus *f);
/* The inverse of A:  A^(m minus 2) (Fermat), thus 0 at 0. */
void fe_inv(Fe *out, const Fe *a, const Modulus *f);
int fe_eq(const Fe *a, const Fe *b);
/* 1 when A is less than B. */
int fe_lt(const Fe *a, const Fe *b);

/* A point of G1 on y^2 = x^3 + 3 over Fp, in affine form. (0, 0) is the
   point at infinity (EIP-196). */
typedef struct {
  Fe x;
  Fe y;
} G1;

/* 1 when P is on the curve; (0, 0) is. */
int g1_on_curve(const G1 *p);
/* The point (X, Y). Returns 1, or 0 when a coordinate is p or more or the
   point is not on the curve (the 0x06 and 0x07 failure). */
int g1_from_words(G1 *out, const U256 *x, const U256 *y);
void g1_to_words(U256 *x, U256 *y, const G1 *p);
/* Each operation takes points on the curve. OUT can be A or B. */
void g1_add(G1 *out, const G1 *a, const G1 *b);
void g1_double(G1 *out, const G1 *a);
/* K times P, double and add over all 256 bits of K, so a K of r or more
   works as in 0x07. */
void g1_mul(G1 *out, const G1 *p, const U256 *k);

/* The same operations on any curve y^2 = x^3 + b over the field F (a = 0;
   the formulas do not use b), for the secp256k1 of front/secp256k1.c (S7 b4).
   The g1_* operations are these with F = BN254_FP. */
void ec0_double(G1 *out, const G1 *a, const Modulus *f);
void ec0_add(G1 *out, const G1 *a, const G1 *b, const Modulus *f);
void ec0_mul(G1 *out, const G1 *p, const U256 *k, const Modulus *f);

#endif
