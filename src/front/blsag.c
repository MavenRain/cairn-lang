/* bLSAG ring signatures over secp256k1 in the host (docs/PLAN.md section 8,
   builder call S8, slice B2a). See front/blsag.h.

   Builder defaults, each named in the commit message: the tag word, H_p by
   try and increment from counter 0, the ring digest fold, the challenge fold
   (tag, D, poll id, choice, I.x, I.y, L.x, L.y, R.x, R.y) and the nonce fold
   (tag, x, D, poll id, choice, i, role) mod n. */
#include "front/blsag.h"

#include "sha256.h"

const U256 BLSAG_TAG = {{0x636169726eu, 0u, 0u, 0u}};

/* K G. */
static void gen_mul(GroupPoint *out, const U256 *k) {
  GroupPoint g;
  group_gen(&g);
  group_mul(out, &g, k);
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
  modulus_word(&r, &GROUP_N);
  *out = *x;
  while (u256_cmp(out, &r) >= 0)
    u256_sub(out, out, &r);
}

void blsag_key(U256 *out, uint64_t i) {
  U256 w;
  u256_from_u64(&w, i);
  sha256_words(&w, &BLSAG_TAG, &w);
  blsag_reduce(out, &w);
}

int blsag_hash_point(GroupPoint *out, uint32_t *counter, const GroupPoint *p) {
  U256 px;
  U256 py;
  U256 h;
  U256 k;
  U256 xk;
  uint32_t i;
  group_to_words(&px, &py, p);
  sha256_words(&h, &px, &py);
  for (i = 0; i < BLSAG_HP_TRIES; i++) {
    u256_from_u64(&k, i);
    sha256_words(&xk, &h, &k);
    if (group_lift_x(out, &xk)) {
      *counter = i;
      return 1;
    }
  }
  return 0;
}

void blsag_digest(U256 *out, const GroupPoint *ring, uint32_t n) {
  U256 x;
  U256 y;
  U256 t;
  uint32_t i;
  group_to_words(&x, &y, &ring[0]);
  sha256_words(out, &x, &y);
  for (i = 1; i < n; i++) {
    group_to_words(&x, &y, &ring[i]);
    sha256_words(&t, out, &x);
    sha256_words(out, &t, &y);
  }
}

/* The challenge: fold hash left over tag, D, poll id, choice, I.x, I.y, L.x,
   L.y, R.x, R.y. */
static void challenge(U256 *out, const U256 *d, const U256 *poll, const U256 *choice, const GroupPoint *img, const GroupPoint *l, const GroupPoint *r) {
  U256 w[6];
  uint32_t i;
  group_to_words(&w[0], &w[1], img);
  group_to_words(&w[2], &w[3], l);
  group_to_words(&w[4], &w[5], r);
  sha256_words(out, &BLSAG_TAG, d);
  sha256_words(out, out, poll);
  sha256_words(out, out, choice);
  for (i = 0; i < 6u; i++)
    sha256_words(out, out, &w[i]);
}

/* Bind deterministic scalars to the entire signing context. Role 0 is the
   private alpha, role 1 a public decoy response. Both the ring digest and
   the role prevent nonce reuse when a signer changes rings or positions. */
static void nonce(U256 *out, const U256 *x, const U256 *d, const U256 *poll, const U256 *choice, uint32_t i, uint32_t role) {
  U256 t;
  U256 k;
  sha256_words(&t, &BLSAG_TAG, x);
  sha256_words(&t, &t, d);
  sha256_words(&t, &t, poll);
  sha256_words(&t, &t, choice);
  u256_from_u64(&k, i);
  sha256_words(&t, &t, &k);
  u256_from_u64(&k, role);
  sha256_words(&t, &t, &k);
  blsag_reduce(out, &t);
}

/* One link of the chain: L = s G + c P, R = s H + c I, and NEXT the challenge
   of them. NEXT can be C. */
static void link(U256 *next, const U256 *d, const U256 *poll, const U256 *choice, const GroupPoint *img, const GroupPoint *p, const GroupPoint *h, const U256 *s, const U256 *c) {
  GroupPoint l;
  GroupPoint r;
  GroupPoint t;
  gen_mul(&l, s);
  group_mul(&t, p, c);
  group_add(&l, &l, &t);
  group_mul(&r, h, s);
  group_mul(&t, img, c);
  group_add(&r, &r, &t);
  challenge(next, d, poll, choice, img, &l, &r);
}

int blsag_sign(U256 *sig, const GroupPoint *ring, uint32_t n, const U256 *x, const U256 *poll, const U256 *choice) {
  GroupPoint h[BLSAG_RING_MAX];
  GroupPoint pj;
  GroupPoint img;
  GroupPoint l;
  GroupPoint r;
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
  if (n == 0 || n > BLSAG_RING_MAX || word_zero(x) || !fe_from_u256(&fx, x, &GROUP_N))
    return 0;
  gen_mul(&pj, x);
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
  group_mul(&img, &h[j], x);
  nonce(&alpha, x, &d, poll, choice, j, 0u);
  gen_mul(&l, &alpha);
  group_mul(&r, &h[j], &alpha);
  challenge(&c[(j + 1u) % n], &d, poll, choice, &img, &l, &r);
  for (k = 1; k < n; k++) {
    i = (j + k) % n;
    nonce(&sig[1u + i], x, &d, poll, choice, i, 1u);
    link(&c[(i + 1u) % n], &d, poll, choice, &img, &ring[i], &h[i], &sig[1u + i], &c[i]);
  }
  /* s_j = alpha minus c_j x mod n, with c_j reduced first. */
  blsag_reduce(&cj, &c[j]);
  fe_from_u256(&fc, &cj, &GROUP_N);
  fe_from_u256(&fa, &alpha, &GROUP_N);
  fe_mul(&prod, &fc, &fx, &GROUP_N);
  fe_sub(&sj, &fa, &prod, &GROUP_N);
  fe_to_u256(&sig[1u + j], &sj);
  sig[0] = c[0];
  group_to_words(&sig[1u + n], &sig[2u + n], &img);
  return 1;
}

BlsagVerdict blsag_verify(const GroupPoint *ring, uint32_t n, const U256 *poll, const U256 *choice, const U256 *sig) {
  GroupPoint img;
  GroupPoint h;
  U256 r;
  U256 d;
  U256 c;
  uint32_t counter = 0;
  uint32_t i;
  if (n == 0 || n > BLSAG_RING_MAX)
    return BLSAG_TRAP;
  if (word_zero(&sig[1u + n]) && word_zero(&sig[2u + n]))
    return BLSAG_REJECT;
  if (!group_from_words(&img, &sig[1u + n], &sig[2u + n]))
    return BLSAG_TRAP;
  modulus_word(&r, &GROUP_N);
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
