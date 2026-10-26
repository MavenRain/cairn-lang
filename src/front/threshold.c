/* Threshold opening of exponential ElGamal over secp256k1 in the host
   (slice S6 b1). See front/threshold.h. */
#include <string.h>

#include "front/threshold.h"

#include "front/blsag.h"
#include "sha256.h"

const U256 THR_TAG = {{0x7368616d6972u, 0u, 0u, 0u}};
const U256 DLEQ_TAG = {{0x646c6571u, 0u, 0u, 0u}};
const U256 DLEQ_NONCE_TAG = {{0x646c657177u, 0u, 0u, 0u}};

void thr_hash(U256 *out, const U256 *a, const U256 *b) {
  sha256_words(out, a, b);
}

void thr_challenge(U256 *e, const GroupPoint *key, const GroupPoint *c1, const GroupPoint *d, const GroupPoint *a, const GroupPoint *b) {
  const GroupPoint *points[5];
  U256 x;
  U256 y;
  uint32_t i;
  points[0] = key;
  points[1] = c1;
  points[2] = d;
  points[3] = a;
  points[4] = b;
  *e = DLEQ_TAG;
  for (i = 0; i < 5u; i++) {
    group_to_words(&x, &y, points[i]);
    thr_hash(e, e, &x);
    thr_hash(e, e, &y);
  }
}

void thr_nonce(U256 *out, const U256 *seed, uint64_t k) {
  U256 domain;
  U256 w;
  thr_hash(&domain, &THR_TAG, seed);
  u256_from_u64(&w, k);
  thr_hash(&w, &domain, &w);
  blsag_reduce(out, &w);
}

int thr_deal(ThrShare *shares, GroupPoint *pk, const U256 *sk, const U256 *seed, uint32_t t, uint32_t n) {
  Fe coef[THR_N_MAX];
  Fe x;
  Fe acc;
  Fe tmp;
  U256 w;
  GroupPoint g;
  uint32_t i;
  uint32_t k;
  if (t < 1u || t > n || n > THR_N_MAX || !fe_from_u256(&coef[0], sk, &GROUP_N))
    return 0;
  for (k = 1; k < t; k++) {
    thr_nonce(&w, seed, k);
    fe_from_u256(&coef[k], &w, &GROUP_N);
  }
  elg_generator(&g);
  for (i = 1; i <= n; i++) {
    /* f(i) by Horner from a_(t - 1) down to a_0 = sk. */
    fe_from_u64(&x, i);
    acc = coef[t - 1u];
    for (k = t - 1u; k > 0; k--) {
      fe_mul(&tmp, &acc, &x, &GROUP_N);
      fe_add(&acc, &tmp, &coef[k - 1u], &GROUP_N);
    }
    shares[i - 1u].x = i;
    fe_to_u256(&shares[i - 1u].s, &acc);
    group_mul(&shares[i - 1u].key, &g, &shares[i - 1u].s);
  }
  elg_public(pk, sk);
  return 1;
}

void thr_partial(ThrPart *out, const ThrShare *share, const ElgCipher *ct) {
  U256 w;
  U256 e;
  U256 c1x;
  U256 c1y;
  Fe fw;
  Fe fe;
  Fe fs;
  Fe es;
  Fe fz;
  GroupPoint g;
  group_to_words(&c1x, &c1y, &ct->c1);
  thr_hash(&w, &DLEQ_NONCE_TAG, &share->s);
  thr_hash(&w, &w, &c1x);
  thr_hash(&w, &w, &c1y);
  blsag_reduce(&w, &w);
  out->x = share->x;
  group_mul(&out->d, &ct->c1, &share->s);
  elg_generator(&g);
  group_mul(&out->a, &g, &w);
  group_mul(&out->b, &ct->c1, &w);
  thr_challenge(&e, &share->key, &ct->c1, &out->d, &out->a, &out->b);
  blsag_reduce(&e, &e);
  fe_from_u256(&fw, &w, &GROUP_N);
  fe_from_u256(&fe, &e, &GROUP_N);
  fe_from_u256(&fs, &share->s, &GROUP_N);
  fe_mul(&es, &fe, &fs, &GROUP_N);
  fe_add(&fz, &fw, &es, &GROUP_N);
  fe_to_u256(&out->z, &fz);
}

int thr_verify(const ThrPart *part, const GroupPoint *key, const ElgCipher *ct) {
  U256 e;
  GroupPoint g;
  GroupPoint l1;
  GroupPoint r1;
  GroupPoint l2;
  GroupPoint r2;
  thr_challenge(&e, key, &ct->c1, &part->d, &part->a, &part->b);
  elg_generator(&g);
  group_mul(&l1, &g, &part->z);
  group_mul(&r1, key, &e);
  group_add(&r1, &part->a, &r1);
  group_mul(&l2, &ct->c1, &part->z);
  group_mul(&r2, &part->d, &e);
  group_add(&r2, &part->b, &r2);
  return group_eq(&l1, &r1) && group_eq(&l2, &r2);
}

void thr_lagrange(U256 *out, const ThrPart *parts, uint32_t k, uint32_t i) {
  Fe xi;
  Fe xj;
  Fe den;
  Fe inv;
  Fe tmp;
  Fe lambda;
  uint32_t j;
  fe_from_u64(&xi, parts[i].x);
  fe_from_u64(&lambda, 1u);
  for (j = 0; j < k; j++) {
    if (j != i) {
      fe_from_u64(&xj, parts[j].x);
      fe_sub(&den, &xj, &xi, &GROUP_N);
      fe_inv(&inv, &den, &GROUP_N);
      fe_mul(&tmp, &lambda, &xj, &GROUP_N);
      fe_mul(&lambda, &tmp, &inv, &GROUP_N);
    }
  }
  fe_to_u256(out, &lambda);
}

ThrResult thr_open(uint64_t *m, const ThrPart *parts, uint32_t k, const GroupPoint *keys, uint32_t t, uint32_t n, const ElgCipher *ct, uint32_t bits) {
  uint32_t seen = 0;
  uint32_t i;
  ThrResult res = THR_REFUSED;
  U256 lambda;
  GroupPoint sum;
  GroupPoint term;
  ElgResult got;
  if (bits > ELG_BITS_MAX || n > THR_N_MAX || t < 1u || t > n || k > THR_N_MAX || !group_on_curve(&ct->c1) || !group_on_curve(&ct->c2))
    return THR_REFUSED;
  for (i = 0; i < n; i++)
    if (!group_on_curve(&keys[i]))
      return THR_REFUSED;
  for (i = 0; i < k; i++)
    if (!group_on_curve(&parts[i].d) || !group_on_curve(&parts[i].a) || !group_on_curve(&parts[i].b))
      return THR_REFUSED;
  if (k < t)
    return THR_TOO_FEW;
  for (i = 0; i < k; i++) {
    if (parts[i].x == 0u || parts[i].x > n || ((seen >> parts[i].x) & 1u) != 0u)
      return THR_BAD_INDEX;
    seen |= 1u << parts[i].x;
  }
  for (i = 0; i < k; i++)
    if (!thr_verify(&parts[i], &keys[parts[i].x - 1u], ct))
      return THR_BAD_PROOF;
  /* (0, 0) is the point at infinity. */
  memset(&sum, 0, sizeof sum);
  for (i = 0; i < k; i++) {
    thr_lagrange(&lambda, parts, k, i);
    group_mul(&term, &parts[i].d, &lambda);
    group_add(&sum, &sum, &term);
  }
  group_neg(&sum, &sum);
  group_add(&sum, &ct->c2, &sum);
  got = elg_log(m, &sum, bits);
  switch (got) {
  case ELG_FOUND:
    res = THR_FOUND;
    break;
  case ELG_NOT_FOUND:
    res = THR_NOT_FOUND;
    break;
  case ELG_REFUSED:
    res = THR_REFUSED;
    break;
  case ELG_NO_MEMORY:
    res = THR_NO_MEMORY;
    break;
  }
  return res;
}
