/* Host Sigma regressions (slice S7 b2): the proof of randomness, the bit
   proof and the one-hot ballot of front/sigma.c, with the forgeries and
   refusals that the verifiers must reject. With the argument `words` it
   prints the chain constants of examples/onehot.lang (slice S7 b3), one
   `name 0x<word>` per line. */
#include <stdio.h>
#include <string.h>

#include "front/blsag.h"
#include "front/elgamal.h"
#include "front/sigma.h"

#define BITS 8u

static int expect(const char *name, SigResult got, SigResult want) {
  if (got == want)
    return 1;
  fprintf(stderr, "FAIL sigma %s: result %d, want %d\n", name, (int)got, (int)want);
  return 0;
}

static void word(const char *prefix, const char *name, const U256 *w) {
  printf("%s%s 0x%016llx%016llx%016llx%016llx\n", prefix, name, (unsigned long long)w->limb[3], (unsigned long long)w->limb[2],
         (unsigned long long)w->limb[1], (unsigned long long)w->limb[0]);
}

static void point(const char *prefix, const char *x, const char *y, const G1 *p) {
  U256 a;
  U256 b;
  g1_to_words(&a, &b, p);
  word(prefix, x, &a);
  word(prefix, y, &b);
}

/* The ciphertext of M under PK with the nonce r_I; *R gets r_I. */
static void encrypt_u64(ElgCipher *out, U256 *r, const G1 *pk, uint64_t m, uint64_t i) {
  U256 w;
  u256_from_u64(&w, m);
  elg_nonce(r, i);
  elg_encrypt(out, pk, &w, r);
}

/* A bit proof for CT with both branches simulated from fixed words: each of
   the four equations holds and e0 + e1 is not e, so only the split check
   refuses it. On an encryption of 2 it would prove a non-bit. */
static void forge(SigOr *out, const G1 *pk, const ElgCipher *ct) {
  U256 one;
  G1 d;
  blsag_key(&out->e0, 31u);
  blsag_key(&out->e1, 32u);
  blsag_key(&out->z0, 33u);
  blsag_key(&out->z1, 34u);
  u256_from_u64(&one, 1u);
  sig_plain(&d, ct, &one);
  sig_simulate(&out->a0, &out->b0, pk, &ct->c1, &ct->c2, &out->e0, &out->z0);
  sig_simulate(&out->a1, &out->b1, pk, &ct->c1, &d, &out->e1, &out->z1);
}

/* The honest ballot for CHOICE of K: proved, verified, and each slot opens
   to its bit. */
static int onehot_good(const char *name, const U256 *sk, const G1 *pk, uint32_t k, uint32_t choice, const U256 *seed, const U256 *ctx) {
  ElgCipher cts[SIG_K_MAX];
  SigOr slots[SIG_K_MAX];
  SigDleq sum;
  uint64_t m = 0;
  uint32_t j;
  if (!expect(name, sig_onehot_prove(cts, slots, &sum, pk, k, choice, seed, ctx), SIG_OK)
      || !expect(name, sig_onehot_verify(cts, slots, &sum, pk, k, ctx), SIG_OK))
    return 0;
  for (j = 0; j < k; j++)
    if (elg_decrypt(&m, sk, &cts[j], BITS) != ELG_FOUND || m != (uint64_t)(j == choice)) {
      fprintf(stderr, "FAIL sigma %s: slot %u opens to %llu\n", name, j, (unsigned long long)m);
      return 0;
    }
  return 1;
}

/* Two slots that encrypt B0 and B1, each with a good bit proof, and the
   proof of randomness of the sum for m = 1 with the sum of the nonces: the
   sum proof fails unless B0 + B1 = 1. */
static int onehot_bad_sum(const char *name, const G1 *pk, uint32_t b0, uint32_t b1, const U256 *ctx) {
  ElgCipher cts[2];
  SigOr slots[2];
  SigDleq sum;
  ElgCipher total;
  U256 r0;
  U256 r1;
  U256 rs;
  U256 one;
  Fe f0;
  Fe f1;
  Fe fs;
  encrypt_u64(&cts[0], &r0, pk, b0, 10u);
  encrypt_u64(&cts[1], &r1, pk, b1, 11u);
  sig_or_prove(&slots[0], pk, &cts[0], b0, &r0, ctx, 0u);
  sig_or_prove(&slots[1], pk, &cts[1], b1, &r1, ctx, 1u);
  if (!expect(name, sig_or_verify(&slots[0], pk, &cts[0], ctx, 0u), SIG_OK)
      || !expect(name, sig_or_verify(&slots[1], pk, &cts[1], ctx, 1u), SIG_OK))
    return 0;
  fe_from_u256(&f0, &r0, &BN254_FR);
  fe_from_u256(&f1, &r1, &BN254_FR);
  fe_add(&fs, &f0, &f1, &BN254_FR);
  fe_to_u256(&rs, &fs);
  elg_add(&total, &cts[0], &cts[1]);
  u256_from_u64(&one, 1u);
  sig_rand_prove(&sum, pk, &total, &one, &rs, ctx);
  return expect(name, sig_onehot_verify(cts, slots, &sum, pk, 2u, ctx), SIG_BAD_SUM);
}

static void slot_words(const char *prefix, const ElgCipher *ct, const SigOr *s) {
  point(prefix, "c1x", "c1y", &ct->c1);
  point(prefix, "c2x", "c2y", &ct->c2);
  point(prefix, "a0x", "a0y", &s->a0);
  point(prefix, "b0x", "b0y", &s->b0);
  point(prefix, "a1x", "a1y", &s->a1);
  point(prefix, "b1x", "b1y", &s->b1);
  word(prefix, "e0", &s->e0);
  word(prefix, "e1", &s->e1);
  word(prefix, "z0", &s->z0);
  word(prefix, "z1", &s->z1);
}

/* The words of the ballot for choice 1 of k 2 (slot 0 encrypts 0, slot 1
   encrypts 1), its sum and sum proof (m = 1, and z + 1 for the bad row),
   -G, and the forged slot 0 on an encryption of 2. 1 when a proof does not
   verify as the chain rows expect. */
static int words(const G1 *pk, const U256 *seed, const U256 *ctx) {
  ElgCipher cts[2];
  SigOr slots[2];
  SigDleq sum;
  ElgCipher total;
  ElgCipher two;
  SigOr forged;
  U256 r;
  U256 one;
  U256 bad;
  G1 g;
  if (sig_onehot_prove(cts, slots, &sum, pk, 2u, 1u, seed, ctx) != SIG_OK || sig_onehot_verify(cts, slots, &sum, pk, 2u, ctx) != SIG_OK) {
    fputs("FAIL sigma words: the ballot does not verify\n", stderr);
    return 1;
  }
  encrypt_u64(&two, &r, pk, 2u, 4u);
  forge(&forged, pk, &two);
  if (sig_or_verify(&forged, pk, &two, ctx, 0u) != SIG_BAD_SLOT) {
    fputs("FAIL sigma words: the forged slot verifies\n", stderr);
    return 1;
  }
  word("", "ctx", ctx);
  point("", "pkx", "pky", pk);
  elg_generator(&g);
  g1_neg(&g, &g);
  point("", "negGx", "negGy", &g);
  slot_words("s0", &cts[0], &slots[0]);
  slot_words("s1", &cts[1], &slots[1]);
  elg_add(&total, &cts[0], &cts[1]);
  point("sum", "c1x", "c1y", &total.c1);
  point("sum", "c2x", "c2y", &total.c2);
  point("sum", "ax", "ay", &sum.a);
  point("sum", "bx", "by", &sum.b);
  word("sum", "z", &sum.z);
  u256_from_u64(&one, 1u);
  u256_add(&bad, &sum.z, &one);
  word("sum", "zbad", &bad);
  slot_words("f", &two, &forged);
  return 0;
}

int main(int argc, char **argv) {
  ElgCipher cts[SIG_K_MAX];
  SigOr slots[SIG_K_MAX];
  SigDleq sum;
  U256 sk;
  U256 seed;
  U256 ctx;
  U256 ctx2;
  U256 one;
  U256 m;
  U256 m2;
  U256 r;
  U256 r0;
  U256 r1;
  U256 r2;
  G1 pk;
  ElgCipher ct;
  ElgCipher off;
  ElgCipher c0;
  ElgCipher c1;
  ElgCipher c2;
  SigDleq proof;
  SigDleq lie;
  SigOr or0;
  SigOr or1;
  SigOr forged;
  int ok = 1;
  blsag_key(&sk, 5u);
  elg_public(&pk, &sk);
  /* Fixed entropy and context for reproducible vectors only, never a live
     ballot. */
  blsag_key(&seed, 19u);
  blsag_key(&ctx, 23u);
  if (argc == 2 && strcmp(argv[1], "words") == 0)
    return words(&pk, &seed, &ctx);
  u256_from_u64(&one, 1u);
  u256_add(&ctx2, &ctx, &one);
  u256_from_u64(&m, 7u);
  u256_from_u64(&m2, 8u);
  encrypt_u64(&ct, &r, &pk, 7u, 1u);
  sig_rand_prove(&proof, &pk, &ct, &m, &r, &ctx);
  ok &= expect("rand good", sig_rand_verify(&proof, &pk, &ct, &m, &ctx), SIG_OK);
  /* A prover with the nonce of c claims the plaintext 8: only the second
     equation refuses it. */
  sig_rand_prove(&lie, &pk, &ct, &m2, &r, &ctx);
  ok &= expect("rand wrong m", sig_rand_verify(&lie, &pk, &ct, &m2, &ctx), SIG_BAD_SUM)
        & expect("rand wrong m (the proof of 7)", sig_rand_verify(&proof, &pk, &ct, &m2, &ctx), SIG_BAD_SUM);
  ok &= expect("rand wrong ctx", sig_rand_verify(&proof, &pk, &ct, &m, &ctx2), SIG_BAD_SUM);
  off = ct;
  fe_from_u64(&off.c1.x, 1u);
  fe_from_u64(&off.c1.y, 3u);
  ok &= expect("rand off-curve C1", sig_rand_verify(&proof, &pk, &off, &m, &ctx), SIG_REFUSED)
        & expect("rand off-curve C1 (prove)", sig_rand_prove(&lie, &pk, &off, &m, &r, &ctx), SIG_REFUSED);
  encrypt_u64(&c0, &r0, &pk, 0u, 2u);
  encrypt_u64(&c1, &r1, &pk, 1u, 3u);
  sig_or_prove(&or0, &pk, &c0, 0u, &r0, &ctx, 0u);
  ok &= expect("OR bit 0", sig_or_verify(&or0, &pk, &c0, &ctx, 0u), SIG_OK);
  sig_or_prove(&or1, &pk, &c1, 1u, &r1, &ctx, 1u);
  ok &= expect("OR bit 1", sig_or_verify(&or1, &pk, &c1, &ctx, 1u), SIG_OK);
  ok &= expect("OR prove of bit 2", sig_or_prove(&forged, &pk, &c1, 2u, &r1, &ctx, 0u), SIG_REFUSED);
  encrypt_u64(&c2, &r2, &pk, 2u, 4u);
  forge(&forged, &pk, &c2);
  ok &= expect("OR forged split (e0 + e1 != e) on an encryption of 2", sig_or_verify(&forged, &pk, &c2, &ctx, 0u), SIG_BAD_SLOT);
  ok &= expect("OR swapped slot index j", sig_or_verify(&or0, &pk, &c0, &ctx, 1u), SIG_BAD_SLOT);
  ok &= onehot_good("one-hot k 2 choice 0", &sk, &pk, 2u, 0u, &seed, &ctx);
  ok &= onehot_good("one-hot k 3 choice 2", &sk, &pk, 3u, 2u, &seed, &ctx);
  ok &= onehot_bad_sum("two-hot", &pk, 1u, 1u, &ctx);
  ok &= onehot_bad_sum("all-zero ballot", &pk, 0u, 0u, &ctx);
  ok &= expect("one-hot k 0", sig_onehot_prove(cts, slots, &sum, &pk, 0u, 0u, &seed, &ctx), SIG_REFUSED)
        & expect("one-hot k 9", sig_onehot_prove(cts, slots, &sum, &pk, SIG_K_MAX + 1u, 0u, &seed, &ctx), SIG_REFUSED)
        & expect("one-hot choice 2 of k 2", sig_onehot_prove(cts, slots, &sum, &pk, 2u, 2u, &seed, &ctx), SIG_REFUSED)
        & expect("one-hot k 2 (for the verify rows)", sig_onehot_prove(cts, slots, &sum, &pk, 2u, 1u, &seed, &ctx), SIG_OK)
        & expect("one-hot verify k 0", sig_onehot_verify(cts, slots, &sum, &pk, 0u, &ctx), SIG_REFUSED)
        & expect("one-hot verify k 9", sig_onehot_verify(cts, slots, &sum, &pk, SIG_K_MAX + 1u, &ctx), SIG_REFUSED);
  if (!ok)
    return 1;
  puts("sigma regressions: passed");
  return 0;
}
