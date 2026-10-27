/* Threshold Schnorr signatures of the appends under the append key, in the
   host (docs/PLAN.md section 3.3; slice S7 b1, Q-S7-2 to Q-S7-5 (a)). The
   signing is host C only: a share is never a guest value (Q-S6-4 (a)). The
   guest checks a signature with the builtin schnorrOk (schnorr_verify).

   FROST as in Komlo and Goldberg (SAC 2020, IACR ePrint 2020/852) and RFC
   9591, over the threshold shares of front/dkg.h. Keys and nonces are
   x-only: a point is its x and the smaller of its two y (group_lift_x,
   Q-S4-1 (a)). When the group key P or the group commitment R has the
   larger y, the signers negate their shares or their nonces, so the
   signature (R.x, z) satisfies z G = R + c P for the lifts of P.x and R.x.

   Round 1: signer i takes the nonces (d_i, e_i) from a fresh SEED and its
   share and publishes the commitments (D_i, E_i) = (d_i G, e_i G). Round 2:
   with the list B of the commitments of the signer set S, sorted by i,
   signer i computes the binding factor rho_i, R = sum_j (D_j + rho_j E_j),
   the challenge c and the share z_i = d_i + rho_i e_i + lambda_i s_i c,
   with lambda_i at 0 over S. The aggregator checks each share and takes z
   = sum_i z_i.

   SEED is fresh secret entropy for each signing session and is never
   reused: one nonce in two sessions with two commitment lists gives two z_i
   for one nonce and discloses s_i. Fixed seeds are for test vectors only.
   Use the nonces of a round 1 for one round 2 only. Callers authenticate
   each signer and send the same round 1 list to every signer. The caller
   supplies these communication guarantees, as for front/dkg.h. */
#ifndef LANG_FRONT_FROST_H
#define LANG_FRONT_FROST_H

#include <stdint.h>

#include "front/threshold.h"

/* The domain tags: the ASCII bytes of "cairn-auth" (the challenge chain),
   "cairn-authw" (the nonces) and "cairn-rho" (the binding factors) as
   words. */
extern const U256 AUTH_TAG;
extern const U256 AUTH_NONCE_TAG;
extern const U256 AUTH_RHO_TAG;

typedef enum {
  AUTH_OK,
  AUTH_BAD_SHARE, /* a signature share fails its check */
  AUTH_BAD_SIG,   /* z G is not R + c P */
  AUTH_REFUSED    /* an argument is out of range, or a point is off the curve or has no lift */
} AuthResult;

/* The private round 1 nonces of signer I. */
typedef struct {
  uint32_t i;
  U256 d;
  U256 e;
} AuthNonce;

/* The public round 1 commitments of signer I: D = d G and E = e G. */
typedef struct {
  uint32_t i;
  GroupPoint d;
  GroupPoint e;
} AuthCommit;

/* The signature share Z of signer I. */
typedef struct {
  uint32_t i;
  U256 z;
} AuthShare;

/* A signature: RX = R.x and Z below GROUP_N. */
typedef struct {
  U256 rx;
  U256 z;
} AuthSig;

/* Round 1 of SHARE: d and e = the chain of thr_hash from AUTH_NONCE_TAG
   over SEED, s, k for k = 0 and 1 (as a word), mod GROUP_N (the RFC 9591
   nonce_generate shape), and D = d G, E = e G. I = SHARE.x. */
void frost_commit(AuthNonce *nonce, AuthCommit *commit, const ThrShare *share, const U256 *seed);
/* The binding factor of signer I for the message (HEAD, E): the chain of
   thr_hash from AUTH_RHO_TAG over I (as a word), PX, HEAD, E and then j (as
   a word), D.x, D.y, E.x, E.y for each of the K commitments of LIST, mod
   GROUP_N. */
void frost_rho(U256 *rho, uint32_t i, const U256 *px, const U256 *head, const U256 *e, const AuthCommit *list, uint32_t k);
/* The challenge c: the chain of thr_hash from AUTH_TAG over PX, RX, HEAD,
   E (4 hashes), mod GROUP_N. */
void frost_challenge(U256 *c, const U256 *px, const U256 *rx, const U256 *head, const U256 *e);
/* Round 2 of SHARE with its NONCE for the message (HEAD, E) under the group
   key PK, with the LIST of the K commitments of the signer set. AUTH_OK, or
   AUTH_REFUSED unless 1 <= K <= THR_N_MAX, the list indices rise strictly
   from 1 to at most THR_N_MAX, every point is on the curve, PK and R are
   not the point at infinity, NONCE.i = SHARE.x, the list holds SHARE.x with
   the commitments of NONCE, and S and the nonces are below GROUP_N. */
AuthResult frost_sign(AuthShare *out, const ThrShare *share, const AuthNonce *nonce, const GroupPoint *pk, const U256 *head, const U256 *e, const AuthCommit *list, uint32_t k);
/* The signature from the K SHARES (SHARES[m].i = LIST[m].i) for the message
   (HEAD, E) under PK, with KEYS[x - 1] = Y_x for the N members and the
   threshold T. In order: AUTH_REFUSED unless 1 <= T <= N <= THR_N_MAX, T <=
   K, LIST passes the tests of frost_sign, each index is at most N, each Z
   is below GROUP_N and each key is on the curve; AUTH_BAD_SHARE with *AT =
   the index of the first signer whose z_i G is not D_i + rho_i E_i + c
   lambda_i Y_i (with the negations of frost_sign); otherwise
   schnorr_verify of (R.x, sum z_i). R and lambda depend on the signer set,
   so after a bad share the caller runs both rounds again with a new set. */
AuthResult frost_aggregate(AuthSig *out, uint32_t *at, const AuthShare *shares, const AuthCommit *list, uint32_t k, const GroupPoint *keys, uint32_t t, uint32_t n, const GroupPoint *pk, const U256 *head, const U256 *e);
/* The check of SIG for the message (HEAD, E) under the x-only key PX: P and
   R are the lifts of PX and SIG.rx. AUTH_REFUSED when Z is GROUP_N or more,
   or PX or RX has no lift; AUTH_OK when z G = R + c P (c =
   frost_challenge); otherwise AUTH_BAD_SIG. */
AuthResult schnorr_verify(const U256 *px, const AuthSig *sig, const U256 *head, const U256 *e);

#endif
