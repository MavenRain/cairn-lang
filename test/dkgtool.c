/* Host DKG regressions (slice S6 b5, Q-S6-4 (a)): one code, two runs, for
   keyOpen (3 of 4 to 2 of 3: member 3 leaves) and keyAppend (2 of 3 to 3 of
   4: tAppend 2 -> 3 of G4, member 4 joins). Each run: the DKG key is the
   sum of the dealt secrets, a DKG quorum opens a ciphertext through
   thr_open, the reshare keeps the key and a reshared quorum opens the old
   ciphertext. Refused: a bad share, a bad proof, a dealing without the
   Lagrange weight, a quorum below the old threshold, a reshare against
   another key (the sum) and a dealer outside the quorum. The seeds are
   fixed for reproducible vectors only. Initial DKG admission also checks
   the complete agreed dealer roster and threshold. */
#include <stdio.h>

#include "front/blsag.h"
#include "front/dkg.h"

#define BITS 16u
#define PLAIN 1234u

static int fail(const char *run, const char *what) {
  fprintf(stderr, "FAIL dkg %s: %s\n", run, what);
  return 0;
}

/* The ciphertext of M under PK with the nonce r_I. */
static void encrypt_u64(ElgCipher *out, const GroupPoint *pk, uint64_t m, uint64_t i) {
  U256 w;
  U256 r;
  u256_from_u64(&w, m);
  elg_nonce(&r, i);
  elg_encrypt(out, pk, &w, &r);
}

/* COL[i] = DEALT[i][J] for the K dealings: the shares of member J + 1. */
static void column(U256 *col, U256 (*dealt)[THR_N_MAX], uint32_t k, uint32_t j) {
  uint32_t i;
  for (i = 0; i < k; i++)
    col[i] = dealt[i][j];
}

/* 1 when the T shares from SHARES[FROM] open CT to PLAIN through thr_open,
   with KEYS for the N shares. */
static int opens(const ThrShare *shares, uint32_t from, uint32_t t, const GroupPoint *keys, uint32_t n, const ElgCipher *ct) {
  ThrPart parts[THR_N_MAX];
  uint64_t m = 0;
  uint32_t i;
  for (i = 0; i < t; i++)
    thr_partial(&parts[i], &shares[from + i], ct);
  return thr_open(&m, parts, t, keys, t, n, ct, BITS) == THR_FOUND && m == PLAIN;
}

/* The DKG of N members at the threshold T, member i with the seed
   blsag_key(BASE + i) and a_0 = thr_nonce(seed, 0): SHARES (x = 1 to N),
   the public key PK and SK = the sum of the a_0 (known here only to test
   PK). 1, or 0 when a dealing or a combine refuses or two members get
   different keys. */
static int dkg_run(ThrShare *shares, GroupPoint *pk, U256 *sk, uint32_t t, uint32_t n, uint64_t base) {
  DkgDeal deals[THR_N_MAX];
  U256 dealt[THR_N_MAX][THR_N_MAX];
  U256 col[THR_N_MAX];
  U256 seed;
  U256 a0;
  GroupPoint got;
  Fe acc;
  Fe fa;
  Fe tmp;
  uint32_t i;
  uint32_t j;
  fe_from_u64(&acc, 0u);
  for (i = 0; i < n; i++) {
    blsag_key(&seed, base + i);
    thr_nonce(&a0, &seed, 0u);
    if (!dkg_deal(&deals[i], dealt[i], &a0, &seed, i + 1u, t, n))
      return 0;
    fe_from_u256(&fa, &a0, &GROUP_N);
    fe_add(&tmp, &acc, &fa, &GROUP_N);
    acc = tmp;
  }
  fe_to_u256(sk, &acc);
  for (j = 0; j < n; j++) {
    column(col, dealt, n, j);
    if (!dkg_combine(&shares[j], &got, deals, col, n, t, n, j + 1u))
      return 0;
    if (j == 0u)
      *pk = got;
    if (!group_eq(pk, &got))
      return 0;
  }
  return 1;
}

/* One run: a DKG of N0 members at the threshold T0, then a reshare by the
   Q old members of QUORUM to N1 members at the threshold T1. */
static int key_run(const char *name, uint32_t t0, uint32_t n0, const uint32_t *quorum, uint32_t q, uint32_t t1, uint32_t n1, uint64_t base) {
  ThrShare old[THR_N_MAX];
  ThrShare fresh[THR_N_MAX];
  ThrShare out;
  ThrShare stranger;
  GroupPoint pk;
  GroupPoint want;
  GroupPoint old_keys[THR_N_MAX];
  GroupPoint new_keys[THR_N_MAX];
  DkgDeal deals[THR_N_MAX];
  DkgDeal alt[THR_N_MAX];
  U256 dealt[THR_N_MAX][THR_N_MAX];
  U256 col[THR_N_MAX];
  U256 own[THR_N_MAX];
  U256 sk;
  U256 seed;
  U256 one;
  ElgCipher ct;
  uint32_t i;
  uint32_t j;
  int ok = 1;
  if (!dkg_run(old, &pk, &sk, t0, n0, base))
    return fail(name, "the DKG refused");
  elg_public(&want, &sk);
  if (!group_eq(&pk, &want))
    ok = fail(name, "the key is not the sum of the dealt secrets");
  for (i = 0; i < n0; i++)
    old_keys[i] = old[i].key;
  encrypt_u64(&ct, &pk, PLAIN, base);
  if (!opens(old, n0 - t0, t0, old_keys, n0, &ct))
    ok = fail(name, "a DKG quorum does not open");
  /* The reshare: member i of QUORUM deals lambda_i s_i at the threshold T1. */
  for (i = 0; i < q; i++) {
    blsag_key(&seed, base + 100u + i);
    if (!dkg_reshare(&deals[i], dealt[i], &old[quorum[i] - 1u], quorum, q, &seed, t1, n1))
      return fail(name, "a reshare dealing refused");
  }
  for (j = 0; j < n1; j++) {
    column(col, dealt, q, j);
    if (!dkg_reshare_combine(&fresh[j], deals, col, q, old_keys, t0, n0, &pk, j + 1u))
      return fail(name, "the reshare aborted");
    new_keys[j] = fresh[j].key;
  }
  if (!opens(fresh, n1 - t1, t1, new_keys, n1, &ct))
    ok = fail(name, "a reshared quorum does not open the old ciphertext");
  /* The refusals, for new member 1. */
  u256_from_u64(&one, 1u);
  column(col, dealt, q, 0u);
  u256_add(&col[0], &col[0], &one);
  if (dkg_verify(&deals[0], &col[0], 1u) || dkg_reshare_combine(&out, deals, col, q, old_keys, t0, n0, &pk, 1u))
    ok = fail(name, "a bad share is not refused");
  column(col, dealt, q, 0u);
  for (i = 0; i < q; i++)
    alt[i] = deals[i];
  u256_add(&alt[0].mu, &alt[0].mu, &one);
  if (dkg_verify(&alt[0], &col[0], 1u) || dkg_reshare_combine(&out, alt, col, q, old_keys, t0, n0, &pk, 1u))
    ok = fail(name, "a bad proof is not refused");
  blsag_key(&seed, base + 200u);
  if (!dkg_deal(&alt[0], own, &old[quorum[0] - 1u].s, &seed, quorum[0], t1, n1))
    ok = fail(name, "a dealing refused");
  col[0] = own[0];
  if (dkg_reshare_combine(&out, alt, col, q, old_keys, t0, n0, &pk, 1u))
    ok = fail(name, "a dealing without the Lagrange weight is not refused");
  column(col, dealt, q, 0u);
  if (dkg_reshare_combine(&out, deals, col, t0 - 1u, old_keys, t0, n0, &pk, 1u))
    ok = fail(name, "a quorum below the old threshold is not refused");
  if (dkg_reshare_combine(&out, deals, col, q, old_keys, t0, n0, &old_keys[0], 1u))
    ok = fail(name, "a reshare against another key does not abort");
  stranger = old[0];
  stranger.x = n0 + 1u;
  if (dkg_reshare(&alt[0], own, &stranger, quorum, q, &seed, t1, n1))
    ok = fail(name, "a dealer outside the quorum is not refused");
  return ok;
}

static int initial_admission(void) {
  DkgDeal deals[THR_N_MAX];
  DkgDeal alt[THR_N_MAX];
  ThrShare out;
  GroupPoint pk;
  U256 dealt[THR_N_MAX][THR_N_MAX];
  U256 col[THR_N_MAX];
  U256 sk;
  U256 seed;
  uint32_t i;
  /* Shares for member 4 make the outsider cases otherwise valid. */
  for (i = 0; i < 4u; i++) {
    u256_from_u64(&sk, 7u + i);
    u256_from_u64(&seed, 9u + i);
    if (!dkg_deal(&deals[i], dealt[i], &sk, &seed, i + 1u, 2u, 4u))
      return fail("initial admission", "the dealing refused");
    alt[i] = deals[i];
  }
  column(col, dealt, 3u, 0u);
  if (dkg_combine(&out, &pk, deals, col, 1u, 2u, 3u, 1u))
    return fail("initial admission", "one dealer completed a 2-of-3 DKG");
  if (dkg_combine(&out, &pk, deals, col, 2u, 2u, 3u, 1u))
    return fail("initial admission", "a missing member is not refused");
  if (dkg_combine(&out, &pk, deals, col, 3u, 3u, 3u, 1u))
    return fail("initial admission", "an unexpected threshold is not refused");
  if (!dkg_combine(&out, &pk, deals, col, 3u, 2u, 3u, 1u))
    return fail("initial admission", "the complete roster refused");
  alt[1] = deals[0];
  col[1] = col[0];
  if (dkg_combine(&out, &pk, alt, col, 3u, 2u, 3u, 1u))
    return fail("initial admission", "a repeated dealer is not refused");
  alt[1] = deals[1];
  alt[2] = deals[3];
  column(col, dealt, 3u, 0u);
  col[2] = dealt[3][0];
  if (dkg_combine(&out, &pk, alt, col, 3u, 2u, 3u, 1u))
    return fail("initial admission", "a foreign dealer is not refused");
  column(col, dealt, 3u, 3u);
  if (dkg_combine(&out, &pk, deals, col, 3u, 2u, 3u, 4u))
    return fail("initial admission", "a foreign recipient is not refused");
  for (i = 0; i < 3u; i++) {
    alt[i] = deals[2u - i];
    col[i] = dealt[2u - i][0];
  }
  if (!dkg_combine(&out, &pk, alt, col, 3u, 2u, 3u, 1u))
    return fail("initial admission", "the reordered roster refused");
  return 1;
}

int main(void) {
  static const uint32_t open_q[3] = {1u, 2u, 4u};
  static const uint32_t append_q[2] = {1u, 3u};
  int ok = 1;
  ok &= initial_admission();
  ok &= key_run("keyOpen", 3u, 4u, open_q, 3u, 2u, 3u, 10u);
  ok &= key_run("keyAppend", 2u, 3u, append_q, 2u, 3u, 4u, 20u);
  if (!ok)
    return 1;
  puts("dkg regressions: passed");
  return 0;
}
