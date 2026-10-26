/* Host ElGamal regressions (slice S5 b5): the round trip, the sum and the
   scale of ciphertexts, and the refusals of front/elgamal.c. With the two
   arguments BITS M it decrypts one ciphertext of M by a search of BITS bits
   and prints the result (the timing run of docs/VALIDATION.md). */
#include <stdio.h>
#include <stdlib.h>

#include "front/blsag.h"
#include "front/elgamal.h"

/* The ciphertext of M under PK with the nonce r_I. */
static void encrypt_u64(ElgCipher *out, const G1 *pk, uint64_t m, uint64_t i) {
  U256 w;
  U256 r;
  u256_from_u64(&w, m);
  elg_nonce(&r, i);
  elg_encrypt(out, pk, &w, &r);
}

/* 1 when CT decrypts under SK at BITS to EXPECT, with the plaintext WANT on
   ELG_FOUND. */
static int row(const char *name, const U256 *sk, const ElgCipher *ct, uint32_t bits, ElgResult expect, uint64_t want) {
  uint64_t m = 0;
  ElgResult got = elg_decrypt(&m, sk, ct, bits);
  if (got == expect && (expect != ELG_FOUND || m == want))
    return 1;
  fprintf(stderr, "FAIL elgamal %s: result %d, m %llu\n", name, (int)got, (unsigned long long)m);
  return 0;
}

int main(int argc, char **argv) {
  U256 sk;
  U256 k;
  G1 pk;
  ElgCipher a;
  ElgCipher b;
  ElgCipher sum;
  ElgCipher bad;
  int ok = 1;
  blsag_key(&sk, 5u);
  elg_public(&pk, &sk);
  if (argc == 3) {
    uint64_t m = 0;
    ElgResult got;
    encrypt_u64(&a, &pk, strtoull(argv[2], NULL, 10), 0u);
    got = elg_decrypt(&m, &sk, &a, (uint32_t)strtoul(argv[1], NULL, 10));
    printf("elgamal timing: result %d, m %llu\n", (int)got, (unsigned long long)m);
    return got == ELG_FOUND ? 0 : 1;
  }
  encrypt_u64(&a, &pk, 0u, 1u);
  ok &= row("round trip 0", &sk, &a, 16u, ELG_FOUND, 0u);
  encrypt_u64(&a, &pk, 1u, 2u);
  ok &= row("round trip 1", &sk, &a, 16u, ELG_FOUND, 1u);
  encrypt_u64(&a, &pk, 65535u, 3u);
  ok &= row("round trip 2^16 - 1", &sk, &a, 16u, ELG_FOUND, 65535u);
  encrypt_u64(&a, &pk, 300u, 4u);
  encrypt_u64(&b, &pk, 1200u, 5u);
  elg_add(&sum, &a, &b);
  ok &= row("add 300 + 1200", &sk, &sum, 16u, ELG_FOUND, 1500u);
  u256_from_u64(&k, 3u);
  encrypt_u64(&a, &pk, 500u, 6u);
  elg_scale(&a, &a, &k);
  ok &= row("scale 3 * 500", &sk, &a, 16u, ELG_FOUND, 1500u);
  encrypt_u64(&a, &pk, 65536u, 7u);
  ok &= row("2^16 at 16 bits not found", &sk, &a, 16u, ELG_NOT_FOUND, 0u);
  encrypt_u64(&a, &pk, 1u, 8u);
  ok &= row("41 bits refused", &sk, &a, 41u, ELG_REFUSED, 0u);
  bad = a;
  fe_from_u64(&bad.c1.x, 1u);
  fe_from_u64(&bad.c1.y, 3u);
  ok &= row("off-curve C1 refused", &sk, &bad, 16u, ELG_REFUSED, 0u);
  if (!ok)
    return 1;
  puts("elgamal regressions: passed");
  return 0;
}
