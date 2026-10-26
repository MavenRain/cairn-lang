/* The prime order group of the Sigma, DLEQ, ElGamal, Shamir and bLSAG code
   (slice S4, survey section 4, ruling Q-S0-2 (a)): secp256k1, the curve
   y^2 = x^3 + 7 over Fp with the generator G and the order n of SEC 2
   2.4.1. front/secp256k1.c implements it. The callers see no base field p:
   the scalar arithmetic uses the fe_* of front/field.h with GROUP_N.

   p = 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f
   n = 0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141 */
#ifndef LANG_FRONT_GROUP_H
#define LANG_FRONT_GROUP_H

#include "front/field.h"
#include "front/u256.h"

/* An affine point. (0, 0) is the point at infinity: 0 is not 0 + 7. */
typedef struct {
  Fe x;
  Fe y;
} GroupPoint;

/* The order n. */
extern const Modulus GROUP_N;

/* G of SEC 2 2.4.1. */
void group_gen(GroupPoint *out);
/* 1 when P is the point at infinity or y^2 = x^3 + 7. */
int group_on_curve(const GroupPoint *p);
/* Returns 1, or 0 when X or Y is p or more, or (X, Y) is not on the curve. */
int group_from_words(GroupPoint *out, const U256 *x, const U256 *y);
void group_to_words(U256 *x, U256 *y, const GroupPoint *p);
void group_add(GroupPoint *out, const GroupPoint *a, const GroupPoint *b);
/* K P for any 256-bit K. */
void group_mul(GroupPoint *out, const GroupPoint *p, const U256 *k);
void group_neg(GroupPoint *out, const GroupPoint *p);
int group_eq(const GroupPoint *a, const GroupPoint *b);
/* The point with x = X and the smaller of its two y (the bLSAG hash to the
   curve, ruling Q-S4-1 (a)). Returns 0 when X is p or more or no x on the
   curve. */
int group_lift_x(GroupPoint *out, const U256 *x);

#endif
