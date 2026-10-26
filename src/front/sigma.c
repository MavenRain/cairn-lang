/* Sigma verifiers over exponential ElGamal in the host (slice S7 b2). See
   front/sigma.h. */
#include "front/sigma.h"

#include "front/blsag.h"
#include "front/threshold.h"

const U256 RAND_TAG = {{0x72616e64u, 0u, 0u, 0u}};
const U256 ONEHOT_TAG = {{0x6f6e65686f74u, 0u, 0u, 0u}};
const U256 RAND_NONCE_TAG = {{0x72616e6477u, 0u, 0u, 0u}};
const U256 ONEHOT_NONCE_TAG = {{0x6f6e65686f7477u, 0u, 0u, 0u}};
const U256 BALLOT_TAG = {{0x62616c6c6f74u, 0u, 0u, 0u}};

/* A + B modulo 2^256 (u256_add traps on an overflow). OUT can be A or B. */
static void wrap_add(U256 *out, const U256 *a, const U256 *b) {
  uint64_t carry = 0;
  uint64_t s;
  uint64_t t;
  uint64_t c;
  uint32_t i;
  for (i = 0; i < 4u; i++) {
    s = a->limb[i] + b->limb[i];
    c = s < a->limb[i];
    t = s + carry;
    carry = c | (t < s);
    out->limb[i] = t;
  }
}

/* A - B modulo 2^256 (u256_sub stops at 0). OUT can be A or B. */
static void wrap_sub(U256 *out, const U256 *a, const U256 *b) {
  uint64_t borrow = 0;
  uint64_t d;
  uint64_t t;
  uint64_t c;
  uint32_t i;
  for (i = 0; i < 4u; i++) {
    d = a->limb[i] - b->limb[i];
    c = a->limb[i] < b->limb[i];
    t = d - borrow;
    borrow = c | (d < borrow);
    out->limb[i] = t;
  }
}

/* ACC = thr_hash(thr_hash(ACC, P.x), P.y). */
static void absorb(U256 *acc, const G1 *p) {
  U256 x;
  U256 y;
  g1_to_words(&x, &y, p);
  thr_hash(acc, acc, &x);
  thr_hash(acc, acc, &y);
}

/* The chain of thr_hash from TAG over the secret X, CTX, PK, C1 and C2. */
static void secret_chain(U256 *out, const U256 *tag, const U256 *x, const U256 *ctx, const G1 *pk, const ElgCipher *ct) {
  thr_hash(out, tag, x);
  thr_hash(out, out, ctx);
  absorb(out, pk);
  absorb(out, &ct->c1);
  absorb(out, &ct->c2);
}

/* thr_hash(BASE, I). */
static void draw(U256 *out, const U256 *base, uint64_t i) {
  U256 w;
  u256_from_u64(&w, i);
  thr_hash(out, base, &w);
}

/* z = w + (e mod r) x mod r, as thr_partial. */
static void respond(U256 *z, const U256 *w, const U256 *e, const U256 *x) {
  U256 er;
  Fe fw;
  Fe fe;
  Fe fx;
  Fe ex;
  Fe fz;
  blsag_reduce(&er, e);
  fe_from_u256(&fw, w, &BN254_FR);
  fe_from_u256(&fe, &er, &BN254_FR);
  fe_from_u256(&fx, x, &BN254_FR);
  fe_mul(&ex, &fe, &fx, &BN254_FR);
  fe_add(&fz, &fw, &ex, &BN254_FR);
  fe_to_u256(z, &fz);
}

/* 1 when z P = A + e Q. */
static int eq_holds(const U256 *z, const G1 *p, const G1 *a, const U256 *e, const G1 *q) {
  G1 l;
  G1 r;
  g1_mul(&l, p, z);
  g1_mul(&r, q, e);
  g1_add(&r, a, &r);
  return g1_eq(&l, &r);
}

static int ct_on_curve(const G1 *pk, const ElgCipher *ct) {
  return g1_on_curve(pk) && g1_on_curve(&ct->c1) && g1_on_curve(&ct->c2);
}

static int or_on_curve(const SigOr *proof) {
  return g1_on_curve(&proof->a0) && g1_on_curve(&proof->b0) && g1_on_curve(&proof->a1) && g1_on_curve(&proof->b1);
}

/* The pointwise sum of the K >= 1 ciphertexts CTS. */
static void ballot_sum(ElgCipher *out, const ElgCipher *cts, uint32_t k) {
  uint32_t j;
  *out = cts[0];
  for (j = 1; j < k; j++)
    elg_add(out, out, &cts[j]);
}

void sig_plain(G1 *out, const ElgCipher *ct, const U256 *m) {
  G1 g;
  G1 mg;
  elg_generator(&g);
  g1_mul(&mg, &g, m);
  g1_neg(&mg, &mg);
  g1_add(out, &ct->c2, &mg);
}

void sig_rand_challenge(U256 *e, const G1 *pk, const ElgCipher *ct, const U256 *m, const G1 *a, const G1 *b, const U256 *ctx) {
  thr_hash(e, &RAND_TAG, ctx);
  absorb(e, pk);
  absorb(e, &ct->c1);
  absorb(e, &ct->c2);
  thr_hash(e, e, m);
  absorb(e, a);
  absorb(e, b);
}

void sig_or_challenge(U256 *e, const G1 *pk, const ElgCipher *ct, const SigOr *proof, const U256 *ctx, uint32_t j) {
  U256 slot;
  u256_from_u64(&slot, j);
  thr_hash(e, &ONEHOT_TAG, ctx);
  thr_hash(e, e, &slot);
  absorb(e, pk);
  absorb(e, &ct->c1);
  absorb(e, &ct->c2);
  absorb(e, &proof->a0);
  absorb(e, &proof->b0);
  absorb(e, &proof->a1);
  absorb(e, &proof->b1);
}

void sig_simulate(G1 *a, G1 *b, const G1 *pk, const G1 *c1, const G1 *y, const U256 *e, const U256 *z) {
  G1 g;
  G1 t;
  G1 u;
  elg_generator(&g);
  g1_mul(&t, &g, z);
  g1_mul(&u, c1, e);
  g1_neg(&u, &u);
  g1_add(a, &t, &u);
  g1_mul(&t, pk, z);
  g1_mul(&u, y, e);
  g1_neg(&u, &u);
  g1_add(b, &t, &u);
}

SigResult sig_rand_prove(SigDleq *out, const G1 *pk, const ElgCipher *ct, const U256 *m, const U256 *r, const U256 *ctx) {
  U256 rr;
  U256 w;
  U256 e;
  G1 g;
  if (!ct_on_curve(pk, ct))
    return SIG_REFUSED;
  blsag_reduce(&rr, r);
  secret_chain(&w, &RAND_NONCE_TAG, &rr, ctx, pk, ct);
  thr_hash(&w, &w, m);
  blsag_reduce(&w, &w);
  elg_generator(&g);
  g1_mul(&out->a, &g, &w);
  g1_mul(&out->b, pk, &w);
  sig_rand_challenge(&e, pk, ct, m, &out->a, &out->b, ctx);
  respond(&out->z, &w, &e, &rr);
  return SIG_OK;
}

SigResult sig_rand_verify(const SigDleq *proof, const G1 *pk, const ElgCipher *ct, const U256 *m, const U256 *ctx) {
  U256 e;
  G1 g;
  G1 v;
  if (!ct_on_curve(pk, ct) || !g1_on_curve(&proof->a) || !g1_on_curve(&proof->b))
    return SIG_REFUSED;
  elg_generator(&g);
  sig_plain(&v, ct, m);
  sig_rand_challenge(&e, pk, ct, m, &proof->a, &proof->b, ctx);
  if (!eq_holds(&proof->z, &g, &proof->a, &e, &ct->c1))
    return SIG_BAD_SUM;
  /* With the first equation only, a prover with the nonce of c proves any
     plaintext. */
  if (!eq_holds(&proof->z, pk, &proof->b, &e, &v))
    return SIG_BAD_SUM;
  return SIG_OK;
}

SigResult sig_or_prove(SigOr *out, const G1 *pk, const ElgCipher *ct, uint32_t bit, const U256 *r, const U256 *ctx, uint32_t j) {
  G1 y[2];
  G1 a[2];
  G1 b[2];
  U256 e[2];
  U256 z[2];
  U256 m;
  U256 rr;
  U256 base;
  U256 w;
  U256 ch;
  G1 g;
  uint32_t s;
  if (bit > 1u || !ct_on_curve(pk, ct))
    return SIG_REFUSED;
  s = 1u - bit;
  u256_from_u64(&m, 0u);
  sig_plain(&y[0], ct, &m);
  u256_from_u64(&m, 1u);
  sig_plain(&y[1], ct, &m);
  blsag_reduce(&rr, r);
  secret_chain(&base, &ONEHOT_NONCE_TAG, &rr, ctx, pk, ct);
  u256_from_u64(&m, j);
  thr_hash(&base, &base, &m);
  draw(&w, &base, 0u);
  blsag_reduce(&w, &w);
  draw(&e[s], &base, 1u);
  draw(&z[s], &base, 2u);
  blsag_reduce(&z[s], &z[s]);
  elg_generator(&g);
  g1_mul(&a[bit], &g, &w);
  g1_mul(&b[bit], pk, &w);
  sig_simulate(&a[s], &b[s], pk, &ct->c1, &y[s], &e[s], &z[s]);
  out->a0 = a[0];
  out->b0 = b[0];
  out->a1 = a[1];
  out->b1 = b[1];
  sig_or_challenge(&ch, pk, ct, out, ctx, j);
  wrap_sub(&e[bit], &ch, &e[s]);
  respond(&z[bit], &w, &e[bit], &rr);
  out->e0 = e[0];
  out->e1 = e[1];
  out->z0 = z[0];
  out->z1 = z[1];
  return SIG_OK;
}

SigResult sig_or_verify(const SigOr *proof, const G1 *pk, const ElgCipher *ct, const U256 *ctx, uint32_t j) {
  U256 ch;
  U256 sum;
  U256 one;
  G1 g;
  G1 d;
  if (!ct_on_curve(pk, ct) || !or_on_curve(proof))
    return SIG_REFUSED;
  sig_or_challenge(&ch, pk, ct, proof, ctx, j);
  wrap_add(&sum, &proof->e0, &proof->e1);
  /* Without the split, sig_simulate gives both branches for any c. */
  if (u256_cmp(&sum, &ch) != 0)
    return SIG_BAD_SLOT;
  elg_generator(&g);
  u256_from_u64(&one, 1u);
  sig_plain(&d, ct, &one);
  if (!eq_holds(&proof->z0, &g, &proof->a0, &proof->e0, &ct->c1) || !eq_holds(&proof->z0, pk, &proof->b0, &proof->e0, &ct->c2)
      || !eq_holds(&proof->z1, &g, &proof->a1, &proof->e1, &ct->c1) || !eq_holds(&proof->z1, pk, &proof->b1, &proof->e1, &d))
    return SIG_BAD_SLOT;
  return SIG_OK;
}

SigResult sig_onehot_prove(ElgCipher *cts, SigOr *slots, SigDleq *sum, const G1 *pk, uint32_t k, uint32_t choice, const U256 *seed, const U256 *ctx) {
  U256 domain;
  U256 r;
  U256 m;
  U256 total_r;
  Fe acc;
  Fe fr;
  Fe next;
  ElgCipher total;
  uint32_t bit;
  uint32_t j;
  if (k < 1u || k > SIG_K_MAX || choice >= k || !g1_on_curve(pk))
    return SIG_REFUSED;
  thr_hash(&domain, &BALLOT_TAG, seed);
  fe_from_u64(&acc, 0u);
  for (j = 0; j < k; j++) {
    bit = (uint32_t)(j == choice);
    draw(&r, &domain, j);
    blsag_reduce(&r, &r);
    u256_from_u64(&m, bit);
    elg_encrypt(&cts[j], pk, &m, &r);
    sig_or_prove(&slots[j], pk, &cts[j], bit, &r, ctx, j);
    fe_from_u256(&fr, &r, &BN254_FR);
    fe_add(&next, &acc, &fr, &BN254_FR);
    acc = next;
  }
  fe_to_u256(&total_r, &acc);
  ballot_sum(&total, cts, k);
  u256_from_u64(&m, 1u);
  return sig_rand_prove(sum, pk, &total, &m, &total_r, ctx);
}

SigResult sig_onehot_verify(const ElgCipher *cts, const SigOr *slots, const SigDleq *sum, const G1 *pk, uint32_t k, const U256 *ctx) {
  ElgCipher total;
  U256 one;
  uint32_t j;
  if (k < 1u || k > SIG_K_MAX || !g1_on_curve(pk) || !g1_on_curve(&sum->a) || !g1_on_curve(&sum->b))
    return SIG_REFUSED;
  for (j = 0; j < k; j++)
    if (!ct_on_curve(pk, &cts[j]) || !or_on_curve(&slots[j]))
      return SIG_REFUSED;
  for (j = 0; j < k; j++)
    if (sig_or_verify(&slots[j], pk, &cts[j], ctx, j) != SIG_OK)
      return SIG_BAD_SLOT;
  ballot_sum(&total, cts, k);
  u256_from_u64(&one, 1u);
  /* Without the sum proof, two slots that encrypt 1 pass. */
  return sig_rand_verify(sum, pk, &total, &one, ctx);
}
