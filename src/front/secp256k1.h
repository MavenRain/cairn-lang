/* secp256k1 public key recovery for the host ecrecover (slice S7 b4, survey
   section 6, USER ruling Q-S7-9): the rules of the 0x01 precompile over the
   generic fe_* and ec0_* of front/bn254.h, with the secp256k1 p and n as
   Modulus values.

   p = 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f
   n = 0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141 */
#ifndef LANG_FRONT_SECP256K1_H
#define LANG_FRONT_SECP256K1_H

#include "front/bn254.h"

extern const Modulus SECP_P;
extern const Modulus SECP_N;

/* The address that 0x01 gives for the hash H, the recovery id V and the
   signature (R, S): the low 160 bits of keccak256(Q.x, Q.y) with
   Q = R^-1 (S Rp - H G), Rp the point with x = R and the y parity V - 27.
   V must be 27 or 28 and 1 <= R, S < n; else, and for an R that is no x on
   the curve or a Q at infinity, OUT is the address 0 and the result 0. A
   high S is accepted (the precompile has no EIP-2 rule), so (R, n - S, the
   other V: 28 for 27, 27 for 28) recovers the same address. */
int secp_recover(U256 *out, const U256 *h, const U256 *v, const U256 *r, const U256 *s);

#endif
