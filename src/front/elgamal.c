/* Exponential ElGamal over secp256k1 in the host (slice S5 b5). See
   front/elgamal.h.

   The baby step table holds j G for each j below n = 2^ceil(bits / 2), with
   open addressing over 2 n slots and a key from the low limbs of x and y. A
   key hit is checked by a full m G, so a clash of keys costs time but never
   gives a wrong m. */
#include <stdlib.h>
#include <string.h>

#include "front/elgamal.h"

#include "front/blsag.h"
#include "sha256.h"

const U256 ELG_TAG = {{0x656c67616d616cu, 0u, 0u, 0u}};

/* A slot of the table: the key and j + 1, or a step of 0 when empty. */
typedef struct {
  uint32_t key;
  uint32_t step;
} Slot;

void elg_generator(GroupPoint *out) {
  group_gen(out);
}

static void gen_mul(GroupPoint *out, const U256 *k) {
  GroupPoint g;
  group_gen(&g);
  group_mul(out, &g, k);
}

void elg_nonce(U256 *out, uint64_t i) {
  U256 w;
  u256_from_u64(&w, i);
  sha256_words(&w, &ELG_TAG, &w);
  blsag_reduce(out, &w);
}

void elg_public(GroupPoint *out, const U256 *sk) {
  gen_mul(out, sk);
}

void elg_encrypt(ElgCipher *out, const GroupPoint *pk, const U256 *m, const U256 *r) {
  ElgCipher t;
  GroupPoint shared;
  gen_mul(&t.c1, r);
  gen_mul(&t.c2, m);
  group_mul(&shared, pk, r);
  group_add(&t.c2, &t.c2, &shared);
  *out = t;
}

void elg_add(ElgCipher *out, const ElgCipher *a, const ElgCipher *b) {
  group_add(&out->c1, &a->c1, &b->c1);
  group_add(&out->c2, &a->c2, &b->c2);
}

void elg_scale(ElgCipher *out, const ElgCipher *ct, const U256 *k) {
  group_mul(&out->c1, &ct->c1, k);
  group_mul(&out->c2, &ct->c2, k);
}

static uint32_t point_key(const GroupPoint *p) {
  return p->x.w[0] ^ (p->y.w[0] * 0x9e3779b9u);
}

/* The m = BASE + j with m G = TARGET, for each slot j with the key of Q.
   Returns 1 when found. */
static int probe(uint64_t *m, const Slot *table, uint32_t mask, const GroupPoint *q, const GroupPoint *target, uint64_t base) {
  uint32_t key = point_key(q);
  uint32_t s = key & mask;
  U256 w;
  GroupPoint check;
  while (table[s].step != 0) {
    if (table[s].key == key) {
      u256_from_u64(&w, base + table[s].step - 1u);
      gen_mul(&check, &w);
      if (group_eq(&check, target)) {
        *m = base + table[s].step - 1u;
        return 1;
      }
    }
    s = (s + 1u) & mask;
  }
  return 0;
}

ElgResult elg_log(uint64_t *m, const GroupPoint *point, uint32_t bits) {
  uint32_t n;
  uint32_t giants;
  uint32_t mask;
  uint32_t i;
  Slot *table;
  GroupPoint baby;
  GroupPoint stride;
  GroupPoint q;
  GroupPoint gen;
  if (bits > ELG_BITS_MAX || !group_on_curve(point))
    return ELG_REFUSED;
  n = 1u << ((bits + 1u) / 2u);
  giants = 1u << (bits / 2u);
  mask = 2u * n - 1u;
  table = calloc(2u * (size_t)n, sizeof *table);
  if (table == NULL)
    return ELG_NO_MEMORY;
  memset(&baby, 0, sizeof baby);
  group_gen(&gen);
  for (i = 0; i < n; i++) {
    uint32_t key = point_key(&baby);
    uint32_t s = key & mask;
    while (table[s].step != 0)
      s = (s + 1u) & mask;
    table[s].key = key;
    table[s].step = i + 1u;
    group_add(&baby, &baby, &gen);
  }
  /* BABY is n G here, so each giant step adds minus n G. */
  group_neg(&stride, &baby);
  q = *point;
  for (i = 0; i < giants; i++) {
    if (probe(m, table, mask, &q, point, (uint64_t)i * n)) {
      free(table);
      return ELG_FOUND;
    }
    group_add(&q, &q, &stride);
  }
  free(table);
  return ELG_NOT_FOUND;
}

ElgResult elg_decrypt(uint64_t *m, const U256 *sk, const ElgCipher *ct, uint32_t bits) {
  GroupPoint point;
  if (bits > ELG_BITS_MAX || !group_on_curve(&ct->c1) || !group_on_curve(&ct->c2))
    return ELG_REFUSED;
  group_mul(&point, &ct->c1, sk);
  group_neg(&point, &point);
  group_add(&point, &ct->c2, &point);
  return elg_log(m, &point, bits);
}
