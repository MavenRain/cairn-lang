/* SHA-256 (FIPS 180-4) for the guest `hash` and the host proofs (slice
   S4). */
#ifndef LANG_SHA256_H
#define LANG_SHA256_H
#include <stddef.h>

#include "front/u256.h"

void sha256(const unsigned char *data, size_t size, unsigned char digest[32]);
/* OUT = SHA-256 of the 64 bytes of A then B, each big endian (limb 3
   first), read as a big endian word: the guest `hash a b`. OUT can be A
   or B. */
void sha256_words(U256 *out, const U256 *a, const U256 *b);
#endif
