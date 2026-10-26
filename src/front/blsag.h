/* bLSAG ring signatures over secp256k1 in the host (docs/PLAN.md section 8,
   builder call S8, slice B2a). The kernel verify definition (B3) must use the
   same hash chain order as front/blsag.c.

   Words are U256 (front/u256.h), points are affine GroupPoint (front/group.h)
   and hash(a, b) is the kit `hash`: SHA-256 of the 64 packed big endian
   bytes.
   The member index i runs from 0 in constructor order; the word c_1 of the
   plan is the challenge of member 0. */
#ifndef LANG_FRONT_BLSAG_H
#define LANG_FRONT_BLSAG_H

#include <stdint.h>

#include "front/group.h"
#include "front/u256.h"

/* The largest ring (O28). */
#define BLSAG_RING_MAX 28u
/* H_p gives up after this many counters. Each counter succeeds with a chance
   near 0.4, so the bound is not reached. */
#define BLSAG_HP_TRIES 256u

/* The domain tag: the ASCII bytes of "cairn" as a word (builder default). */
extern const U256 BLSAG_TAG;

typedef enum {
  BLSAG_REJECT,
  BLSAG_ACCEPT,
  BLSAG_TRAP /* the ring size is bad, the key image is off the curve or H_p found no point */
} BlsagVerdict;

/* X mod n for any word (one subtraction at most, as 2^256 < 2 n). */
void blsag_reduce(U256 *out, const U256 *x);
/* The test key x_i = hash(tag, i) mod n. */
void blsag_key(U256 *out, uint64_t i);
/* H_p(P) by try and increment: x_k = hash(hash(P.x, P.y), k) from k = 0; the
   first x_k below p with x_k^3 + 7 a square gives the point, with y the
   smaller root. *COUNTER gets k. 1, or 0 when BLSAG_HP_TRIES counters fail.
   OUT can be P. */
int blsag_hash_point(GroupPoint *out, uint32_t *counter, const GroupPoint *p);
/* The ring digest: D_0 = hash(P_0.x, P_0.y) and D_k = hash(hash(D_(k minus
   1), P_k.x), P_k.y). N is 1 to BLSAG_RING_MAX. */
void blsag_digest(U256 *out, const GroupPoint *ring, uint32_t n);
/* Signs the vote (POLL, CHOICE) for the ring with the key X, whose point x G
   must be in the ring. SIG gets the N + 3 words c_1, s_1 to s_n, I.x, I.y.
   Deterministic scalars bind tag, X, ring digest, poll, choice, index and
   role (private alpha or public response). 1, or 0 when N is 0 or above
   BLSAG_RING_MAX, X is 0 or GROUP_N or more, or x G is not in the ring. */
int blsag_sign(U256 *sig, const GroupPoint *ring, uint32_t n, const U256 *x, const U256 *poll, const U256 *choice);
/* Checks the N + 3 words SIG for the ring and the vote: accept when the
   chain closes (c_(n+1) = c_1), each s_i is below GROUP_N and I is not
   (0, 0). No range test on c_1. */
BlsagVerdict blsag_verify(const GroupPoint *ring, uint32_t n, const U256 *poll, const U256 *choice, const U256 *sig);

#endif
