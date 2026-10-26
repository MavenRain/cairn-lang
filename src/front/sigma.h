/* Sigma verifiers over exponential ElGamal on BN254 G1 in the host
   (docs/PLAN.md section 9, slice S7 b2, Q-S7-4 .. Q-S7-7 (a)). Not guest
   visible: the guest keeps the truncation ZKProof and the law `verified`
   (docs/SPEC.md rule O7); this file is a host model of the proofs under it,
   and examples/onehot.lang checks the same equations on the chain.

   Three proofs, each a Fiat-Shamir Chaum-Pedersen DLEQ:
   - randomness: the prover knows the nonce r of c = (C1, C2) and c
     encrypts the public m: log_G C1 = log_PK V with V = C2 - m G;
   - bit (CDS OR): c encrypts 0 (log_G C1 = log_PK C2) or 1 (log_G C1 =
     log_PK (C2 - G)), with one branch simulated. The split is e0 + e1 = e
     modulo 2^256 (the guest u256Add has no mod); each z uses e_b mod r;
   - one-hot: a ballot of k bit proofs and the proof of randomness of the
     sum for m = 1.

   Each challenge e is the raw chain of thr_hash (the guest `hash a b`) from
   a tag over ctx and the statement, not reduced: ecMul by a raw word is
   ecMul by the word mod r, as G1 has prime order r. ctx binds a proof to its
   context (S9: the bLSAG key image; else a copied ballot verifies). The
   nonces come from the secret r and the statement, never from public data:
   a public nonce gives r, thus the vote (the S6 review lesson). */
#ifndef LANG_FRONT_SIGMA_H
#define LANG_FRONT_SIGMA_H

#include <stdint.h>

#include "front/bn254.h"
#include "front/elgamal.h"
#include "front/u256.h"

/* The most slots of a one-hot ballot. */
#define SIG_K_MAX 8u

/* The domain tags: the ASCII bytes of "rand" and "onehot" (the challenge
   chains), "randw" and "onehotw" (the proof nonces) and "ballot" (the slot
   nonces from the seed) as words. */
extern const U256 RAND_TAG;
extern const U256 ONEHOT_TAG;
extern const U256 RAND_NONCE_TAG;
extern const U256 ONEHOT_NONCE_TAG;
extern const U256 BALLOT_TAG;

/* A proof of randomness: A = w G, B = w PK, z = w + (e mod r) r. */
typedef struct {
  G1 a;
  G1 b;
  U256 z;
} SigDleq;

/* A bit proof: branch 0 (A0, B0, e0, z0) for c encrypts 0, branch 1 (A1,
   B1, e1, z1) for c encrypts 1. */
typedef struct {
  G1 a0;
  G1 b0;
  G1 a1;
  G1 b1;
  U256 e0;
  U256 e1;
  U256 z0;
  U256 z1;
} SigOr;

typedef enum {
  SIG_OK,
  SIG_REFUSED,  /* k or the choice is out of range, a bit is above 1, or a point is off the curve */
  SIG_BAD_SLOT, /* a bit proof fails: the split or an equation */
  SIG_BAD_SUM   /* a proof of randomness fails (the sum proof of a ballot) */
} SigResult;

/* C2 - m G: the point that is r PK when CT encrypts M with the nonce r.
   CT must be on the curve. */
void sig_plain(G1 *out, const ElgCipher *ct, const U256 *m);
/* The challenge of a proof of randomness: the chain of thr_hash from
   RAND_TAG over CTX, PK.x, PK.y, C1.x, C1.y, C2.x, C2.y, M, A.x, A.y, B.x,
   B.y (12 hashes), not reduced. */
void sig_rand_challenge(U256 *e, const G1 *pk, const ElgCipher *ct, const U256 *m, const G1 *a, const G1 *b, const U256 *ctx);
/* The challenge of a bit proof: the chain of thr_hash from ONEHOT_TAG over
   CTX, J, PK, C1, C2, A0, B0, A1, B1 (16 hashes), not reduced. */
void sig_or_challenge(U256 *e, const G1 *pk, const ElgCipher *ct, const SigOr *proof, const U256 *ctx, uint32_t j);
/* The simulated branch of a DLEQ for log_G C1 = log_PK Y with the
   challenge E and the response Z: A = z G - e C1 and B = z PK - e Y. Each
   (E, Z) gives a branch that satisfies its two equations, so only the split
   binds a bit proof. The points must be on the curve. */
void sig_simulate(G1 *a, G1 *b, const G1 *pk, const G1 *c1, const G1 *y, const U256 *e, const U256 *z);
/* The proof that CT encrypts M with the nonce R under PK, for CTX, with
   the nonce w = the chain of thr_hash from RAND_NONCE_TAG over r mod r and
   the statement, mod r. SIG_REFUSED when a point is off the curve, else
   SIG_OK. A wrong M or R gives a proof that sig_rand_verify refuses. */
SigResult sig_rand_prove(SigDleq *out, const G1 *pk, const ElgCipher *ct, const U256 *m, const U256 *r, const U256 *ctx);
/* SIG_OK when z G = A + e C1 and z PK = B + e V, with V = sig_plain(CT,
   M) and e = sig_rand_challenge. SIG_REFUSED when a point is off the
   curve (before the proof), SIG_BAD_SUM when an equation fails. */
SigResult sig_rand_verify(const SigDleq *proof, const G1 *pk, const ElgCipher *ct, const U256 *m, const U256 *ctx);
/* The bit proof that CT encrypts BIT with the nonce R under PK, for CTX
   and the slot J. The real branch has the nonce w, the simulated one the
   full word e_sim and z_sim, each from the chain of thr_hash from
   ONEHOT_NONCE_TAG over r mod r and the statement; the real challenge is e
   - e_sim modulo 2^256. SIG_REFUSED when BIT is above 1 or a point is off
   the curve, else SIG_OK. */
SigResult sig_or_prove(SigOr *out, const G1 *pk, const ElgCipher *ct, uint32_t bit, const U256 *r, const U256 *ctx, uint32_t j);
/* SIG_OK when e0 + e1 = e modulo 2^256 (e = sig_or_challenge) and the two
   equations of each branch hold (branch 1 with C2 - G). SIG_REFUSED when a
   point is off the curve (before the proof), else SIG_BAD_SLOT. */
SigResult sig_or_verify(const SigOr *proof, const G1 *pk, const ElgCipher *ct, const U256 *ctx, uint32_t j);
/* The ballot for CHOICE of K slots under PK: CTS[j] encrypts 1 for j =
   CHOICE and 0 else, with the nonce r_j = hash(hash(BALLOT_TAG, SEED), j)
   mod r, SLOTS[j] its bit proof, and SUM the proof of randomness of the
   pointwise sum for m = 1 with the nonce sum r_j mod r. SEED is fresh
   secret entropy for each ballot. SIG_REFUSED when K is 0 or above
   SIG_K_MAX, CHOICE is K or more or PK is off the curve (before any
   write), else SIG_OK. */
SigResult sig_onehot_prove(ElgCipher *cts, SigOr *slots, SigDleq *sum, const G1 *pk, uint32_t k, uint32_t choice, const U256 *seed, const U256 *ctx);
/* In order: SIG_REFUSED when K is 0 or above SIG_K_MAX or a point is off
   the curve (before any proof), SIG_BAD_SLOT for the first slot whose bit
   proof fails, then the result of sig_rand_verify on the pointwise sum
   for m = 1. */
SigResult sig_onehot_verify(const ElgCipher *cts, const SigOr *slots, const SigDleq *sum, const G1 *pk, uint32_t k, const U256 *ctx);

#endif
