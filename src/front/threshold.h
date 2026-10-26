/* Threshold opening of exponential ElGamal over BN254 G1 in the host
   (docs/PLAN.md section 9, slice S6 b1, Q-S6-3 (a) and Q-S6-4 (a)). Not
   guest visible: the guest keeps the public part of a quorum (`Quorum`) and
   the law `open q c = decrypt c` (docs/SPEC.md rule O7); this file is a
   host model of the scheme under it.

   A trusted dealer splits the key sk by Shamir over Fr (t of n, x_i = i).
   The holder of share i gives the partial decryption D_i = s_i C1 with a
   Chaum-Pedersen proof (A, B, z) that log_G PK_i = log_C1 D_i. Any t parts
   with good proofs give sk C1 = sum lambda_i D_i (Lagrange in the exponent
   at 0), thus m G = C2 - sk C1 and m by elg_log. The dealer and the
   distributed key generation that would replace it are VALIDATION
   assumptions.

   The challenge e is the hash chain of the guest `hash a b` (keccak256 of
   the 64 big endian bytes of a and b) from DLEQ_TAG over the ten point
   words, as a raw word: the view dleqOk of examples/threshold.lang computes
   the same chain on the chain (ecMul, ecAdd, hash and u256Eq only). */
#ifndef LANG_FRONT_THRESHOLD_H
#define LANG_FRONT_THRESHOLD_H

#include <stdint.h>

#include "front/bn254.h"
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
  G1 key;
} ThrShare;

/* The partial decryption of share x for one ciphertext and its proof. */
typedef struct {
  uint32_t x;
  G1 d; /* s C1 */
  G1 a; /* w G */
  G1 b; /* w C1 */
  U256 z; /* w + e s mod r */
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

/* keccak256 of the 64 big endian bytes of A and B: the guest `hash a b`.
   OUT can be A or B. */
void thr_hash(U256 *out, const U256 *a, const U256 *b);
/* The challenge e: the chain of thr_hash from DLEQ_TAG over KEY.x, KEY.y,
   C1.x, C1.y, D.x, D.y, A.x, A.y, B.x, B.y (10 hashes), not reduced. */
void thr_challenge(U256 *e, const G1 *key, const G1 *c1, const G1 *d, const G1 *a, const G1 *b);
/* The private coefficient a_k = hash(hash(THR_TAG, SEED), k) mod r.
   SEED is fresh secret 256-bit entropy, independent of SK, for each deal.
   Keep it private from shareholders, including after the deal. */
void thr_nonce(U256 *out, const U256 *seed, uint64_t k);
/* The N shares of SK for a threshold T (SHARES[i] has x = i + 1) and the
   public key sk G, using the private SEED described above. Public or reused
   coefficient entropy lets a single shareholder recover SK. Fixed seeds
   are for test vectors only. 1, or 0 unless
   1 <= T <= N <= THR_N_MAX and SK < r. */
int thr_deal(ThrShare *shares, G1 *pk, const U256 *sk, const U256 *seed, uint32_t t, uint32_t n);
/* The part of SHARE for CT, with the nonce w = hash chain of
   DLEQ_NONCE_TAG, s, C1.x, C1.y mod r: one w per share and ciphertext. CT
   must be on the curve. */
void thr_partial(ThrPart *out, const ThrShare *share, const ElgCipher *ct);
/* 1 when z G = A + e KEY and z C1 = B + e D, with e = thr_challenge. Each
   point must be on the curve (thr_open tests them). */
int thr_verify(const ThrPart *part, const G1 *key, const ElgCipher *ct);
/* lambda_i at 0 for the set of the K part indices: the product over j != I
   of x_j / (x_j - x_i) over Fr. The indices must be distinct and nonzero. */
void thr_lagrange(U256 *out, const ThrPart *parts, uint32_t k, uint32_t i);
/* The m below 2^BITS of CT from K parts, with KEYS[x - 1] = PK_x for the N
   shares and the threshold T. In order: THR_REFUSED, THR_TOO_FEW,
   THR_BAD_INDEX (before the Lagrange sum: fe_inv(0) = 0, so a repeated x
   gives a wrong sum without an error), THR_BAD_PROOF, then the result of
   elg_log on C2 - sum lambda_i D_i. A K above T uses all K parts. *M gets m
   on THR_FOUND. */
ThrResult thr_open(uint64_t *m, const ThrPart *parts, uint32_t k, const G1 *keys, uint32_t t, uint32_t n, const ElgCipher *ct, uint32_t bits);

#endif
