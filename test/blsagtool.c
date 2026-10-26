/* Public signatures must not disclose the private scalar across rings. */
#include <stdio.h>

#include "front/blsag.h"

static int same_point(const GroupPoint *a, const GroupPoint *b) {
  return fe_eq(&a->x, &b->x) && fe_eq(&a->y, &b->y);
}

/* Recover the alleged key from two public scalars and a challenge delta. */
static int recovers(const GroupPoint *generator, const GroupPoint *public_key, const U256 *a, const U256 *b, const U256 *ca, const U256 *cb) {
  U256 wa;
  U256 wb;
  U256 scalar;
  Fe fa;
  Fe fb;
  Fe numerator;
  Fe denominator;
  Fe inverse;
  Fe key;
  GroupPoint recovered;
  blsag_reduce(&wa, a);
  blsag_reduce(&wb, b);
  fe_from_u256(&fa, &wa, &GROUP_N);
  fe_from_u256(&fb, &wb, &GROUP_N);
  fe_sub(&numerator, &fa, &fb, &GROUP_N);
  blsag_reduce(&wa, ca);
  blsag_reduce(&wb, cb);
  fe_from_u256(&fa, &wa, &GROUP_N);
  fe_from_u256(&fb, &wb, &GROUP_N);
  fe_sub(&denominator, &fa, &fb, &GROUP_N);
  fe_inv(&inverse, &denominator, &GROUP_N);
  fe_mul(&key, &numerator, &inverse, &GROUP_N);
  fe_to_u256(&scalar, &key);
  group_mul(&recovered, generator, &scalar);
  return same_point(&recovered, public_key);
}

int main(void) {
  const U256 zero = {{0u, 0u, 0u, 0u}};
  const U256 one = {{1u, 0u, 0u, 0u}};
  const U256 two = {{2u, 0u, 0u, 0u}};
  const U256 key = {{71u, 0u, 0u, 0u}};
  GroupPoint generator;
  GroupPoint ring[3];
  GroupPoint changed[2];
  GroupPoint swapped[2];
  GroupPoint infinity;
  U256 original[5];
  U256 other[5];
  U256 moved[5];
  U256 again[5];
  uint32_t i;
  int ok = 1;
  group_gen(&generator);
  if (!group_from_words(&infinity, &zero, &zero))
    return 1;
  group_mul(&ring[0], &generator, &key);
  ring[1] = generator;
  group_mul(&ring[2], &generator, &two);
  changed[0] = ring[0];
  changed[1] = ring[2];
  swapped[0] = ring[1];
  swapped[1] = ring[0];
  if (!blsag_sign(original, ring, 2u, &key, &zero, &one)
      || !blsag_sign(other, changed, 2u, &key, &zero, &one)
      || !blsag_sign(moved, swapped, 2u, &key, &zero, &one)
      || !blsag_sign(again, ring, 2u, &key, &zero, &one)) {
    fputs("FAIL bLSAG signing setup\n", stderr);
    return 1;
  }
  if (blsag_verify(ring, 2u, &zero, &one, original) != BLSAG_ACCEPT
      || blsag_verify(changed, 2u, &zero, &one, other) != BLSAG_ACCEPT
      || blsag_verify(swapped, 2u, &zero, &one, moved) != BLSAG_ACCEPT) {
    fputs("FAIL bLSAG valid signatures\n", stderr);
    ok = 0;
  }
  /* Same signer position, different ring: x = (s_a - s_b)/(c_b - c_a). */
  if (recovers(&generator, &ring[0], &original[1], &other[1], &other[0], &original[0])) {
    fputs("FAIL bLSAG ring change discloses private key\n", stderr);
    ok = 0;
  }
  /* A moved signer must not publish the old alpha as a decoy response. */
  if (recovers(&generator, &ring[0], &moved[1], &original[1], &original[0], &zero)) {
    fputs("FAIL bLSAG signer move discloses private key\n", stderr);
    ok = 0;
  }
  for (i = 0; i < 5u; i++) {
    if (u256_cmp(&original[i], &again[i]) != 0) {
      fputs("FAIL bLSAG deterministic signing\n", stderr);
      ok = 0;
    }
  }
  if (blsag_sign(again, &infinity, 1u, &zero, &zero, &one)) {
    fputs("FAIL bLSAG zero key produces an unverifiable signature\n", stderr);
    ok = 0;
  }
  puts(ok ? "bLSAG regressions: passed" : "bLSAG regressions: FAILED");
  return ok ? 0 : 1;
}
