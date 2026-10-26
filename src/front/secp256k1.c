/* secp256k1 recovery for the host ecrecover (slice S7 b4, survey section 6). */
#include "front/secp256k1.h"

#include "keccak.h"

const Modulus SECP_P = {{0xfffffc2fu, 0xfffffffeu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}};
const Modulus SECP_N = {{0xd0364141u, 0xbfd25e8cu, 0xaf48a03bu, 0xbaaedce6u, 0xfffffffeu, 0xffffffffu, 0xffffffffu, 0xffffffffu}};

static const U256 N_WORD = {{0xbfd25e8cd0364141ull, 0xbaaedce6af48a03bull, 0xfffffffffffffffeull, 0xffffffffffffffffull}};
/* (p + 1) / 4: p = 3 mod 4, so A^((p + 1) / 4) is a square root of a square A. */
static const U256 SQRT_EXP = {{0xffffffffbfffff0cull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0x3fffffffffffffffull}};
static const U256 GX = {{0x59f2815b16f81798ull, 0x029bfcdb2dce28d9ull, 0x55a06295ce870b07ull, 0x79be667ef9dcbbacull}};
static const U256 GY = {{0x9c47d08ffb10d4b8ull, 0xfd17b448a6855419ull, 0x5da4fbfc0e1108a8ull, 0x483ada7726a3c465ull}};

/* X mod n for any word X (X < 2n). */
static void scalar_of(Fe *out, const U256 *x) {
  U256 t;
  if (fe_from_u256(out, x, &SECP_N))
    return;
  u256_sub(&t, x, &N_WORD);
  fe_from_u256(out, &t, &SECP_N);
}

/* The point with x = X and a y of parity ODD, or 0 when X is no x on the
   curve y^2 = x^3 + 7. */
static int lift_x(G1 *out, const Fe *x, uint32_t odd) {
  Fe rhs;
  Fe y;
  Fe sq;
  Fe seven;
  U256 w;
  fe_from_u64(&seven, 7);
  fe_mul(&rhs, x, x, &SECP_P);
  fe_mul(&rhs, &rhs, x, &SECP_P);
  fe_add(&rhs, &rhs, &seven, &SECP_P);
  fe_pow(&y, &rhs, &SQRT_EXP, &SECP_P);
  fe_mul(&sq, &y, &y, &SECP_P);
  if (!fe_eq(&sq, &rhs))
    return 0;
  fe_to_u256(&w, &y);
  if ((uint32_t)(w.limb[0] & 1u) != odd)
    fe_neg(&y, &y, &SECP_P);
  out->x = *x;
  out->y = y;
  return 1;
}

/* The 32 big-endian bytes of X at OUT. */
static void put_word(unsigned char *out, const U256 *x) {
  uint32_t i;
  for (i = 0; i < 32u; i++)
    out[i] = (unsigned char)(x->limb[3u - i / 8u] >> (56u - 8u * (i % 8u)));
}

int secp_recover(U256 *out, const U256 *h, const U256 *v, const U256 *r, const U256 *s) {
  U256 v27;
  U256 v28;
  U256 w;
  Fe zero;
  Fe fr;
  Fe fs;
  Fe x;
  Fe e;
  Fe rinv;
  Fe u1;
  Fe u2;
  G1 g;
  G1 rp;
  G1 q;
  unsigned char data[64];
  unsigned char digest[32];
  uint32_t k;
  u256_from_u64(out, 0);
  u256_from_u64(&v27, 27);
  u256_from_u64(&v28, 28);
  fe_from_u64(&zero, 0);
  if ((u256_cmp(v, &v27) != 0 && u256_cmp(v, &v28) != 0) || !fe_from_u256(&fr, r, &SECP_N) ||
      !fe_from_u256(&fs, s, &SECP_N) || fe_eq(&fr, &zero) || fe_eq(&fs, &zero))
    return 0;
  /* r < n < p, so r is an element of Fp too. */
  if (!fe_from_u256(&x, r, &SECP_P) || !lift_x(&rp, &x, (uint32_t)(u256_cmp(v, &v28) == 0)))
    return 0;
  scalar_of(&e, h);
  fe_inv(&rinv, &fr, &SECP_N);
  fe_mul(&u1, &e, &rinv, &SECP_N);
  fe_neg(&u1, &u1, &SECP_N);
  fe_mul(&u2, &fs, &rinv, &SECP_N);
  fe_from_u256(&g.x, &GX, &SECP_P);
  fe_from_u256(&g.y, &GY, &SECP_P);
  fe_to_u256(&w, &u1);
  ec0_mul(&g, &g, &w, &SECP_P);
  fe_to_u256(&w, &u2);
  ec0_mul(&q, &rp, &w, &SECP_P);
  ec0_add(&q, &g, &q, &SECP_P);
  /* (0, 0) is the point at infinity: it is not on the curve. */
  if (fe_eq(&q.x, &zero) && fe_eq(&q.y, &zero))
    return 0;
  fe_to_u256(&w, &q.x);
  put_word(data, &w);
  fe_to_u256(&w, &q.y);
  put_word(data + 32, &w);
  keccak256(data, sizeof data, digest);
  for (k = 12; k < 32u; k++)
    out->limb[(31u - k) / 8u] |= (uint64_t)digest[k] << (8u * ((31u - k) % 8u));
  return 1;
}
