/* bLSAG ring signatures over BN254 G1 in the host (docs/PLAN.md section 8,
   builder call S8, slice B2a). See front/blsag.h.

   Builder defaults, each named in the commit message: the tag word, H_p by
   try and increment from counter 0, the ring digest fold, the challenge fold
   (tag, D, poll id, choice, I.x, I.y, L.x, L.y, R.x, R.y) and the nonce fold
   (tag, x, D, poll id, choice, i, role) mod r. */
#include "front/blsag.h"

#include "keccak.h"

const U256 BLSAG_TAG = {{0x6f72626974u, 0u, 0u, 0u}};

static const U256 ONE = {{1u, 0u, 0u, 0u}};

/* The generator (1, 2). */
static const G1 GEN = {{{1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}}, {{2u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}}};

/* The 32 big endian bytes of X at OUT (limb 3 first), as word_bytes in
   front/eval.c. */
static void word_bytes(const U256 *x, unsigned char *out) {
  uint32_t i;
  for (i = 0; i < 32u; i++)
    out[i] = (unsigned char)(x->limb[3u - i / 8u] >> (56u - 8u * (i % 8u)));
}

/* hash(a, b), as hash_words in front/eval.c. OUT can be A or B. */
static void hash2(U256 *out, const U256 *a, const U256 *b) {
  unsigned char data[64];
  unsigned char digest[32];
  uint32_t k;
  uint32_t j;
  word_bytes(a, data);
  word_bytes(b, data + 32);
  keccak256(data, sizeof data, digest);
  for (k = 0; k < 4u; k++) {
    out->limb[3u - k] = 0;
    for (j = 0; j < 8u; j++)
      out->limb[3u - k] = (out->limb[3u - k] << 8) | digest[8u * k + j];
  }
}

/* The modulus F as a word. */
static void modulus_word(U256 *out, const Modulus *f) {
  uint32_t i;
  for (i = 0; i < 4u; i++)
    out->limb[i] = (uint64_t)f->w[2u * i] | ((uint64_t)f->w[2u * i + 1u] << 32);
}

static int word_zero(const U256 *x) {
  return x->limb[0] == 0 && x->limb[1] == 0 && x->limb[2] == 0 && x->limb[3] == 0;
}

void blsag_reduce(U256 *out, const U256 *x) {
  U256 r;
  modulus_word(&r, &BN254_FR);
  *out = *x;
  while (u256_cmp(out, &r) >= 0)
    u256_sub(out, out, &r);
}

void blsag_key(U256 *out, uint64_t i) {
  U256 w;
  u256_from_u64(&w, i);
  hash2(&w, &BLSAG_TAG, &w);
  blsag_reduce(out, &w);
}

int blsag_hash_point(G1 *out, uint32_t *counter, const G1 *p) {
  U256 px;
  U256 py;
  U256 h;
  U256 k;
  U256 xk;
  U256 e;
  U256 four;
  Fe x;
  Fe x2;
  Fe cube;
  Fe rhs;
  Fe three;
  Fe s;
  Fe s2;
  Fe neg;
  uint32_t i;
  g1_to_words(&px, &py, p);
  hash2(&h, &px, &py);
  /* The square root exponent (p + 1) / 4: p is 3 mod 4. */
  modulus_word(&e, &BN254_FP);
  u256_add(&e, &e, &ONE);
  u256_from_u64(&four, 4u);
  u256_div(&e, &e, &four);
  fe_from_u64(&three, 3u);
  for (i = 0; i < BLSAG_HP_TRIES; i++) {
    u256_from_u64(&k, i);
    hash2(&xk, &h, &k);
    if (!fe_from_u256(&x, &xk, &BN254_FP))
      continue;
    fe_mul(&x2, &x, &x, &BN254_FP);
    fe_mul(&cube, &x2, &x, &BN254_FP);
    fe_add(&rhs, &cube, &three, &BN254_FP);
    fe_pow(&s, &rhs, &e, &BN254_FP);
    fe_mul(&s2, &s, &s, &BN254_FP);
    if (!fe_eq(&s2, &rhs))
      continue;
    fe_neg(&neg, &s, &BN254_FP);
    out->x = x;
    out->y = s;
    if (fe_lt(&neg, &s))
      out->y = neg;
    *counter = i;
    return 1;
  }
  return 0;
}

void blsag_digest(U256 *out, const G1 *ring, uint32_t n) {
  U256 x;
  U256 y;
  U256 t;
  uint32_t i;
  g1_to_words(&x, &y, &ring[0]);
  hash2(out, &x, &y);
  for (i = 1; i < n; i++) {
    g1_to_words(&x, &y, &ring[i]);
    hash2(&t, out, &x);
    hash2(out, &t, &y);
  }
}

/* The challenge: fold hash left over tag, D, poll id, choice, I.x, I.y, L.x,
   L.y, R.x, R.y. */
static void challenge(U256 *out, const U256 *d, const U256 *poll, const U256 *choice, const G1 *img, const G1 *l, const G1 *r) {
  U256 w[6];
  uint32_t i;
  g1_to_words(&w[0], &w[1], img);
  g1_to_words(&w[2], &w[3], l);
  g1_to_words(&w[4], &w[5], r);
  hash2(out, &BLSAG_TAG, d);
  hash2(out, out, poll);
  hash2(out, out, choice);
  for (i = 0; i < 6u; i++)
    hash2(out, out, &w[i]);
}

/* Bind deterministic scalars to the entire signing context. Role 0 is the
   private alpha, role 1 a public decoy response. Both the ring digest and
   the role prevent nonce reuse when a signer changes rings or positions. */
static void nonce(U256 *out, const U256 *x, const U256 *d, const U256 *poll, const U256 *choice, uint32_t i, uint32_t role) {
  U256 t;
  U256 k;
  hash2(&t, &BLSAG_TAG, x);
  hash2(&t, &t, d);
  hash2(&t, &t, poll);
  hash2(&t, &t, choice);
  u256_from_u64(&k, i);
  hash2(&t, &t, &k);
  u256_from_u64(&k, role);
  hash2(&t, &t, &k);
  blsag_reduce(out, &t);
}

/* One link of the chain: L = s G + c P, R = s H + c I, and NEXT the challenge
   of them. NEXT can be C. */
static void link(U256 *next, const U256 *d, const U256 *poll, const U256 *choice, const G1 *img, const G1 *p, const G1 *h, const U256 *s, const U256 *c) {
  G1 l;
  G1 r;
  G1 t;
  g1_mul(&l, &GEN, s);
  g1_mul(&t, p, c);
  g1_add(&l, &l, &t);
  g1_mul(&r, h, s);
  g1_mul(&t, img, c);
  g1_add(&r, &r, &t);
  challenge(next, d, poll, choice, img, &l, &r);
}

int blsag_sign(U256 *sig, const G1 *ring, uint32_t n, const U256 *x, const U256 *poll, const U256 *choice) {
  G1 h[BLSAG_RING_MAX];
  G1 pj;
  G1 img;
  G1 l;
  G1 r;
  U256 c[BLSAG_RING_MAX];
  U256 d;
  U256 alpha;
  U256 cj;
  Fe fx;
  Fe fc;
  Fe fa;
  Fe prod;
  Fe sj;
  uint32_t counter = 0;
  uint32_t i;
  uint32_t j;
  uint32_t k;
  if (n == 0 || n > BLSAG_RING_MAX || word_zero(x) || !fe_from_u256(&fx, x, &BN254_FR))
    return 0;
  g1_mul(&pj, &GEN, x);
  j = 0;
  while (j < n && !(fe_eq(&pj.x, &ring[j].x) && fe_eq(&pj.y, &ring[j].y)))
    j++;
  if (j == n)
    return 0;
  for (i = 0; i < n; i++) {
    if (!blsag_hash_point(&h[i], &counter, &ring[i]))
      return 0;
  }
  blsag_digest(&d, ring, n);
  g1_mul(&img, &h[j], x);
  nonce(&alpha, x, &d, poll, choice, j, 0u);
  g1_mul(&l, &GEN, &alpha);
  g1_mul(&r, &h[j], &alpha);
  challenge(&c[(j + 1u) % n], &d, poll, choice, &img, &l, &r);
  for (k = 1; k < n; k++) {
    i = (j + k) % n;
    nonce(&sig[1u + i], x, &d, poll, choice, i, 1u);
    link(&c[(i + 1u) % n], &d, poll, choice, &img, &ring[i], &h[i], &sig[1u + i], &c[i]);
  }
  /* s_j = alpha minus c_j x mod r, with c_j reduced first. */
  blsag_reduce(&cj, &c[j]);
  fe_from_u256(&fc, &cj, &BN254_FR);
  fe_from_u256(&fa, &alpha, &BN254_FR);
  fe_mul(&prod, &fc, &fx, &BN254_FR);
  fe_sub(&sj, &fa, &prod, &BN254_FR);
  fe_to_u256(&sig[1u + j], &sj);
  sig[0] = c[0];
  g1_to_words(&sig[1u + n], &sig[2u + n], &img);
  return 1;
}

BlsagVerdict blsag_verify(const G1 *ring, uint32_t n, const U256 *poll, const U256 *choice, const U256 *sig) {
  G1 img;
  G1 h;
  U256 r;
  U256 d;
  U256 c;
  uint32_t counter = 0;
  uint32_t i;
  if (n == 0 || n > BLSAG_RING_MAX)
    return BLSAG_TRAP;
  if (word_zero(&sig[1u + n]) && word_zero(&sig[2u + n]))
    return BLSAG_REJECT;
  if (!g1_from_words(&img, &sig[1u + n], &sig[2u + n]))
    return BLSAG_TRAP;
  modulus_word(&r, &BN254_FR);
  for (i = 0; i < n; i++) {
    if (u256_cmp(&sig[1u + i], &r) >= 0)
      return BLSAG_REJECT;
  }
  blsag_digest(&d, ring, n);
  c = sig[0];
  for (i = 0; i < n; i++) {
    if (!blsag_hash_point(&h, &counter, &ring[i]))
      return BLSAG_TRAP;
    link(&c, &d, poll, choice, &img, &ring[i], &h, &sig[1u + i], &c);
  }
  return u256_cmp(&c, &sig[0]) == 0 ? BLSAG_ACCEPT : BLSAG_REJECT;
}
