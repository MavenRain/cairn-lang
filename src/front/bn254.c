/* Ported from glue-lang src/front/fr.c at cded21d (the field part, with the
   modulus as a parameter). The G1 part is new (slice S8 B1). See
   front/bn254.h. */
#include <string.h>

#include "front/bn254.h"

const Modulus BN254_FP = {{
  0xd87cfd47u, 0x3c208c16u, 0x6871ca8du, 0x97816a91u,
  0x8181585du, 0xb85045b6u, 0xe131a029u, 0x30644e72u,
}};

const Modulus BN254_FR = {{
  0xf0000001u, 0x43e1f593u, 0x79b97091u, 0x2833e848u,
  0x8181585du, 0xb85045b6u, 0xe131a029u, 0x30644e72u,
}};

/* -1, 0 or 1 as A is less than, equal to or more than B. */
static int cmp_limbs(const uint32_t *a, const uint32_t *b) {
  unsigned i;
  for (i = BN_LIMBS; i > 0; i--) {
    if (a[i - 1] != b[i - 1])
      return a[i - 1] < b[i - 1] ? -1 : 1;
  }
  return 0;
}

/* A = A - B modulo 2^256. Returns the borrow. */
static uint32_t sub_limbs(uint32_t *a, const uint32_t *b) {
  uint64_t borrow = 0;
  unsigned i;
  for (i = 0; i < BN_LIMBS; i++) {
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
  for (i = 0; i < BN_LIMBS; i++) {
    uint64_t s = (uint64_t)a[i] + b[i] + carry;
    a[i] = (uint32_t)s;
    carry = s >> 32;
  }
  return (uint32_t)carry;
}

static int is_zero(const Fe *x) {
  unsigned i;
  for (i = 0; i < BN_LIMBS; i++) {
    if (x->w[i] != 0)
      return 0;
  }
  return 1;
}

static void u256_limbs(uint32_t *out, const U256 *x) {
  unsigned i;
  for (i = 0; i < BN_LIMBS; i++)
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
  uint32_t p[2u * BN_LIMBS];
  Fe rem;
  unsigned i;
  unsigned j;
  memset(p, 0, sizeof p);
  memset(&rem, 0, sizeof rem);
  for (i = 0; i < BN_LIMBS; i++) {
    uint64_t carry = 0;
    for (j = 0; j < BN_LIMBS; j++) {
      uint64_t t = (uint64_t)a->w[i] * b->w[j] + p[i + j] + carry;
      p[i + j] = (uint32_t)t;
      carry = t >> 32;
    }
    p[i + BN_LIMBS] = (uint32_t)carry;
  }
  for (i = 2u * BN_LIMBS * 32u; i > 0; i--) {
    uint32_t bit = (p[(i - 1) / 32u] >> ((i - 1) % 32u)) & 1u;
    uint32_t top = rem.w[BN_LIMBS - 1] >> 31;
    for (j = BN_LIMBS - 1; j > 0; j--)
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
  for (i = BN_LIMBS * 32u; i > 0; i--) {
    fe_mul(&acc, &acc, &acc, f);
    if ((e[(i - 1) / 32u] >> ((i - 1) % 32u)) & 1u)
      fe_mul(&acc, &acc, a, f);
  }
  *out = acc;
}

void fe_pow(Fe *out, const Fe *a, const U256 *e, const Modulus *f) {
  uint32_t w[BN_LIMBS];
  u256_limbs(w, e);
  pow_limbs(out, a, w, f);
}

/* At 0 the power is 0, because m minus 2 is not 0. The low limb of p and of
   r is 2 or more, so the subtract has no borrow. */
void fe_inv(Fe *out, const Fe *a, const Modulus *f) {
  uint32_t e[BN_LIMBS];
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

/* G1. Each coordinate is in Fp. */

static int is_infinity(const G1 *p) {
  return is_zero(&p->x) && is_zero(&p->y);
}

static void infinity(G1 *out) {
  memset(out, 0, sizeof *out);
}

int g1_on_curve(const G1 *p) {
  Fe lhs;
  Fe rhs;
  Fe three;
  if (is_infinity(p))
    return 1;
  fe_from_u64(&three, 3);
  fe_mul(&lhs, &p->y, &p->y, &BN254_FP);
  fe_mul(&rhs, &p->x, &p->x, &BN254_FP);
  fe_mul(&rhs, &rhs, &p->x, &BN254_FP);
  fe_add(&rhs, &rhs, &three, &BN254_FP);
  return fe_eq(&lhs, &rhs);
}

int g1_from_words(G1 *out, const U256 *x, const U256 *y) {
  G1 t;
  if (!fe_from_u256(&t.x, x, &BN254_FP) || !fe_from_u256(&t.y, y, &BN254_FP) || !g1_on_curve(&t))
    return 0;
  *out = t;
  return 1;
}

void g1_to_words(U256 *x, U256 *y, const G1 *p) {
  fe_to_u256(x, &p->x);
  fe_to_u256(y, &p->y);
}

/* The point with the slope L through A: x3 = L^2 minus a.x minus b.x,
   y3 = L (a.x minus x3) minus a.y. */
static void chord(G1 *out, const Fe *l, const G1 *a, const Fe *bx, const Modulus *f) {
  G1 t;
  fe_mul(&t.x, l, l, f);
  fe_sub(&t.x, &t.x, &a->x, f);
  fe_sub(&t.x, &t.x, bx, f);
  fe_sub(&t.y, &a->x, &t.x, f);
  fe_mul(&t.y, l, &t.y, f);
  fe_sub(&t.y, &t.y, &a->y, f);
  *out = t;
}

/* L = 3 x^2 / 2 y. A point with y = 0 has order 2, so its double is the
   point at infinity (G1 has none). */
void ec0_double(G1 *out, const G1 *a, const Modulus *f) {
  Fe num;
  Fe den;
  Fe three;
  if (is_zero(&a->y)) {
    infinity(out);
    return;
  }
  fe_from_u64(&three, 3);
  fe_mul(&num, &a->x, &a->x, f);
  fe_mul(&num, &num, &three, f);
  fe_add(&den, &a->y, &a->y, f);
  fe_inv(&den, &den, f);
  fe_mul(&num, &num, &den, f);
  chord(out, &num, a, &a->x, f);
}

/* L = (b.y minus a.y) / (b.x minus a.x). Equal x gives the double, or the
   point at infinity for B = minus A. */
void ec0_add(G1 *out, const G1 *a, const G1 *b, const Modulus *f) {
  Fe num;
  Fe den;
  if (is_infinity(a)) {
    *out = *b;
    return;
  }
  if (is_infinity(b)) {
    *out = *a;
    return;
  }
  if (fe_eq(&a->x, &b->x)) {
    if (fe_eq(&a->y, &b->y))
      ec0_double(out, a, f);
    else
      infinity(out);
    return;
  }
  fe_sub(&num, &b->y, &a->y, f);
  fe_sub(&den, &b->x, &a->x, f);
  fe_inv(&den, &den, f);
  fe_mul(&num, &num, &den, f);
  chord(out, &num, a, &b->x, f);
}

/* The scalar mul keeps a Jacobian point (X, Y, Z), the affine point
   (X / Z^2, Y / Z^3), so a product takes one inverse, not one per step.
   Z = 0 is the point at infinity. */
typedef struct {
  Fe x;
  Fe y;
  Fe z;
} Jac;

/* S = 4 X Y^2, M = 3 X^2, X3 = M^2 minus 2 S, Y3 = M (S minus X3) minus
   8 Y^4, Z3 = 2 Y Z. */
static void jac_double(Jac *out, const Jac *a, const Modulus *f) {
  Fe yy;
  Fe s;
  Fe mm;
  Fe t;
  Jac r;
  if (is_zero(&a->z) || is_zero(&a->y)) {
    memset(out, 0, sizeof *out);
    return;
  }
  fe_mul(&yy, &a->y, &a->y, f);
  fe_mul(&s, &a->x, &yy, f);
  fe_add(&s, &s, &s, f);
  fe_add(&s, &s, &s, f);
  fe_mul(&mm, &a->x, &a->x, f);
  fe_add(&t, &mm, &mm, f);
  fe_add(&mm, &t, &mm, f);
  fe_mul(&r.x, &mm, &mm, f);
  fe_sub(&r.x, &r.x, &s, f);
  fe_sub(&r.x, &r.x, &s, f);
  fe_mul(&yy, &yy, &yy, f);
  fe_add(&yy, &yy, &yy, f);
  fe_add(&yy, &yy, &yy, f);
  fe_add(&yy, &yy, &yy, f);
  fe_sub(&t, &s, &r.x, f);
  fe_mul(&r.y, &mm, &t, f);
  fe_sub(&r.y, &r.y, &yy, f);
  fe_mul(&r.z, &a->y, &a->z, f);
  fe_add(&r.z, &r.z, &r.z, f);
  *out = r;
}

/* A plus the affine point B. U = b.x Z^2, S = b.y Z^3, H = U minus X,
   R = S minus Y; X3 = R^2 minus H^3 minus 2 X H^2,
   Y3 = R (X H^2 minus X3) minus Y H^3, Z3 = Z H. */
static void jac_add_affine(Jac *out, const Jac *a, const G1 *b, const Modulus *f) {
  Fe zz;
  Fe u;
  Fe s;
  Fe h;
  Fe hh;
  Fe hhh;
  Fe v;
  Fe t;
  Jac r;
  if (is_infinity(b)) {
    *out = *a;
    return;
  }
  if (is_zero(&a->z)) {
    out->x = b->x;
    out->y = b->y;
    fe_from_u64(&out->z, 1);
    return;
  }
  fe_mul(&zz, &a->z, &a->z, f);
  fe_mul(&u, &b->x, &zz, f);
  fe_mul(&s, &b->y, &zz, f);
  fe_mul(&s, &s, &a->z, f);
  fe_sub(&h, &u, &a->x, f);
  fe_sub(&s, &s, &a->y, f);
  if (is_zero(&h)) {
    if (is_zero(&s))
      jac_double(out, a, f);
    else
      memset(out, 0, sizeof *out);
    return;
  }
  fe_mul(&hh, &h, &h, f);
  fe_mul(&hhh, &h, &hh, f);
  fe_mul(&v, &a->x, &hh, f);
  fe_mul(&r.x, &s, &s, f);
  fe_sub(&r.x, &r.x, &hhh, f);
  fe_sub(&r.x, &r.x, &v, f);
  fe_sub(&r.x, &r.x, &v, f);
  fe_sub(&t, &v, &r.x, f);
  fe_mul(&r.y, &s, &t, f);
  fe_mul(&t, &a->y, &hhh, f);
  fe_sub(&r.y, &r.y, &t, f);
  fe_mul(&r.z, &a->z, &h, f);
  *out = r;
}

void ec0_mul(G1 *out, const G1 *p, const U256 *k, const Modulus *f) {
  uint32_t e[BN_LIMBS];
  Jac acc;
  Fe zi;
  Fe zi2;
  unsigned i;
  u256_limbs(e, k);
  memset(&acc, 0, sizeof acc);
  for (i = BN_LIMBS * 32u; i > 0; i--) {
    jac_double(&acc, &acc, f);
    if ((e[(i - 1) / 32u] >> ((i - 1) % 32u)) & 1u)
      jac_add_affine(&acc, &acc, p, f);
  }
  if (is_zero(&acc.z)) {
    infinity(out);
    return;
  }
  fe_inv(&zi, &acc.z, f);
  fe_mul(&zi2, &zi, &zi, f);
  fe_mul(&out->x, &acc.x, &zi2, f);
  fe_mul(&zi2, &zi2, &zi, f);
  fe_mul(&out->y, &acc.y, &zi2, f);
}

void g1_double(G1 *out, const G1 *a) {
  ec0_double(out, a, &BN254_FP);
}

void g1_add(G1 *out, const G1 *a, const G1 *b) {
  ec0_add(out, a, b, &BN254_FP);
}

void g1_mul(G1 *out, const G1 *p, const U256 *k) {
  ec0_mul(out, p, k, &BN254_FP);
}
