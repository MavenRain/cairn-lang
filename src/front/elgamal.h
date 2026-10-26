/* Exponential ElGamal over BN254 G1 in the host (docs/PLAN.md section 9,
   slice S5 b5, Q-S5-4 (a)). Not guest visible: the guest Enc monad is
   sealed (docs/SPEC.md rule O7), and this file is a host model of it.

   The message m is the point m G, so the sum of two ciphertexts is the
   ciphertext of the sum of the plaintexts. Decrypt finds m by baby step
   giant step, thus only for a small m: below 2^bits, with bits at most
   ELG_BITS_MAX. Points are affine G1 (front/bn254.h) and words are U256. */
#ifndef LANG_FRONT_ELGAMAL_H
#define LANG_FRONT_ELGAMAL_H

#include <stdint.h>

#include "front/bn254.h"
#include "front/u256.h"

/* The largest search: 2^20 baby steps and 2^20 giant steps. */
#define ELG_BITS_MAX 40u

/* The domain tag of the nonces: the ASCII bytes of "elgamal" as a word. */
extern const U256 ELG_TAG;

typedef struct {
  G1 c1; /* r G */
  G1 c2; /* m G + r PK */
} ElgCipher;

typedef enum {
  ELG_FOUND,
  ELG_NOT_FOUND, /* m is 2^bits or more */
  ELG_REFUSED,   /* bits is above ELG_BITS_MAX or a point is off the curve */
  ELG_NO_MEMORY  /* no memory for the baby step table */
} ElgResult;

/* The generator (1, 2). */
void elg_generator(G1 *out);
/* Minus P. The point at infinity stays. OUT can be P. */
void g1_neg(G1 *out, const G1 *p);
int g1_eq(const G1 *a, const G1 *b);
/* The nonce r_i = hash(ELG_TAG, i) mod r, as blsag_key. There is no other
   source of randomness. */
void elg_nonce(U256 *out, uint64_t i);
/* The public key sk G. */
void elg_public(G1 *out, const U256 *sk);
/* (r G, m G + r PK). PK must be on the curve. */
void elg_encrypt(ElgCipher *out, const G1 *pk, const U256 *m, const U256 *r);
/* The pointwise sum: the ciphertext of the sum of the plaintexts. OUT can be
   A or B. */
void elg_add(ElgCipher *out, const ElgCipher *a, const ElgCipher *b);
/* (k C1, k C2): the ciphertext of k m for a public K. OUT can be CT. */
void elg_scale(ElgCipher *out, const ElgCipher *ct, const U256 *k);
/* The m below 2^BITS with m G = POINT, by baby step giant step (slice S6
   b1: the search of elg_decrypt, for the threshold opening). *M gets m on
   ELG_FOUND. ELG_REFUSED when BITS is above ELG_BITS_MAX or POINT is off the
   curve. */
ElgResult elg_log(uint64_t *m, const G1 *point, uint32_t bits);
/* C2 minus sk C1 = m G, then elg_log. *M gets m on ELG_FOUND. */
ElgResult elg_decrypt(uint64_t *m, const U256 *sk, const ElgCipher *ct, uint32_t bits);

#endif
