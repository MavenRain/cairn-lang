/* Host threshold regressions (slice S6 b1): every quorum opens the plaintext
   (the host lawQuorum), the opening agrees with elg_decrypt (the host
   lawOpen), a quorum opens a sum (the host lawTally), and the refusals of
   front/threshold.c. With the argument `words` it prints the chain constants
   of examples/threshold.lang (slice S6 b3), one `name 0x<word>` per line. */
#include <stdio.h>
#include <string.h>

#include "front/blsag.h"
#include "front/elgamal.h"
#include "front/threshold.h"

#define T 2u
#define N 3u
#define BITS 16u

/* The ciphertext of M under PK with the nonce r_I. */
static void encrypt_u64(ElgCipher *out, const GroupPoint *pk, uint64_t m, uint64_t i) {
  U256 w;
  U256 r;
  u256_from_u64(&w, m);
  elg_nonce(&r, i);
  elg_encrypt(out, pk, &w, &r);
}

/* The K parts of the shares with the indices IDX for CT. */
static void quorum(ThrPart *out, const ThrShare *shares, const uint32_t *idx, uint32_t k, const ElgCipher *ct) {
  uint32_t i;
  for (i = 0; i < k; i++)
    thr_partial(&out[i], &shares[idx[i] - 1u], ct);
}

/* 1 when the K PARTS open CT to EXPECT, with the plaintext WANT on
   THR_FOUND. */
static int row(const char *name, const ThrPart *parts, uint32_t k, const GroupPoint *keys, const ElgCipher *ct, ThrResult expect, uint64_t want) {
  uint64_t m = 0;
  ThrResult got = thr_open(&m, parts, k, keys, T, N, ct, BITS);
  if (got == expect && (expect != THR_FOUND || m == want))
    return 1;
  fprintf(stderr, "FAIL threshold %s: result %d, m %llu\n", name, (int)got, (unsigned long long)m);
  return 0;
}

static void word(const char *name, const U256 *w) {
  printf("%s 0x%016llx%016llx%016llx%016llx\n", name, (unsigned long long)w->limb[3], (unsigned long long)w->limb[2],
         (unsigned long long)w->limb[1], (unsigned long long)w->limb[0]);
}

static void point(const char *x, const char *y, const GroupPoint *p) {
  U256 a;
  U256 b;
  group_to_words(&a, &b, p);
  word(x, &a);
  word(y, &b);
}

/* A holder of share 1 must not recover sk by subtracting a public
   coefficient from s_1. This recovered the entire key before the dealer
   coefficients were derived from private entropy. */
static int private_dealer(const ThrShare *share, const GroupPoint *pk) {
  U256 public_coef;
  U256 recovered;
  Fe s;
  Fe a;
  Fe guess;
  GroupPoint guessed_pk;
  u256_from_u64(&public_coef, 1u);
  thr_hash(&public_coef, &THR_TAG, &public_coef);
  blsag_reduce(&public_coef, &public_coef);
  fe_from_u256(&s, &share->s, &GROUP_N);
  fe_from_u256(&a, &public_coef, &GROUP_N);
  fe_sub(&guess, &s, &a, &GROUP_N);
  fe_to_u256(&recovered, &guess);
  elg_public(&guessed_pk, &recovered);
  if (!group_eq(&guessed_pk, pk))
    return 1;
  fputs("FAIL threshold one share recovers master key\n", stderr);
  return 0;
}

/* Fresh entropy must change the shares while preserving the master key. */
static int fresh_dealer(const ThrShare *shares, const GroupPoint *pk, const U256 *sk, const U256 *seed) {
  ThrShare fresh[N];
  GroupPoint fresh_pk;
  U256 other;
  thr_hash(&other, &THR_TAG, seed);
  if (thr_deal(fresh, &fresh_pk, sk, &other, T, N) &&
      group_eq(&fresh_pk, pk) && u256_cmp(&fresh[0].s, &shares[0].s) != 0)
    return 1;
  fputs("FAIL threshold fresh dealer entropy\n", stderr);
  return 0;
}

/* The words of the quorum {1, 2} for CT (the plaintext 1234). */
static int words(const ThrShare *shares, const ElgCipher *ct) {
  static const uint32_t s12[2] = {1u, 2u};
  ThrPart parts[2];
  U256 e;
  U256 l1;
  U256 l2;
  U256 m;
  U256 one;
  U256 bad;
  GroupPoint g;
  GroupPoint p;
  GroupPoint q1;
  GroupPoint q2;
  quorum(parts, shares, s12, 2u, ct);
  thr_challenge(&e, &shares[0].key, &ct->c1, &parts[0].d, &parts[0].a, &parts[0].b);
  thr_lagrange(&l1, parts, 2u, 0u);
  thr_lagrange(&l2, parts, 2u, 1u);
  u256_from_u64(&m, 1234u);
  u256_from_u64(&one, 1u);
  u256_add(&bad, &parts[0].z, &one);
  elg_generator(&g);
  point("pk1x", "pk1y", &shares[0].key);
  point("c1x", "c1y", &ct->c1);
  point("d1x", "d1y", &parts[0].d);
  point("a1x", "a1y", &parts[0].a);
  point("b1x", "b1y", &parts[0].b);
  word("z1", &parts[0].z);
  word("e1", &e);
  word("z1bad", &bad);
  point("d2x", "d2y", &parts[1].d);
  word("l1", &l1);
  word("l2", &l2);
  group_mul(&q1, &parts[0].d, &l1);
  group_mul(&q2, &parts[1].d, &l2);
  point("q1x", "q1y", &q1);
  point("q2x", "q2y", &q2);
  group_add(&p, &q1, &q2);
  point("kx", "ky", &p);
  point("c2x", "c2y", &ct->c2);
  word("m", &m);
  group_mul(&p, &g, &m);
  point("mgx", "mgy", &p);
  group_mul(&p, &g, &parts[0].z);
  point("zgx", "zgy", &p);
  group_mul(&p, &shares[0].key, &e);
  point("epkx", "epky", &p);
  group_add(&p, &parts[0].a, &p);
  point("r1x", "r1y", &p);
  group_mul(&p, &ct->c1, &parts[0].z);
  point("zcx", "zcy", &p);
  group_mul(&p, &parts[0].d, &e);
  point("edx", "edy", &p);
  group_add(&p, &parts[0].b, &p);
  point("r2x", "r2y", &p);
  return 0;
}

int main(int argc, char **argv) {
  static const uint32_t s12[2] = {1u, 2u};
  static const uint32_t s13[2] = {1u, 3u};
  static const uint32_t s23[2] = {2u, 3u};
  static const uint32_t s123[3] = {1u, 2u, 3u};
  U256 sk;
  U256 seed;
  U256 one;
  GroupPoint pk;
  GroupPoint g;
  GroupPoint keys[N];
  ThrShare shares[N];
  ThrPart parts[N];
  ThrPart bad[2];
  ElgCipher ct;
  ElgCipher a;
  ElgCipher b;
  ElgCipher c;
  ElgCipher sum;
  uint64_t m = 0;
  uint64_t want = 0;
  ThrResult got;
  ElgResult elg;
  uint32_t i;
  int ok = 1;
  blsag_key(&sk, 5u);
  /* Fixed entropy for reproducible vectors only, never a live deal. */
  blsag_key(&seed, 17u);
  if (!thr_deal(shares, &pk, &sk, &seed, T, N)) {
    fputs("FAIL threshold deal\n", stderr);
    return 1;
  }
  for (i = 0; i < N; i++)
    keys[i] = shares[i].key;
  encrypt_u64(&ct, &pk, 1234u, 1u);
  if (argc == 2 && strcmp(argv[1], "words") == 0)
    return words(shares, &ct);
  ok &= private_dealer(&shares[0], &pk);
  ok &= fresh_dealer(shares, &pk, &sk, &seed);
  quorum(parts, shares, s12, 2u, &ct);
  ok &= row("open {1, 2}", parts, 2u, keys, &ct, THR_FOUND, 1234u);
  quorum(parts, shares, s13, 2u, &ct);
  ok &= row("open {1, 3}", parts, 2u, keys, &ct, THR_FOUND, 1234u);
  quorum(parts, shares, s23, 2u, &ct);
  ok &= row("open {2, 3}", parts, 2u, keys, &ct, THR_FOUND, 1234u);
  quorum(parts, shares, s123, 3u, &ct);
  ok &= row("open {1, 2, 3}", parts, 3u, keys, &ct, THR_FOUND, 1234u);
  quorum(parts, shares, s12, 2u, &ct);
  got = thr_open(&m, parts, 2u, keys, T, N, &ct, BITS);
  elg = elg_decrypt(&want, &sk, &ct, BITS);
  if (got != THR_FOUND || elg != ELG_FOUND || m != want) {
    fprintf(stderr, "FAIL threshold open = decrypt: results %d %d, m %llu %llu\n", (int)got, (int)elg,
            (unsigned long long)m, (unsigned long long)want);
    ok = 0;
  }
  encrypt_u64(&a, &pk, 100u, 2u);
  encrypt_u64(&b, &pk, 200u, 3u);
  encrypt_u64(&c, &pk, 300u, 4u);
  elg_add(&sum, &a, &b);
  elg_add(&sum, &sum, &c);
  quorum(bad, shares, s23, 2u, &sum);
  ok &= row("tally 100 + 200 + 300 by {2, 3}", bad, 2u, keys, &sum, THR_FOUND, 600u);
  elg_generator(&g);
  bad[0] = parts[0];
  bad[1] = parts[1];
  group_add(&bad[1].d, &bad[1].d, &g);
  ok &= row("D_2 + G with the proof of D_2", bad, 2u, keys, &ct, THR_BAD_PROOF, 0u);
  bad[1] = parts[1];
  u256_from_u64(&one, 1u);
  u256_add(&bad[0].z, &bad[0].z, &one);
  ok &= row("z_1 + 1", bad, 2u, keys, &ct, THR_BAD_PROOF, 0u);
  ok &= row("one part for t = 2", parts, 1u, keys, &ct, THR_TOO_FEW, 0u);
  bad[0] = parts[0];
  bad[1] = parts[0];
  ok &= row("{1, 1}", bad, 2u, keys, &ct, THR_BAD_INDEX, 0u);
  bad[1] = parts[1];
  bad[0].x = 0u;
  ok &= row("{0, 2}", bad, 2u, keys, &ct, THR_BAD_INDEX, 0u);
  bad[0] = parts[1];
  bad[1] = parts[0];
  bad[1].x = 4u;
  ok &= row("{2, 4}", bad, 2u, keys, &ct, THR_BAD_INDEX, 0u);
  bad[0] = parts[0];
  bad[1] = parts[1];
  fe_from_u64(&bad[0].d.x, 1u);
  fe_from_u64(&bad[0].d.y, 3u);
  ok &= row("D_1 off the curve", bad, 2u, keys, &ct, THR_REFUSED, 0u);
  if (!ok)
    return 1;
  puts("threshold regressions: passed");
  return 0;
}
