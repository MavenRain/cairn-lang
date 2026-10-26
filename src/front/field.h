/* The prime fields of the secp256k1 group (slice S4, survey section 3).
   Moved from the alt_bn128 header, whose field part came from glue-lang
   src/front/fr.c at cded21d. front/secp256k1.c gives the moduli p and n.

   An element is 8 limbs of 32 bits, least significant first, with a value in
   [0, m) for its modulus m. The arithmetic uses uint64_t intermediates only
   (no __int128), so that TinyCC builds it. */
#ifndef LANG_FRONT_FIELD_H
#define LANG_FRONT_FIELD_H

#include <stdint.h>

#include "front/u256.h"

#define FE_LIMBS 8u

/* A modulus: p for the base field, n for the scalars. */
typedef struct {
  uint32_t w[FE_LIMBS];
} Modulus;

typedef struct {
  uint32_t w[FE_LIMBS];
} Fe;

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
/* 1 when X is 0. */
int fe_is_zero(const Fe *x);

#endif
