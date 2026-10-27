/* Distributed key generation and resharing in the host (slice S6 b5). See
   front/dkg.h. */
#include <string.h>

#include "front/dkg.h"

#include "front/blsag.h"

const U256 DKG_TAG = {{0x646b67u, 0u, 0u, 0u}};
const U256 DKG_NONCE_TAG = {{0x646b6777u, 0u, 0u, 0u}};

/* 1 when 1 <= K <= THR_N_MAX, N <= THR_N_MAX and the K IDS are 1 to N with
   no repeat. */
static int ids_ok(const uint32_t *ids, uint32_t k, uint32_t n) {
  uint32_t seen = 0;
  uint32_t m;
  if (k < 1u || k > THR_N_MAX || n > THR_N_MAX)
    return 0;
  for (m = 0; m < k; m++) {
    if (ids[m] == 0u || ids[m] > n || ((seen >> ids[m]) & 1u) != 0u)
      return 0;
    seen |= 1u << ids[m];
  }
  return 1;
}

/* lambda at 0 of IDS[I] over the K IDS (thr_lagrange). */
static void ids_lagrange(U256 *out, const uint32_t *ids, uint32_t k, uint32_t i) {
  ThrPart xs[THR_N_MAX];
  uint32_t m;
  memset(xs, 0, sizeof xs);
  for (m = 0; m < k; m++)
    xs[m].x = ids[m];
  thr_lagrange(out, xs, k, i);
}

void dkg_challenge(U256 *c, uint32_t i, const GroupPoint *a0, const GroupPoint *r) {
  U256 x;
  U256 y;
  u256_from_u64(&x, i);
  thr_hash(c, &DKG_TAG, &x);
  group_to_words(&x, &y, a0);
  thr_hash(c, c, &x);
  thr_hash(c, c, &y);
  group_to_words(&x, &y, r);
  thr_hash(c, c, &x);
  thr_hash(c, c, &y);
}

int dkg_deal(DkgDeal *out, U256 *shares, const U256 *a0, const U256 *seed, uint32_t i, uint32_t t, uint32_t n) {
  Fe coef[THR_N_MAX];
  Fe x;
  Fe acc;
  Fe tmp;
  Fe fw;
  Fe fc;
  U256 w;
  U256 c;
  GroupPoint g;
  uint32_t j;
  uint32_t k;
  if (i < 1u || i > THR_N_MAX || t < 1u || t > n || n > THR_N_MAX || !fe_from_u256(&coef[0], a0, &GROUP_N))
    return 0;
  for (k = 1; k < t; k++) {
    thr_nonce(&w, seed, k);
    fe_from_u256(&coef[k], &w, &GROUP_N);
  }
  elg_generator(&g);
  memset(out, 0, sizeof *out);
  out->i = i;
  out->t = t;
  for (k = 0; k < t; k++) {
    fe_to_u256(&w, &coef[k]);
    group_mul(&out->commit[k], &g, &w);
  }
  /* The Schnorr proof of a_0: R = w G, mu = w + c a_0. */
  thr_hash(&w, &DKG_NONCE_TAG, seed);
  thr_hash(&w, &w, a0);
  u256_from_u64(&c, i);
  thr_hash(&w, &w, &c);
  blsag_reduce(&w, &w);
  group_mul(&out->r, &g, &w);
  dkg_challenge(&c, i, &out->commit[0], &out->r);
  blsag_reduce(&c, &c);
  fe_from_u256(&fw, &w, &GROUP_N);
  fe_from_u256(&fc, &c, &GROUP_N);
  fe_mul(&tmp, &fc, &coef[0], &GROUP_N);
  fe_add(&acc, &fw, &tmp, &GROUP_N);
  fe_to_u256(&out->mu, &acc);
  for (j = 1; j <= n; j++) {
    /* f(j) by Horner from a_(t - 1) down to a_0, as in thr_deal. */
    fe_from_u64(&x, j);
    acc = coef[t - 1u];
    for (k = t - 1u; k > 0; k--) {
      fe_mul(&tmp, &acc, &x, &GROUP_N);
      fe_add(&acc, &tmp, &coef[k - 1u], &GROUP_N);
    }
    fe_to_u256(&shares[j - 1u], &acc);
  }
  return 1;
}

int dkg_verify(const DkgDeal *deal, const U256 *share, uint32_t j) {
  U256 c;
  U256 jw;
  Fe f;
  GroupPoint g;
  GroupPoint l;
  GroupPoint r;
  GroupPoint tmp;
  uint32_t k;
  if (deal->t < 1u || deal->t > THR_N_MAX || j < 1u || !fe_from_u256(&f, share, &GROUP_N) || !fe_from_u256(&f, &deal->mu, &GROUP_N) || !group_on_curve(&deal->r))
    return 0;
  for (k = 0; k < deal->t; k++)
    if (!group_on_curve(&deal->commit[k]))
      return 0;
  /* The proof: mu G = R + c A_0. */
  elg_generator(&g);
  dkg_challenge(&c, deal->i, &deal->commit[0], &deal->r);
  blsag_reduce(&c, &c);
  group_mul(&l, &g, &deal->mu);
  group_mul(&tmp, &deal->commit[0], &c);
  group_add(&r, &deal->r, &tmp);
  if (!group_eq(&l, &r))
    return 0;
  /* The share: s G = sum_k j^k A_k, by Horner from A_(t - 1) down to A_0. */
  u256_from_u64(&jw, j);
  r = deal->commit[deal->t - 1u];
  for (k = deal->t - 1u; k > 0; k--) {
    group_mul(&tmp, &r, &jw);
    group_add(&r, &tmp, &deal->commit[k - 1u]);
  }
  group_mul(&l, &g, share);
  return group_eq(&l, &r);
}

static int combine(ThrShare *out, GroupPoint *pk, const DkgDeal *deals, const U256 *shares, uint32_t k, uint32_t j) {
  uint32_t ids[THR_N_MAX];
  uint32_t m;
  Fe acc;
  Fe fs;
  Fe tmp;
  GroupPoint g;
  GroupPoint sum;
  if (k < 1u || k > THR_N_MAX || j < 1u || j > THR_N_MAX)
    return 0;
  for (m = 0; m < k; m++)
    ids[m] = deals[m].i;
  if (!ids_ok(ids, k, THR_N_MAX))
    return 0;
  for (m = 0; m < k; m++)
    if (deals[m].t != deals[0].t || !dkg_verify(&deals[m], &shares[m], j))
      return 0;
  fe_from_u64(&acc, 0u);
  /* (0, 0) is the point at infinity. */
  memset(&sum, 0, sizeof sum);
  for (m = 0; m < k; m++) {
    fe_from_u256(&fs, &shares[m], &GROUP_N);
    fe_add(&tmp, &acc, &fs, &GROUP_N);
    acc = tmp;
    group_add(&sum, &sum, &deals[m].commit[0]);
  }
  out->x = j;
  fe_to_u256(&out->s, &acc);
  elg_generator(&g);
  group_mul(&out->key, &g, &out->s);
  *pk = sum;
  return 1;
}

int dkg_combine(ThrShare *out, GroupPoint *pk, const DkgDeal *deals, const U256 *shares, uint32_t k, uint32_t t, uint32_t n, uint32_t j) {
  uint32_t m;
  if (t < 1u || t > n || n > THR_N_MAX || k != n || j < 1u || j > n)
    return 0;
  for (m = 0; m < k; m++)
    if (deals[m].t != t || deals[m].i > n)
      return 0;
  return combine(out, pk, deals, shares, k, j);
}

int dkg_reshare(DkgDeal *out, U256 *shares, const ThrShare *old, const uint32_t *quorum, uint32_t q, const U256 *seed, uint32_t t, uint32_t n) {
  U256 lambda;
  U256 a0;
  Fe fl;
  Fe fs;
  Fe fa;
  uint32_t pos = q;
  uint32_t m;
  if (!ids_ok(quorum, q, THR_N_MAX) || !fe_from_u256(&fs, &old->s, &GROUP_N))
    return 0;
  for (m = 0; m < q; m++)
    if (quorum[m] == old->x)
      pos = m;
  if (pos == q)
    return 0;
  /* a_0 = lambda_i(Q) s_i: the weights keep the public key. */
  ids_lagrange(&lambda, quorum, q, pos);
  fe_from_u256(&fl, &lambda, &GROUP_N);
  fe_mul(&fa, &fl, &fs, &GROUP_N);
  fe_to_u256(&a0, &fa);
  return dkg_deal(out, shares, &a0, seed, old->x, t, n);
}

int dkg_reshare_combine(ThrShare *out, const DkgDeal *deals, const U256 *shares, uint32_t q, const GroupPoint *keys, uint32_t old_t, uint32_t old_n, const GroupPoint *pk, uint32_t j) {
  uint32_t ids[THR_N_MAX];
  uint32_t m;
  U256 lambda;
  GroupPoint want;
  GroupPoint sum;
  GroupPoint got;
  if (old_t < 1u || q < old_t || q > old_n || old_n > THR_N_MAX)
    return 0;
  for (m = 0; m < q; m++)
    ids[m] = deals[m].i;
  if (!ids_ok(ids, q, old_n))
    return 0;
  /* (0, 0) is the point at infinity. */
  memset(&sum, 0, sizeof sum);
  for (m = 0; m < q; m++) {
    ids_lagrange(&lambda, ids, q, m);
    group_mul(&want, &keys[ids[m] - 1u], &lambda);
    if (!group_eq(&deals[m].commit[0], &want))
      return 0;
    group_add(&sum, &sum, &deals[m].commit[0]);
  }
  /* The sum of the constant commitments must be the public key. */
  if (!group_eq(&sum, pk))
    return 0;
  return combine(out, &got, deals, shares, q, j);
}
