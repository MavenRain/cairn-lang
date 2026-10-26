/* Host ecrecover regressions (slice S7 b4, survey section 6): the 0x01 rules
   of front/secp256k1.c. Each vector was confirmed with a direct `evm run` to
   0x..01 (2026-10-23, evm 1.14.12; ~/Documents/intent-lang-work/S7-ec-confirm.sh). */
#include <stdio.h>
#include <string.h>

#include "front/secp256k1.h"
#include "keccak.h"

/* The word of HEX (64 digits or fewer, no prefix). */
static U256 word(const char *hex) {
  U256 x;
  u256_from_u64(&x, 0);
  for (; *hex != '\0'; hex++)
    u256_scale_add(&x, 16u, (uint32_t)(*hex <= '9' ? *hex - '0' : *hex - 'a' + 10));
  return x;
}

static int check(const char *name, const U256 *h, uint64_t v, const U256 *r, const U256 *s, const U256 *want) {
  U256 out;
  U256 vw;
  u256_from_u64(&vw, v);
  secp_recover(&out, h, &vw, r, s);
  if (u256_cmp(&out, want) == 0)
    return 0;
  fprintf(stderr, "FAIL ecrecover %s: %016llx%016llx%016llx\n", name, (unsigned long long)out.limb[2],
          (unsigned long long)out.limb[1], (unsigned long long)out.limb[0]);
  return 1;
}

static void put_hex(const U256 *x) {
  printf("%016llx%016llx%016llx%016llx", (unsigned long long)x->limb[3], (unsigned long long)x->limb[2],
         (unsigned long long)x->limb[1], (unsigned long long)x->limb[0]);
}

/* The address of the point Q: the low 160 bits of keccak256(Q.x, Q.y). */
static void address_of(U256 *out, const G1 *q) {
  unsigned char data[64];
  unsigned char digest[32];
  U256 w;
  uint32_t i;
  fe_to_u256(&w, &q->x);
  for (i = 0; i < 32u; i++)
    data[i] = (unsigned char)(w.limb[3u - i / 8u] >> (56u - 8u * (i % 8u)));
  fe_to_u256(&w, &q->y);
  for (i = 0; i < 32u; i++)
    data[32u + i] = (unsigned char)(w.limb[3u - i / 8u] >> (56u - 8u * (i % 8u)));
  keccak256(data, sizeof data, digest);
  u256_from_u64(out, 0);
  for (i = 12; i < 32u; i++)
    out->limb[(31u - i) / 8u] |= (uint64_t)digest[i] << (8u * ((31u - i) % 8u));
}

/* Vector mode (slice S8 b4, survey section 6): `secptool sign D K H` signs
   the hash H with the key D and the nonce K (hex, no prefix) and prints
   `addr A`, `low V R S` (S <= n / 2) and `high V R S` (r, n - s, the other
   v). Both forms must recover A through secp_recover, else exit 1. */
static int sign(const char *dhex, const char *khex, const char *hhex) {
  U256 d = word(dhex);
  U256 k = word(khex);
  U256 h = word(hhex);
  U256 gx = word("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
  U256 gy = word("483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");
  U256 n = word("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  U256 rw;
  U256 sw;
  U256 yw;
  U256 high;
  U256 addr;
  Fe fd;
  Fe fk;
  Fe fr;
  Fe fe;
  Fe fs;
  G1 g;
  G1 q;
  G1 rp;
  uint64_t v;
  int fail = 0;
  fe_from_u256(&g.x, &gx, &SECP_P);
  fe_from_u256(&g.y, &gy, &SECP_P);
  if (!fe_from_u256(&fd, &d, &SECP_N) || !fe_from_u256(&fk, &k, &SECP_N) || !fe_from_u256(&fe, &h, &SECP_N)) {
    fprintf(stderr, "FAIL sign: D, K and H must be below n\n");
    return 1;
  }
  ec0_mul(&q, &g, &d, &SECP_P);
  address_of(&addr, &q);
  ec0_mul(&rp, &g, &k, &SECP_P);
  fe_to_u256(&rw, &rp.x);
  if (!fe_from_u256(&fr, &rw, &SECP_N)) {
    fprintf(stderr, "FAIL sign: k G has an x of n or more; take another K\n");
    return 1;
  }
  fe_to_u256(&yw, &rp.y);
  v = 27u + (yw.limb[0] & 1u);
  /* s = k^-1 (h + r d) mod n. */
  fe_mul(&fs, &fr, &fd, &SECP_N);
  fe_add(&fs, &fs, &fe, &SECP_N);
  fe_inv(&fk, &fk, &SECP_N);
  fe_mul(&fs, &fs, &fk, &SECP_N);
  fe_to_u256(&sw, &fs);
  u256_sub(&high, &n, &sw);
  if (u256_cmp(&sw, &high) > 0) {
    U256 t = sw;
    sw = high;
    high = t;
    v = 55u - v;
  }
  printf("addr ");
  put_hex(&addr);
  printf("\nlow %llu ", (unsigned long long)v);
  put_hex(&rw);
  printf(" ");
  put_hex(&sw);
  printf("\nhigh %llu ", (unsigned long long)(55u - v));
  put_hex(&rw);
  printf(" ");
  put_hex(&high);
  printf("\n");
  fail += check("sign low", &h, v, &rw, &sw, &addr);
  fail += check("sign high", &h, 55u - v, &rw, &high, &addr);
  return fail != 0;
}

int main(int argc, char **argv) {
  /* go-ethereum core/vm/testdata/precompiles/ecRecover.json, CallEcrecover0:
     r is the hash word. */
  U256 h = word("38d18acb67d25c8bb9942764b62f18e17054f66a817bd4295423adf9ed98873e");
  U256 s = word("789d1dd423d25f0772d2748d60f7e4b81bb14d086eba8e8e8efb6dcff8a4ae02");
  U256 addr = word("ceaccac640adf55b2028469bd36ba501f28b699d");
  U256 n = word("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  U256 h1 = word("38d18acb67d25c8bb9942764b62f18e17054f66a817bd4295423adf9ed98873f");
  U256 other = word("89eea7f8bed00e8eb90e9fb33ee264640a9cb429");
  U256 zero;
  U256 high;
  int fail = 0;
  if (argc == 5 && strcmp(argv[1], "sign") == 0)
    return sign(argv[2], argv[3], argv[4]);
  u256_from_u64(&zero, 0);
  u256_sub(&high, &n, &s);
  fail += check("go-ethereum vector", &h, 27, &h, &s, &addr);
  fail += check("v 29", &h, 29, &h, &s, &zero);
  fail += check("r 0", &h, 27, &zero, &s, &zero);
  fail += check("s n", &h, 27, &h, &n, &zero);
  /* No EIP-2 rule in 0x01: (r, n - s, the other v: 28 for 27, 27 for 28)
     is the same signer. */
  fail += check("high s", &h, 28, &h, &high, &addr);
  fail += check("wrong h", &h1, 27, &h, &s, &other);
  if (fail != 0)
    return 1;
  puts("ecrecover regressions: passed");
  return 0;
}
