/* Host group regressions (slice S4, Q-S4-7): the secp256k1 group of
   front/group.h against SEC 2 version 2.0, section 2.4.1. G is the SEC 2
   generator and lies on the curve, GROUP_N is the order n, n G is the
   point at infinity (0, 0) and (n - 1) G = -G. */
#include <stdio.h>

#include "front/group.h"

/* The word of HEX (64 digits or fewer, no prefix). */
static U256 word(const char *hex) {
  U256 x;
  u256_from_u64(&x, 0);
  for (; *hex != '\0'; hex++)
    u256_scale_add(&x, 16u, (uint32_t)(*hex <= '9' ? *hex - '0' : *hex - 'a' + 10));
  return x;
}

static int check(const char *name, int ok) {
  if (!ok)
    fprintf(stderr, "FAIL group %s\n", name);
  return !ok;
}

int main(void) {
  /* SEC 2 section 2.4.1: G in uncompressed form and the order n. */
  U256 gx = word("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
  U256 gy = word("483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");
  U256 n = word("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  U256 n1;
  U256 one;
  U256 zero;
  U256 x;
  U256 y;
  Fe f;
  GroupPoint g;
  GroupPoint p;
  GroupPoint neg;
  int fail = 0;
  u256_from_u64(&zero, 0);
  u256_from_u64(&one, 1);
  u256_sub(&n1, &n, &one);
  group_gen(&g);
  group_to_words(&x, &y, &g);
  fail += check("G is the SEC 2 generator", u256_cmp(&x, &gx) == 0 && u256_cmp(&y, &gy) == 0);
  fail += check("G on the curve", group_on_curve(&g));
  fail += check("G from its words", group_from_words(&p, &gx, &gy) && group_eq(&p, &g));
  /* n - 1 < GROUP_N <= n. */
  fail += check("GROUP_N is n", fe_from_u256(&f, &n1, &GROUP_N) && !fe_from_u256(&f, &n, &GROUP_N));
  group_mul(&p, &g, &n);
  group_to_words(&x, &y, &p);
  fail += check("n G = (0, 0)", u256_cmp(&x, &zero) == 0 && u256_cmp(&y, &zero) == 0);
  group_mul(&p, &g, &n1);
  group_neg(&neg, &g);
  fail += check("(n - 1) G = -G", group_on_curve(&p) && group_eq(&p, &neg));
  /* (1, 3) is off the curve: 3 * 3 = 9, 1 + 7 = 8. */
  u256_from_u64(&x, 1);
  u256_from_u64(&y, 3);
  fail += check("(1, 3) refused", !group_from_words(&p, &x, &y));
  if (fail != 0)
    return 1;
  puts("group regressions: passed");
  return 0;
}
