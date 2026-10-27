/* Threshold Schnorr signatures of the appends in the host (slice S7 b1).
   See front/frost.h. */
#include <string.h>

#include "front/frost.h"

#include "front/blsag.h"

const U256 AUTH_TAG = {{0x69726e2d61757468u, 0x6361u, 0u, 0u}};
const U256 AUTH_NONCE_TAG = {{0x726e2d6175746877u, 0x636169u, 0u, 0u}};
const U256 AUTH_RHO_TAG = {{0x6169726e2d72686fu, 0x63u, 0u, 0u}};

/* (0, 0) is the point at infinity. */
static int is_infinity(const GroupPoint *p) {
  return fe_is_zero(&p->x) && fe_is_zero(&p->y);
}

/* 1 when P, a point on the curve, has the larger of its two y. */
static int larger_y(const GroupPoint *p) {
  GroupPoint low;
  U256 x;
  U256 y;
  group_to_words(&x, &y, p);
  return group_lift_x(&low, &x) && !group_eq(&low, p);
}

/* 1 when 1 <= K <= THR_N_MAX, the indices of the K commitments of LIST rise
   strictly from 1 to at most THR_N_MAX and every point is on the curve. */
static int list_ok(const AuthCommit *list, uint32_t k) {
  uint32_t m;
  if (k < 1u || k > THR_N_MAX)
    return 0;
  for (m = 0; m < k; m++)
    if (list[m].i < 1u || list[m].i > THR_N_MAX || (m > 0u && list[m].i <= list[m - 1u].i) || !group_on_curve(&list[m].d) || !group_on_curve(&list[m].e))
      return 0;
  return 1;
}

/* lambda at 0 of LIST[M].i over the K indices of LIST (thr_lagrange). */
static void list_lagrange(U256 *out, const AuthCommit *list, uint32_t k, uint32_t m) {
  ThrPart xs[THR_N_MAX];
  uint32_t j;
  memset(xs, 0, sizeof xs);
  for (j = 0; j < k; j++)
    xs[j].x = list[j].i;
  thr_lagrange(out, xs, k, m);
}

/* RHOS[m] = the binding factor of LIST[m].i and R = sum_m (D_m + rho_m E_m). */
static void group_commit(GroupPoint *r, U256 *rhos, const U256 *px, const U256 *head, const U256 *e, const AuthCommit *list, uint32_t k) {
  GroupPoint tmp;
  uint32_t m;
  memset(r, 0, sizeof *r);
  for (m = 0; m < k; m++) {
    frost_rho(&rhos[m], list[m].i, px, head, e, list, k);
    group_mul(&tmp, &list[m].e, &rhos[m]);
    group_add(r, r, &tmp);
    group_add(r, r, &list[m].d);
  }
}

/* The nonce K of round 1: the chain of thr_hash from AUTH_NONCE_TAG over
   SEED, S, K, mod GROUP_N. */
static void nonce_word(U256 *out, const U256 *seed, const U256 *s, uint64_t k) {
  U256 kw;
  thr_hash(out, &AUTH_NONCE_TAG, seed);
  thr_hash(out, out, s);
  u256_from_u64(&kw, k);
  thr_hash(out, out, &kw);
  blsag_reduce(out, out);
}

void frost_commit(AuthNonce *nonce, AuthCommit *commit, const ThrShare *share, const U256 *seed) {
  GroupPoint g;
  nonce->i = share->x;
  nonce_word(&nonce->d, seed, &share->s, 0u);
  nonce_word(&nonce->e, seed, &share->s, 1u);
  elg_generator(&g);
  commit->i = share->x;
  group_mul(&commit->d, &g, &nonce->d);
  group_mul(&commit->e, &g, &nonce->e);
}

void frost_rho(U256 *rho, uint32_t i, const U256 *px, const U256 *head, const U256 *e, const AuthCommit *list, uint32_t k) {
  U256 x;
  U256 y;
  uint32_t m;
  u256_from_u64(&x, i);
  thr_hash(rho, &AUTH_RHO_TAG, &x);
  thr_hash(rho, rho, px);
  thr_hash(rho, rho, head);
  thr_hash(rho, rho, e);
  for (m = 0; m < k; m++) {
    u256_from_u64(&x, list[m].i);
    thr_hash(rho, rho, &x);
    group_to_words(&x, &y, &list[m].d);
    thr_hash(rho, rho, &x);
    thr_hash(rho, rho, &y);
    group_to_words(&x, &y, &list[m].e);
    thr_hash(rho, rho, &x);
    thr_hash(rho, rho, &y);
  }
  blsag_reduce(rho, rho);
}

void frost_challenge(U256 *c, const U256 *px, const U256 *rx, const U256 *head, const U256 *e) {
  thr_hash(c, &AUTH_TAG, px);
  thr_hash(c, c, rx);
  thr_hash(c, c, head);
  thr_hash(c, c, e);
  blsag_reduce(c, c);
}

AuthResult frost_sign(AuthShare *out, const ThrShare *share, const AuthNonce *nonce, const GroupPoint *pk, const U256 *head, const U256 *e, const AuthCommit *list, uint32_t k) {
  U256 rhos[THR_N_MAX];
  U256 px;
  U256 py;
  U256 rx;
  U256 ry;
  U256 c;
  U256 lambda;
  Fe sd;
  Fe se;
  Fe ss;
  Fe srho;
  Fe sc;
  Fe sl;
  Fe t1;
  Fe t2;
  Fe acc;
  GroupPoint g;
  GroupPoint dg;
  GroupPoint eg;
  GroupPoint r;
  uint32_t pos = k;
  uint32_t m;
  if (!list_ok(list, k) || nonce->i != share->x || !group_on_curve(pk) || is_infinity(pk) || !fe_from_u256(&ss, &share->s, &GROUP_N) || !fe_from_u256(&sd, &nonce->d, &GROUP_N) || !fe_from_u256(&se, &nonce->e, &GROUP_N))
    return AUTH_REFUSED;
  for (m = 0; m < k; m++)
    if (list[m].i == share->x)
      pos = m;
  if (pos == k)
    return AUTH_REFUSED;
  /* The list must hold the commitments of NONCE. */
  elg_generator(&g);
  group_mul(&dg, &g, &nonce->d);
  group_mul(&eg, &g, &nonce->e);
  if (!group_eq(&dg, &list[pos].d) || !group_eq(&eg, &list[pos].e))
    return AUTH_REFUSED;
  group_to_words(&px, &py, pk);
  group_commit(&r, rhos, &px, head, e, list, k);
  if (is_infinity(&r))
    return AUTH_REFUSED;
  group_to_words(&rx, &ry, &r);
  frost_challenge(&c, &px, &rx, head, e);
  list_lagrange(&lambda, list, k, pos);
  /* x-only: the share of -P when P has the larger y, the nonces of -R when
     R has the larger y. */
  if (larger_y(pk)) {
    fe_neg(&t1, &ss, &GROUP_N);
    ss = t1;
  }
  if (larger_y(&r)) {
    fe_neg(&t1, &sd, &GROUP_N);
    sd = t1;
    fe_neg(&t1, &se, &GROUP_N);
    se = t1;
  }
  fe_from_u256(&srho, &rhos[pos], &GROUP_N);
  fe_from_u256(&sc, &c, &GROUP_N);
  fe_from_u256(&sl, &lambda, &GROUP_N);
  /* z = d + rho e + lambda s c. */
  fe_mul(&t1, &srho, &se, &GROUP_N);
  fe_add(&acc, &sd, &t1, &GROUP_N);
  fe_mul(&t1, &sl, &ss, &GROUP_N);
  fe_mul(&t2, &t1, &sc, &GROUP_N);
  fe_add(&t1, &acc, &t2, &GROUP_N);
  out->i = share->x;
  fe_to_u256(&out->z, &t1);
  return AUTH_OK;
}

AuthResult frost_aggregate(AuthSig *out, uint32_t *at, const AuthShare *shares, const AuthCommit *list, uint32_t k, const GroupPoint *keys, uint32_t t, uint32_t n, const GroupPoint *pk, const U256 *head, const U256 *e) {
  U256 rhos[THR_N_MAX];
  U256 px;
  U256 py;
  U256 rx;
  U256 ry;
  U256 c;
  U256 lambda;
  U256 cl;
  Fe sz;
  Fe sc;
  Fe sl;
  Fe t1;
  Fe acc;
  GroupPoint g;
  GroupPoint r;
  GroupPoint lhs;
  GroupPoint want;
  GroupPoint tmp;
  GroupPoint neg;
  int neg_p;
  int neg_r;
  uint32_t m;
  *at = 0;
  if (t < 1u || t > n || n > THR_N_MAX || k < t || !list_ok(list, k) || !group_on_curve(pk) || is_infinity(pk))
    return AUTH_REFUSED;
  for (m = 0; m < k; m++)
    if (list[m].i > n || shares[m].i != list[m].i || !fe_from_u256(&sz, &shares[m].z, &GROUP_N) || !group_on_curve(&keys[list[m].i - 1u]))
      return AUTH_REFUSED;
  group_to_words(&px, &py, pk);
  group_commit(&r, rhos, &px, head, e, list, k);
  if (is_infinity(&r))
    return AUTH_REFUSED;
  group_to_words(&rx, &ry, &r);
  frost_challenge(&c, &px, &rx, head, e);
  fe_from_u256(&sc, &c, &GROUP_N);
  neg_p = larger_y(pk);
  neg_r = larger_y(&r);
  elg_generator(&g);
  fe_from_u64(&acc, 0u);
  for (m = 0; m < k; m++) {
    /* z_i G = +-(D_i + rho_i E_i) + c lambda_i (+-Y_i), signs as in
       frost_sign. */
    group_mul(&lhs, &g, &shares[m].z);
    group_mul(&tmp, &list[m].e, &rhos[m]);
    group_add(&want, &list[m].d, &tmp);
    group_neg(&neg, &want);
    want = neg_r ? neg : want;
    list_lagrange(&lambda, list, k, m);
    fe_from_u256(&sl, &lambda, &GROUP_N);
    fe_mul(&t1, &sc, &sl, &GROUP_N);
    fe_to_u256(&cl, &t1);
    group_mul(&tmp, &keys[list[m].i - 1u], &cl);
    group_neg(&neg, &tmp);
    group_add(&want, &want, neg_p ? &neg : &tmp);
    if (!group_eq(&lhs, &want)) {
      *at = list[m].i;
      return AUTH_BAD_SHARE;
    }
    fe_from_u256(&sz, &shares[m].z, &GROUP_N);
    fe_add(&t1, &acc, &sz, &GROUP_N);
    acc = t1;
  }
  out->rx = rx;
  fe_to_u256(&out->z, &acc);
  return schnorr_verify(&px, out, head, e);
}

AuthResult schnorr_verify(const U256 *px, const AuthSig *sig, const U256 *head, const U256 *e) {
  U256 c;
  Fe f;
  GroupPoint g;
  GroupPoint p;
  GroupPoint r;
  GroupPoint lhs;
  GroupPoint rhs;
  GroupPoint tmp;
  if (!fe_from_u256(&f, &sig->z, &GROUP_N) || !group_lift_x(&p, px) || !group_lift_x(&r, &sig->rx))
    return AUTH_REFUSED;
  frost_challenge(&c, px, &sig->rx, head, e);
  elg_generator(&g);
  group_mul(&lhs, &g, &sig->z);
  group_mul(&tmp, &p, &c);
  group_add(&rhs, &r, &tmp);
  return group_eq(&lhs, &rhs) ? AUTH_OK : AUTH_BAD_SIG;
}
