/* Distributed key generation and resharing of the threshold keys in the
   host (docs/PLAN.md section 3.2, the Deal, Reshare row; slice S6 b5,
   Q-S6-4 (a)). Not guest visible: the guest has no def of Deal or Reshare.

   A Pedersen DKG with Feldman commitments, as in FROST KeyGen. Member i
   picks a polynomial f_i of degree t - 1, publishes the commitments A_ik =
   a_ik G and a Schnorr proof of knowledge of a_i0, and sends f_i(j) to
   member j. Member j tests f_i(j) G = sum_k j^k A_ik and the proof, then
   takes s_j = sum_i f_i(j); the public key is sum_i A_i0. The out type is
   ThrShare, so thr_partial, thr_open and thr_close do not change.

   A reshare keeps the public key: each member i of a quorum Q of the old
   threshold deals lambda_i(Q) s_i (Lagrange at 0 over Q) with a polynomial
   of the new degree. The constant commitment of dealer i must be
   lambda_i(Q) Y_i, with Y_i its old verification key, and their sum must be
   the public key; otherwise the reshare aborts. A key change needs a fresh
   DKG. The bias of the Pedersen DKG (Gennaro, Jarecki, Krawczyk, Rabin) is
   the assumption V5 of docs/VALIDATION.md.

   Callers agree on the threshold and member roster before a run, bind
   each dealer identifier to an authenticated sender, reliably broadcast
   the same commitments to every member, and deliver shares over
   authenticated confidential channels. The caller supplies these
   communication guarantees. */
#ifndef LANG_FRONT_DKG_H
#define LANG_FRONT_DKG_H

#include <stdint.h>

#include "front/threshold.h"

/* The domain tags: the ASCII bytes of "dkg" (the proof challenge chain)
   and "dkgw" (the proof nonce) as words. */
extern const U256 DKG_TAG;
extern const U256 DKG_NONCE_TAG;

/* The public part of the dealing of member I for the threshold T: the
   Feldman commitments COMMIT[k] = a_k G for k below T and the Schnorr
   proof (R, MU) of the knowledge of a_0, with R = w G and MU = w + c a_0
   mod GROUP_N. */
typedef struct {
  uint32_t i;
  uint32_t t;
  GroupPoint commit[THR_N_MAX];
  GroupPoint r;
  U256 mu;
} DkgDeal;

/* The proof challenge c: the chain of thr_hash from DKG_TAG over I (as a
   word), A0.x, A0.y, R.x, R.y (5 hashes), not reduced. */
void dkg_challenge(U256 *c, uint32_t i, const GroupPoint *a0, const GroupPoint *r);
/* The dealing of member I for the threshold T over N members: a_0 = A0,
   a_k = thr_nonce(SEED, k) for k = 1 to T - 1, the proof nonce w = the hash
   chain of DKG_NONCE_TAG, SEED, A0, I mod GROUP_N, and SHARES[j - 1] = f(j)
   for j = 1 to N (the private shares, one to each member). SEED is fresh
   secret entropy for each dealing, as for thr_deal; a fresh DKG takes A0 =
   thr_nonce(SEED, 0). 1, or 0 unless 1 <= I <= THR_N_MAX, 1 <= T <= N <=
   THR_N_MAX and A0 < GROUP_N. */
int dkg_deal(DkgDeal *out, U256 *shares, const U256 *a0, const U256 *seed, uint32_t i, uint32_t t, uint32_t n);
/* The test of member J on the share SHARE from DEAL: 1 when T is 1 to
   THR_N_MAX, SHARE and MU are below GROUP_N, the points of DEAL are on the
   curve, MU G = R + c COMMIT[0] (c = dkg_challenge mod GROUP_N) and SHARE G
   = sum_k J^k COMMIT[k]. */
int dkg_verify(const DkgDeal *deal, const U256 *share, uint32_t j);
/* The initial DKG share of member J from the K DEALS, with SHARES[m] =
   f_m(J) from DEALS[m]: s_J = the sum of the shares mod GROUP_N, the key
   s_J G, and *PK = the sum of the constant commitments. T and N are the
   previously agreed threshold and member count, from trusted caller
   state. All N dealers must contribute: their identifiers are exactly
   1 to N, in any order. 1, or 0 unless 1 <= T <= N <= THR_N_MAX, K = N,
   J is 1 to N, every deal has threshold T, every dealer is distinct and
   1 to N, and every deal passes dkg_verify. */
int dkg_combine(ThrShare *out, GroupPoint *pk, const DkgDeal *deals, const U256 *shares, uint32_t k, uint32_t t, uint32_t n, uint32_t j);
/* The reshare dealing of OLD, a member of the quorum QUORUM (Q old share
   indices), for the new threshold T over N members: dkg_deal with I =
   OLD.x and a_0 = lambda(QUORUM) OLD.s mod GROUP_N. 1, or 0 when the
   indices are not distinct and 1 to THR_N_MAX, OLD.x is not in QUORUM, or
   dkg_deal refuses. */
int dkg_reshare(DkgDeal *out, U256 *shares, const ThrShare *old, const uint32_t *quorum, uint32_t q, const U256 *seed, uint32_t t, uint32_t n);
/* The new share of member J from the Q reshare DEALS (their dealers are
   the quorum), with KEYS[x - 1] = Y_x for the OLD_N old shares, the old
   threshold OLD_T and the public key PK. 0 (abort) when Q is below OLD_T
   or above OLD_N, OLD_N is above THR_N_MAX, a dealer is 0, above OLD_N or
   repeated, a constant commitment is not lambda_i(Q) Y_i, or their sum is
   not PK; otherwise verifies all deals at J with a common new threshold
   and sums their shares into OUT. */
int dkg_reshare_combine(ThrShare *out, const DkgDeal *deals, const U256 *shares, uint32_t q, const GroupPoint *keys, uint32_t old_t, uint32_t old_n, const GroupPoint *pk, uint32_t j);

#endif
