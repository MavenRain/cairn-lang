/* Threshold opening of exponential ElGamal over secp256k1 in the host
   (docs/PLAN.md section 9, slice S6 b1, Q-S6-3 (a) and Q-S6-4 (a)). Not
   guest visible: the guest keeps the public part of a quorum (`Quorum`) and
   the law `open q c = decrypt c` (docs/SPEC.md rule O7); this file is a
   host model of the scheme under it.

   A trusted dealer splits the key sk by Shamir mod GROUP_N (t of n, x_i = i).
   The holder of share i gives the partial decryption D_i = s_i C1 with a
   Chaum-Pedersen proof (A, B, z) that log_G PK_i = log_C1 D_i. Any t parts
   with good proofs give sk C1 = sum lambda_i D_i (Lagrange in the exponent
   at 0), thus m G = C2 - sk C1 and m by elg_log. The dealer is for test
   vectors only: front/dkg.c deals the keys (docs/VALIDATION.md,
   Assumptions).

   The challenge e is the hash chain of the guest `hash a b` (SHA-256 of
   the 64 big endian bytes of a and b) from DLEQ_TAG over the ten point
   words, as a raw word: the view dleqOk of examples/threshold.lang computes
   the same chain on the chain (ecMul, ecAdd, hash and u256Eq only). */
#ifndef LANG_FRONT_THRESHOLD_H
#define LANG_FRONT_THRESHOLD_H

#include <stdint.h>

#include "front/group.h"
#include "front/elgamal.h"
#include "front/u256.h"

/* The most shares. */
#define THR_N_MAX 8u

/* The domain tags: the ASCII bytes of "shamir" (the dealer coefficients),
   "dleq" (the challenge chain) and "dleqw" (the proof nonces) as words. */
extern const U256 THR_TAG;
extern const U256 DLEQ_TAG;
extern const U256 DLEQ_NONCE_TAG;

/* Share x of the dealer: s = f(x) and the verification key PK_x = s G. */
typedef struct {
  uint32_t x;
  U256 s;
  GroupPoint key;
} ThrShare;

/* The partial decryption of share x for one ciphertext and its proof. */
typedef struct {
  uint32_t x;
  GroupPoint d; /* s C1 */
  GroupPoint a; /* w G */
  GroupPoint b; /* w C1 */
  U256 z; /* w + e s mod GROUP_N */
} ThrPart;

typedef enum {
  THR_FOUND,
  THR_NOT_FOUND, /* m is 2^bits or more */
  THR_REFUSED,   /* bits, t or n is out of range, or a point is off the curve */
  THR_TOO_FEW,   /* fewer than t parts */
  THR_BAD_INDEX, /* a part index is 0, above n or repeated */
  THR_BAD_PROOF, /* a DLEQ proof fails */
  THR_NO_MEMORY  /* no memory for the baby step table */
} ThrResult;

/* SHA-256 of the 64 big endian bytes of A and B: the guest `hash a b`.
   OUT can be A or B. */
void thr_hash(U256 *out, const U256 *a, const U256 *b);
/* The challenge e: the chain of thr_hash from DLEQ_TAG over KEY.x, KEY.y,
   C1.x, C1.y, D.x, D.y, A.x, A.y, B.x, B.y (10 hashes), not reduced. */
void thr_challenge(U256 *e, const GroupPoint *key, const GroupPoint *c1, const GroupPoint *d, const GroupPoint *a, const GroupPoint *b);
/* The private coefficient a_k = hash(hash(THR_TAG, SEED), k) mod GROUP_N.
   SEED is fresh secret 256-bit entropy, independent of SK, for each deal.
   Keep it private from shareholders, including after the deal. */
void thr_nonce(U256 *out, const U256 *seed, uint64_t k);
/* The N shares of SK for a threshold T (SHARES[i] has x = i + 1) and the
   public key sk G, using the private SEED described above. Public or reused
   coefficient entropy lets a single shareholder recover SK. Fixed seeds
   are for test vectors only. 1, or 0 unless
   1 <= T <= N <= THR_N_MAX and SK < GROUP_N. */
int thr_deal(ThrShare *shares, GroupPoint *pk, const U256 *sk, const U256 *seed, uint32_t t, uint32_t n);
/* The part of SHARE for CT, with the nonce w = hash chain of
   DLEQ_NONCE_TAG, s, C1.x, C1.y mod GROUP_N: one w per share and
   ciphertext. CT must be on the curve. */
void thr_partial(ThrPart *out, const ThrShare *share, const ElgCipher *ct);
/* 1 when z G = A + e KEY and z C1 = B + e D, with e = thr_challenge. Each
   point must be on the curve (thr_open tests them). */
int thr_verify(const ThrPart *part, const GroupPoint *key, const ElgCipher *ct);
/* lambda_i at 0 for the set of the K part indices: the product over j != I
   of x_j / (x_j - x_i) mod GROUP_N. The indices must be distinct and
   nonzero. */
void thr_lagrange(U256 *out, const ThrPart *parts, uint32_t k, uint32_t i);
/* The m below 2^BITS of CT from K parts, with KEYS[x - 1] = PK_x for the N
   shares and the threshold T. In order: THR_REFUSED, THR_TOO_FEW,
   THR_BAD_INDEX (before the Lagrange sum: fe_inv(0) = 0, so a repeated x
   gives a wrong sum without an error), THR_BAD_PROOF, then the result of
   elg_log on C2 - sum lambda_i D_i. A K above T uses all K parts. *M gets m
   on THR_FOUND. */
ThrResult thr_open(uint64_t *m, const ThrPart *parts, uint32_t k, const GroupPoint *keys, uint32_t t, uint32_t n, const ElgCipher *ct, uint32_t bits);
/* The close of a ballot (Q-S6-2 (a)): the K parts for CT in arrival order.
   A part counts when its x is 1 to N and no earlier counted part has the
   same x, its points are on the curve and thr_verify holds against
   KEYS[x - 1]. A part that does not count does not stop the close. In
   order: THR_REFUSED when T or N is out of range or a key or CT is off the
   curve; THR_FOUND when the T-th part counts, with *AT = its index in PARTS
   (from 0); THR_TOO_FEW when fewer than T parts count (the ballot stays
   open). The parts that count, up to *AT, are a quorum for thr_open. */
ThrResult thr_close(uint32_t *at, const ThrPart *parts, uint32_t k, const GroupPoint *keys, uint32_t t, uint32_t n, const ElgCipher *ct);

#endif
