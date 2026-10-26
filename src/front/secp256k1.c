/* The secp256k1 group of front/group.h (slice S4, survey sections 3 and 4):
   SEC 2 2.4.1 over the fields of front/field.h. The point code came from
   the alt_bn128 code, whose curve was y^2 = x^3 + 3: a = 0 for both curves, so
   the doubling formulas hold unchanged. */
#include "front/group.h"

#include <string.h>

static const Modulus SECP_P = {{0xfffffc2fu, 0xfffffffeu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}};
const Modulus GROUP_N = {{0xd0364141u, 0xbfd25e8cu, 0xaf48a03bu, 0xbaaedce6u, 0xfffffffeu, 0xffffffffu, 0xffffffffu, 0xffffffffu}};

/* (p + 1) / 4: p = 3 mod 4, so A^((p + 1) / 4) is a square root of a square A. */
static const U256 SQRT_EXP = {{0xffffffffbfffff0cull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0x3fffffffffffffffull}};
static const U256 GX = {{0x59f2815b16f81798ull, 0x029bfcdb2dce28d9ull, 0x55a06295ce870b07ull, 0x79be667ef9dcbbacull}};
static const U256 GY = {{0x9c47d08ffb10d4b8ull, 0xfd17b448a6855419ull, 0x5da4fbfc0e1108a8ull, 0x483ada7726a3c465ull}};

static int is_infinity(const GroupPoint *p) {
  return fe_is_zero(&p->x) && fe_is_zero(&p->y);
}

static void infinity(GroupPoint *out) {
  memset(out, 0, sizeof *out);
}

/* X^3 + 7. */
static void curve_rhs(Fe *out, const Fe *x) {
  Fe seven;
  fe_from_u64(&seven, 7);
  fe_mul(out, x, x, &SECP_P);
  fe_mul(out, out, x, &SECP_P);
  fe_add(out, out, &seven, &SECP_P);
}

void group_gen(GroupPoint *out) {
  fe_from_u256(&out->x, &GX, &SECP_P);
  fe_from_u256(&out->y, &GY, &SECP_P);
}

int group_on_curve(const GroupPoint *p) {
  Fe lhs;
  Fe rhs;
  if (is_infinity(p))
    return 1;
  fe_mul(&lhs, &p->y, &p->y, &SECP_P);
  curve_rhs(&rhs, &p->x);
  return fe_eq(&lhs, &rhs);
}

int group_from_words(GroupPoint *out, const U256 *x, const U256 *y) {
  GroupPoint t;
  if (!fe_from_u256(&t.x, x, &SECP_P) || !fe_from_u256(&t.y, y, &SECP_P) || !group_on_curve(&t))
    return 0;
  *out = t;
  return 1;
}

void group_to_words(U256 *x, U256 *y, const GroupPoint *p) {
  fe_to_u256(x, &p->x);
  fe_to_u256(y, &p->y);
}

void group_neg(GroupPoint *out, const GroupPoint *p) {
  out->x = p->x;
  fe_neg(&out->y, &p->y, &SECP_P);
}

int group_eq(const GroupPoint *a, const GroupPoint *b) {
  return fe_eq(&a->x, &b->x) && fe_eq(&a->y, &b->y);
}

int group_lift_x(GroupPoint *out, const U256 *x) {
  Fe fx;
  Fe rhs;
  Fe y;
  Fe sq;
  Fe neg;
  if (!fe_from_u256(&fx, x, &SECP_P))
    return 0;
  curve_rhs(&rhs, &fx);
  fe_pow(&y, &rhs, &SQRT_EXP, &SECP_P);
  fe_mul(&sq, &y, &y, &SECP_P);
  if (!fe_eq(&sq, &rhs))
    return 0;
  fe_neg(&neg, &y, &SECP_P);
  out->x = fx;
  out->y = y;
  if (fe_lt(&neg, &y))
    out->y = neg;
  return 1;
}

/* The point with the slope L through A: x3 = L^2 minus a.x minus b.x,
   y3 = L (a.x minus x3) minus a.y. */
static void chord(GroupPoint *out, const Fe *l, const GroupPoint *a, const Fe *bx) {
  GroupPoint t;
  fe_mul(&t.x, l, l, &SECP_P);
  fe_sub(&t.x, &t.x, &a->x, &SECP_P);
  fe_sub(&t.x, &t.x, bx, &SECP_P);
  fe_sub(&t.y, &a->x, &t.x, &SECP_P);
  fe_mul(&t.y, l, &t.y, &SECP_P);
  fe_sub(&t.y, &t.y, &a->y, &SECP_P);
  *out = t;
}

/* L = 3 x^2 / 2 y. A point with y = 0 has order 2, so its double is the
   point at infinity (the group of prime order n has none). */
static void point_double(GroupPoint *out, const GroupPoint *a) {
  Fe num;
  Fe den;
  Fe three;
  if (fe_is_zero(&a->y)) {
    infinity(out);
    return;
  }
  fe_from_u64(&three, 3);
  fe_mul(&num, &a->x, &a->x, &SECP_P);
  fe_mul(&num, &num, &three, &SECP_P);
  fe_add(&den, &a->y, &a->y, &SECP_P);
  fe_inv(&den, &den, &SECP_P);
  fe_mul(&num, &num, &den, &SECP_P);
  chord(out, &num, a, &a->x);
}

/* L = (b.y minus a.y) / (b.x minus a.x). Equal x gives the double, or the
   point at infinity for B = minus A. */
void group_add(GroupPoint *out, const GroupPoint *a, const GroupPoint *b) {
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
      point_double(out, a);
    else
      infinity(out);
    return;
  }
  fe_sub(&num, &b->y, &a->y, &SECP_P);
  fe_sub(&den, &b->x, &a->x, &SECP_P);
  fe_inv(&den, &den, &SECP_P);
  fe_mul(&num, &num, &den, &SECP_P);
  chord(out, &num, a, &b->x);
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
static void jac_double(Jac *out, const Jac *a) {
  Fe yy;
  Fe s;
  Fe mm;
  Fe t;
  Jac r;
  if (fe_is_zero(&a->z) || fe_is_zero(&a->y)) {
    memset(out, 0, sizeof *out);
    return;
  }
  fe_mul(&yy, &a->y, &a->y, &SECP_P);
  fe_mul(&s, &a->x, &yy, &SECP_P);
  fe_add(&s, &s, &s, &SECP_P);
  fe_add(&s, &s, &s, &SECP_P);
  fe_mul(&mm, &a->x, &a->x, &SECP_P);
  fe_add(&t, &mm, &mm, &SECP_P);
  fe_add(&mm, &t, &mm, &SECP_P);
  fe_mul(&r.x, &mm, &mm, &SECP_P);
  fe_sub(&r.x, &r.x, &s, &SECP_P);
  fe_sub(&r.x, &r.x, &s, &SECP_P);
  fe_mul(&yy, &yy, &yy, &SECP_P);
  fe_add(&yy, &yy, &yy, &SECP_P);
  fe_add(&yy, &yy, &yy, &SECP_P);
  fe_add(&yy, &yy, &yy, &SECP_P);
  fe_sub(&t, &s, &r.x, &SECP_P);
  fe_mul(&r.y, &mm, &t, &SECP_P);
  fe_sub(&r.y, &r.y, &yy, &SECP_P);
  fe_mul(&r.z, &a->y, &a->z, &SECP_P);
  fe_add(&r.z, &r.z, &r.z, &SECP_P);
  *out = r;
}

/* A plus the affine point B. U = b.x Z^2, S = b.y Z^3, H = U minus X,
   R = S minus Y; X3 = R^2 minus H^3 minus 2 X H^2,
   Y3 = R (X H^2 minus X3) minus Y H^3, Z3 = Z H. */
static void jac_add_affine(Jac *out, const Jac *a, const GroupPoint *b) {
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
  if (fe_is_zero(&a->z)) {
    out->x = b->x;
    out->y = b->y;
    fe_from_u64(&out->z, 1);
    return;
  }
  fe_mul(&zz, &a->z, &a->z, &SECP_P);
  fe_mul(&u, &b->x, &zz, &SECP_P);
  fe_mul(&s, &b->y, &zz, &SECP_P);
  fe_mul(&s, &s, &a->z, &SECP_P);
  fe_sub(&h, &u, &a->x, &SECP_P);
  fe_sub(&s, &s, &a->y, &SECP_P);
  if (fe_is_zero(&h)) {
    if (fe_is_zero(&s))
      jac_double(out, a);
    else
      memset(out, 0, sizeof *out);
    return;
  }
  fe_mul(&hh, &h, &h, &SECP_P);
  fe_mul(&hhh, &h, &hh, &SECP_P);
  fe_mul(&v, &a->x, &hh, &SECP_P);
  fe_mul(&r.x, &s, &s, &SECP_P);
  fe_sub(&r.x, &r.x, &hhh, &SECP_P);
  fe_sub(&r.x, &r.x, &v, &SECP_P);
  fe_sub(&r.x, &r.x, &v, &SECP_P);
  fe_sub(&t, &v, &r.x, &SECP_P);
  fe_mul(&r.y, &s, &t, &SECP_P);
  fe_mul(&t, &a->y, &hhh, &SECP_P);
  fe_sub(&r.y, &r.y, &t, &SECP_P);
  fe_mul(&r.z, &a->z, &h, &SECP_P);
  *out = r;
}

/* Bit I of K is bit I % 64 of limb I / 64 (limb 0 least significant). */
void group_mul(GroupPoint *out, const GroupPoint *p, const U256 *k) {
  Jac acc;
  Fe zi;
  Fe zi2;
  unsigned i;
  memset(&acc, 0, sizeof acc);
  for (i = 256u; i > 0; i--) {
    jac_double(&acc, &acc);
    if ((k->limb[(i - 1) / 64u] >> ((i - 1) % 64u)) & 1u)
      jac_add_affine(&acc, &acc, p);
  }
  if (fe_is_zero(&acc.z)) {
    infinity(out);
    return;
  }
  fe_inv(&zi, &acc.z, &SECP_P);
  fe_mul(&zi2, &zi, &zi, &SECP_P);
  fe_mul(&out->x, &acc.x, &zi2, &SECP_P);
  fe_mul(&zi2, &zi2, &zi, &SECP_P);
  fe_mul(&out->y, &acc.y, &zi2, &SECP_P);
}
