/* Field arithmetic for front/field.h (slice S4, survey section 3): moved
   from the alt_bn128 code, whose field part came from glue-lang src/front/fr.c
   at cded21d. */
#include <string.h>

#include "front/field.h"

static int cmp_limbs(const uint32_t *a, const uint32_t *b) {
  unsigned i;
  for (i = FE_LIMBS; i > 0; i--) {
    if (a[i - 1] != b[i - 1])
      return a[i - 1] < b[i - 1] ? -1 : 1;
  }
  return 0;
}

/* A = A - B modulo 2^256. Returns the borrow. */
static uint32_t sub_limbs(uint32_t *a, const uint32_t *b) {
  uint64_t borrow = 0;
  unsigned i;
  for (i = 0; i < FE_LIMBS; i++) {
    uint64_t d = (uint64_t)a[i] - b[i] - borrow;
    a[i] = (uint32_t)d;
    borrow = d >> 63;
  }
  return (uint32_t)borrow;
}

/* A = A + B modulo 2^256. Returns the carry. */
static uint32_t add_limbs(uint32_t *a, const uint32_t *b) {
  uint64_t carry = 0;
  unsigned i;
  for (i = 0; i < FE_LIMBS; i++) {
    uint64_t s = (uint64_t)a[i] + b[i] + carry;
    a[i] = (uint32_t)s;
    carry = s >> 32;
  }
  return (uint32_t)carry;
}

int fe_is_zero(const Fe *x) {
  unsigned i;
  for (i = 0; i < FE_LIMBS; i++) {
    if (x->w[i] != 0)
      return 0;
  }
  return 1;
}

static void u256_limbs(uint32_t *out, const U256 *x) {
  unsigned i;
  for (i = 0; i < FE_LIMBS; i++)
    out[i] = (uint32_t)(x->limb[i / 2u] >> (32u * (i % 2u)));
}

int fe_from_u256(Fe *out, const U256 *x, const Modulus *f) {
  Fe t;
  u256_limbs(t.w, x);
  if (cmp_limbs(t.w, f->w) >= 0)
    return 0;
  *out = t;
  return 1;
}

void fe_to_u256(U256 *out, const Fe *x) {
  unsigned i;
  for (i = 0; i < 4u; i++)
    out->limb[i] = (uint64_t)x->w[2u * i] | ((uint64_t)x->w[2u * i + 1u] << 32);
}

void fe_from_u64(Fe *out, uint64_t n) {
  memset(out, 0, sizeof *out);
  out->w[0] = (uint32_t)n;
  out->w[1] = (uint32_t)(n >> 32);
}

void fe_add(Fe *out, const Fe *a, const Fe *b, const Modulus *f) {
  /* a + b < 2m. A carry out of the top limb (a modulus near 2^256, as the
     secp256k1 p of S7 b4) means the sum is m or more; the wrapping
     subtraction is then exact. */
  Fe t = *a;
  uint32_t carry = add_limbs(t.w, b->w);
  if (carry != 0 || cmp_limbs(t.w, f->w) >= 0)
    sub_limbs(t.w, f->w);
  *out = t;
}

void fe_sub(Fe *out, const Fe *a, const Fe *b, const Modulus *f) {
  Fe t = *a;
  if (sub_limbs(t.w, b->w) != 0)
    add_limbs(t.w, f->w);
  *out = t;
}

void fe_neg(Fe *out, const Fe *a, const Modulus *f) {
  Fe zero;
  memset(&zero, 0, sizeof zero);
  fe_sub(out, &zero, a, f);
}

/* The product of A and B, then the remainder modulo m by shift and subtract,
   from the top bit of the 512-bit product. The remainder stays below m, so
   2 rem + 1 < 2m. For a modulus near 2^256 (secp256k1, S7 b4) the shift can
   carry out of the top limb (TOP); the wrapping subtraction is then exact. */
void fe_mul(Fe *out, const Fe *a, const Fe *b, const Modulus *f) {
  uint32_t p[2u * FE_LIMBS];
  Fe rem;
  unsigned i;
  unsigned j;
  memset(p, 0, sizeof p);
  memset(&rem, 0, sizeof rem);
  for (i = 0; i < FE_LIMBS; i++) {
    uint64_t carry = 0;
    for (j = 0; j < FE_LIMBS; j++) {
      uint64_t t = (uint64_t)a->w[i] * b->w[j] + p[i + j] + carry;
      p[i + j] = (uint32_t)t;
      carry = t >> 32;
    }
    p[i + FE_LIMBS] = (uint32_t)carry;
  }
  for (i = 2u * FE_LIMBS * 32u; i > 0; i--) {
    uint32_t bit = (p[(i - 1) / 32u] >> ((i - 1) % 32u)) & 1u;
    uint32_t top = rem.w[FE_LIMBS - 1] >> 31;
    for (j = FE_LIMBS - 1; j > 0; j--)
      rem.w[j] = (rem.w[j] << 1) | (rem.w[j - 1] >> 31);
    rem.w[0] = (rem.w[0] << 1) | bit;
    if (top != 0 || cmp_limbs(rem.w, f->w) >= 0)
      sub_limbs(rem.w, f->w);
  }
  *out = rem;
}

/* Square and multiply over fe_mul, from the top bit of E. */
static void pow_limbs(Fe *out, const Fe *a, const uint32_t *e, const Modulus *f) {
  Fe acc;
  unsigned i;
  fe_from_u64(&acc, 1);
  for (i = FE_LIMBS * 32u; i > 0; i--) {
    fe_mul(&acc, &acc, &acc, f);
    if ((e[(i - 1) / 32u] >> ((i - 1) % 32u)) & 1u)
      fe_mul(&acc, &acc, a, f);
  }
  *out = acc;
}

void fe_pow(Fe *out, const Fe *a, const U256 *e, const Modulus *f) {
  uint32_t w[FE_LIMBS];
  u256_limbs(w, e);
  pow_limbs(out, a, w, f);
}

/* At 0 the power is 0, because m minus 2 is not 0. The low limb of p and of
   r is 2 or more, so the subtract has no borrow. */
void fe_inv(Fe *out, const Fe *a, const Modulus *f) {
  uint32_t e[FE_LIMBS];
  memcpy(e, f->w, sizeof e);
  e[0] -= 2u;
  pow_limbs(out, a, e, f);
}

int fe_eq(const Fe *a, const Fe *b) {
  return cmp_limbs(a->w, b->w) == 0;
}

int fe_lt(const Fe *a, const Fe *b) {
  return cmp_limbs(a->w, b->w) < 0;
}
