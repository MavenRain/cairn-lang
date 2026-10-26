/* Exponential ElGamal over BN254 G1 in the host (slice S5 b5). See
   front/elgamal.h.

   The baby step table holds j G for each j below n = 2^ceil(bits / 2), with
   open addressing over 2 n slots and a key from the low limbs of x and y. A
   key hit is checked by a full m G, so a clash of keys costs time but never
   gives a wrong m. */
#include <stdlib.h>
#include <string.h>

#include "front/elgamal.h"

#include "front/blsag.h"
#include "keccak.h"

const U256 ELG_TAG = {{0x656c67616d616cu, 0u, 0u, 0u}};

/* The generator (1, 2). */
static const G1 GEN = {{{1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}}, {{2u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}}};

/* A slot of the table: the key and j + 1, or a step of 0 when empty. */
typedef struct {
  uint32_t key;
  uint32_t step;
} Slot;

void elg_generator(G1 *out) {
  *out = GEN;
}

void g1_neg(G1 *out, const G1 *p) {
  out->x = p->x;
  fe_neg(&out->y, &p->y, &BN254_FP);
}

int g1_eq(const G1 *a, const G1 *b) {
  return fe_eq(&a->x, &b->x) && fe_eq(&a->y, &b->y);
}

/* The 32 big endian bytes of X at OUT (limb 3 first), as word_bytes in
   front/blsag.c. */
static void word_bytes(const U256 *x, unsigned char *out) {
  uint32_t i;
  for (i = 0; i < 32u; i++)
    out[i] = (unsigned char)(x->limb[3u - i / 8u] >> (56u - 8u * (i % 8u)));
}

void elg_nonce(U256 *out, uint64_t i) {
  unsigned char data[64];
  unsigned char digest[32];
  U256 w;
  uint32_t k;
  uint32_t j;
  u256_from_u64(&w, i);
  word_bytes(&ELG_TAG, data);
  word_bytes(&w, data + 32);
  keccak256(data, sizeof data, digest);
  for (k = 0; k < 4u; k++) {
    w.limb[3u - k] = 0;
    for (j = 0; j < 8u; j++)
      w.limb[3u - k] = (w.limb[3u - k] << 8) | digest[8u * k + j];
  }
  blsag_reduce(out, &w);
}

void elg_public(G1 *out, const U256 *sk) {
  g1_mul(out, &GEN, sk);
}

void elg_encrypt(ElgCipher *out, const G1 *pk, const U256 *m, const U256 *r) {
  ElgCipher t;
  G1 shared;
  g1_mul(&t.c1, &GEN, r);
  g1_mul(&t.c2, &GEN, m);
  g1_mul(&shared, pk, r);
  g1_add(&t.c2, &t.c2, &shared);
  *out = t;
}

void elg_add(ElgCipher *out, const ElgCipher *a, const ElgCipher *b) {
  g1_add(&out->c1, &a->c1, &b->c1);
  g1_add(&out->c2, &a->c2, &b->c2);
}

void elg_scale(ElgCipher *out, const ElgCipher *ct, const U256 *k) {
  g1_mul(&out->c1, &ct->c1, k);
  g1_mul(&out->c2, &ct->c2, k);
}

static uint32_t point_key(const G1 *p) {
  return p->x.w[0] ^ (p->y.w[0] * 0x9e3779b9u);
}

/* The m = BASE + j with m G = TARGET, for each slot j with the key of Q.
   Returns 1 when found. */
static int probe(uint64_t *m, const Slot *table, uint32_t mask, const G1 *q, const G1 *target, uint64_t base) {
  uint32_t key = point_key(q);
  uint32_t s = key & mask;
  U256 w;
  G1 check;
  while (table[s].step != 0) {
    if (table[s].key == key) {
      u256_from_u64(&w, base + table[s].step - 1u);
      g1_mul(&check, &GEN, &w);
      if (g1_eq(&check, target)) {
        *m = base + table[s].step - 1u;
        return 1;
      }
    }
    s = (s + 1u) & mask;
  }
  return 0;
}

ElgResult elg_log(uint64_t *m, const G1 *point, uint32_t bits) {
  uint32_t n;
  uint32_t giants;
  uint32_t mask;
  uint32_t i;
  Slot *table;
  G1 baby;
  G1 stride;
  G1 q;
  if (bits > ELG_BITS_MAX || !g1_on_curve(point))
    return ELG_REFUSED;
  n = 1u << ((bits + 1u) / 2u);
  giants = 1u << (bits / 2u);
  mask = 2u * n - 1u;
  table = calloc(2u * (size_t)n, sizeof *table);
  if (table == NULL)
    return ELG_NO_MEMORY;
  memset(&baby, 0, sizeof baby);
  for (i = 0; i < n; i++) {
    uint32_t key = point_key(&baby);
    uint32_t s = key & mask;
    while (table[s].step != 0)
      s = (s + 1u) & mask;
    table[s].key = key;
    table[s].step = i + 1u;
    g1_add(&baby, &baby, &GEN);
  }
  /* BABY is n G here, so each giant step adds minus n G. */
  g1_neg(&stride, &baby);
  q = *point;
  for (i = 0; i < giants; i++) {
    if (probe(m, table, mask, &q, point, (uint64_t)i * n)) {
      free(table);
      return ELG_FOUND;
    }
    g1_add(&q, &q, &stride);
  }
  free(table);
  return ELG_NOT_FOUND;
}

ElgResult elg_decrypt(uint64_t *m, const U256 *sk, const ElgCipher *ct, uint32_t bits) {
  G1 point;
  if (bits > ELG_BITS_MAX || !g1_on_curve(&ct->c1) || !g1_on_curve(&ct->c2))
    return ELG_REFUSED;
  g1_mul(&point, &ct->c1, sk);
  g1_neg(&point, &point);
  g1_add(&point, &ct->c2, &point);
  return elg_log(m, &point, bits);
}
