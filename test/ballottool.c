/* Host ballot regressions (slice S9 b3): the encrypted ballots of Part D.
   Voters 0, 1 and 2 of the ring zkKey 0 .. 2 cast one-hot ballots (k 2: yes
   is slot 0, no is slot 1) under the election key of thresholdtool (n 3, t
   2). Each proof carries ctx = hash(hash(hash(ballotTag, I.x), I.y), poll)
   for the key image I of the voter (O34), and the ring signs the digest of
   the slot ciphertexts. With the argument `words` it prints the chain
   constants of examples/ballot.lang, one `name 0x<word>` per line. With
   `dao` (slice S10 b2) it prints the constants of examples/daokit.lang: the
   full words of the 3 ballots, the slot 0 opening of the tally, the fill
   intent with its 3 amount openings and the owner signatures. */
#include <stdio.h>
#include <string.h>

#include "front/blsag.h"
#include "front/elgamal.h"
#include "front/secp256k1.h"
#include "front/sigma.h"
#include "front/threshold.h"
#include "keccak.h"

#define N 3u
#define T 2u
#define K 2u
#define BITS 16u
/* "ballot" and "digest" as big endian words. */
#define BALLOT_TAG 0x62616c6c6f74ull
#define DIGEST_TAG 0x646967657374ull
/* The known answer of ballotCtx for voter 0: a recomputation would follow a
   change of the tag, this word does not. */
#define CTX0 "0x913b5ec82461c5d47cc06a914cca0871ec391541fdbf848fd39e279a7449194f"
/* The fill intent of examples/daokit.lang (S10 b2): the sell3 of intent.lang
   (amtIn 3, startOut 8, minOut 4, decoyIdx 3 1 4 1) under the election key,
   token 0x..ee in and 0x..ef out (the mock tokens of test/dao.sh), start
   100, the owner key 0x2b of intentsig.lang. Nonce 7 is the fill, nonce 8
   the same intent for the cancel rows. "intent" as a big endian word. */
#define INTENT_TAG 0x696e74656e74ull
#define IN_TOK 0xeeull
#define OUT_TOK 0xefull
#define START 100u
#define OWNER_KEY 0x2bull
/* The known answers of the fill intent. */
#define DAO_DIGEST "0x7850631940c1e4e007fdb3bc19e780e2694ec7f4429b0f6a9c732b17375d6c38"
#define DAO_HASH7 "0x151ff2cfab4ff5f63c2c09c48a83ed78c611e6f1080c07d9b39aabce83fe89a4"
#define DAO_HASH8 "0x4caf83148da049350672dc29cc07629e913518e7917965ed5a8ca467f57b2689"
#define DAO_OWNER "0x000000000000000000000000eb3025e7ac2764040384316b33476e048961a71f"

/* One cast: the bLSAG words c_1, s_1 .. s_3, I.x, I.y over the digest, the
   ctx of I, the slots and the sum proof. */
typedef struct {
  U256 sig[N + 3u];
  U256 ctx;
  U256 digest;
  ElgCipher cts[K];
  SigOr slots[K];
  SigDleq sum;
} Ballot;

/* The fill intent: the amount ciphertexts (amtIn, startOut, minOut), the
   decoy words, cipherDigest, the owner address, and for nonces 7 and 8 the
   intentHash, the replayKey and the low-s signature (v, r, w). */
typedef struct {
  ElgCipher amt[3];
  U256 decoy[4];
  U256 digest;
  U256 owner;
  U256 hash[2];
  U256 key[2];
  U256 v[2];
  U256 r[2];
  U256 w[2];
} Fill;

static void hex(char *out, size_t size, const U256 *w) {
  snprintf(out, size, "0x%016llx%016llx%016llx%016llx", (unsigned long long)w->limb[3], (unsigned long long)w->limb[2],
           (unsigned long long)w->limb[1], (unsigned long long)w->limb[0]);
}

static void word(const char *prefix, const char *name, const U256 *w) {
  char text[67];
  hex(text, sizeof text, w);
  printf("%s%s %s\n", prefix, name, text);
}

static void point(const char *prefix, const char *x, const char *y, const G1 *p) {
  U256 a;
  U256 b;
  g1_to_words(&a, &b, p);
  word(prefix, x, &a);
  word(prefix, y, &b);
}

/* ballotCtx: the hash chain from ballotTag over I.x, I.y and the poll. */
static void ballot_ctx(U256 *out, const U256 *ix, const U256 *iy, const U256 *poll) {
  u256_from_u64(out, BALLOT_TAG);
  thr_hash(out, out, ix);
  thr_hash(out, out, iy);
  thr_hash(out, out, poll);
}

/* ballotDigest: the hash chain from digestTag over C1.x, C1.y, C2.x, C2.y of
   slot 0, then of slot 1. */
static void ballot_digest(U256 *out, const ElgCipher *cts) {
  U256 w[4];
  uint32_t j;
  uint32_t i;
  u256_from_u64(out, DIGEST_TAG);
  for (j = 0; j < K; j++) {
    g1_to_words(&w[0], &w[1], &cts[j].c1);
    g1_to_words(&w[2], &w[3], &cts[j].c2);
    for (i = 0; i < 4u; i++)
      thr_hash(out, out, &w[i]);
  }
}

/* The ballot of VOTER for CHOICE. The key image does not depend on the
   message, so a first signature gives I, and so ctx, before the slots
   exist. Fixed entropy for reproducible vectors only, never a live ballot.
   1, or 0 when a step refuses. */
static int cast(Ballot *b, const G1 *ring, const G1 *pk, const U256 *poll, uint32_t voter, uint32_t choice) {
  U256 x;
  U256 seed;
  blsag_key(&x, voter);
  blsag_key(&seed, 40u + voter);
  if (!blsag_sign(b->sig, ring, N, &x, poll, poll))
    return 0;
  ballot_ctx(&b->ctx, &b->sig[N + 1u], &b->sig[N + 2u], poll);
  if (sig_onehot_prove(b->cts, b->slots, &b->sum, pk, K, choice, &seed, &b->ctx) != SIG_OK)
    return 0;
  ballot_digest(&b->digest, b->cts);
  return blsag_sign(b->sig, ring, N, &x, poll, &b->digest);
}

/* The sum of slot J over the three ballots. */
static void slot_sum(ElgCipher *out, const Ballot *b, uint32_t j) {
  elg_add(out, &b[0].cts[j], &b[1].cts[j]);
  elg_add(out, out, &b[2].cts[j]);
}

/* The parts of the quorum {1, 2} for CT. */
static void quorum(ThrPart *parts, const ThrShare *shares, const ElgCipher *ct) {
  thr_partial(&parts[0], &shares[0], ct);
  thr_partial(&parts[1], &shares[1], ct);
}

static int fail(const char *name) {
  fprintf(stderr, "FAIL ballot %s\n", name);
  return 0;
}

static void slot_words(const char *prefix, const ElgCipher *ct, const SigOr *s) {
  point(prefix, "c1x", "c1y", &ct->c1);
  point(prefix, "c2x", "c2y", &ct->c2);
  point(prefix, "a0x", "a0y", &s->a0);
  point(prefix, "b0x", "b0y", &s->b0);
  point(prefix, "a1x", "a1y", &s->a1);
  point(prefix, "b1x", "b1y", &s->b1);
  word(prefix, "e0", &s->e0);
  word(prefix, "e1", &s->e1);
  word(prefix, "z0", &s->z0);
  word(prefix, "z1", &s->z1);
}

static void cipher_words(const char *prefix, const ElgCipher *ct) {
  point(prefix, "c1x", "c1y", &ct->c1);
  point(prefix, "c2x", "c2y", &ct->c2);
}

static void part_words(const char *prefix, const char *d, const char *a, const char *b, const char *z, const ThrPart *p) {
  char x[8];
  char y[8];
  snprintf(x, sizeof x, "%sx", d);
  snprintf(y, sizeof y, "%sy", d);
  point(prefix, x, y, &p->d);
  snprintf(x, sizeof x, "%sx", a);
  snprintf(y, sizeof y, "%sy", a);
  point(prefix, x, y, &p->a);
  snprintf(x, sizeof x, "%sx", b);
  snprintf(y, sizeof y, "%sy", b);
  point(prefix, x, y, &p->b);
  word(prefix, z, &p->z);
}

/* The parts of the quorum {1, 2} for CT and K = l1 D1 + l2 D2. */
static void share_words(const char *prefix, const ThrShare *shares, const ElgCipher *ct, const U256 *l1, const U256 *l2) {
  ThrPart parts[2];
  G1 q1;
  G1 q2;
  quorum(parts, shares, ct);
  part_words(prefix, "d1", "a1", "b1", "z1", &parts[0]);
  part_words(prefix, "d2", "a2", "b2", "z2", &parts[1]);
  g1_mul(&q1, &parts[0].d, l1);
  g1_mul(&q2, &parts[1].d, l2);
  g1_add(&q1, &q1, &q2);
  point(prefix, "kx", "ky", &q1);
}

/* The opening of slot sum CT by the quorum {1, 2}: the sum, the two parts
   and K. */
static void open_words(const char *prefix, const ThrShare *shares, const ElgCipher *ct, const U256 *l1, const U256 *l2) {
  cipher_words(prefix, ct);
  share_words(prefix, shares, ct, l1, l2);
}

/* The Lagrange constants l1, l2 of the quorum {1, 2}. */
static void lagrange(U256 *l1, U256 *l2, const ThrShare *shares, const ElgCipher *ct) {
  ThrPart parts[2];
  quorum(parts, shares, ct);
  thr_lagrange(l1, parts, 2u, 0u);
  thr_lagrange(l2, parts, 2u, 1u);
}

/* The opening on the chain (openSum of ballot.lang): both parts of the
   quorum {1, 2} verify, K = l1 D1 + l2 D2 and C2 - K = m G. 1 when M is
   found below 2^BITS. */
static int chain_open(uint64_t *m, const ThrShare *shares, const ElgCipher *ct, const U256 *l1, const U256 *l2) {
  ThrPart parts[2];
  G1 k;
  G1 q;
  G1 neg;
  quorum(parts, shares, ct);
  if (!thr_verify(&parts[0], &shares[0].key, ct) || !thr_verify(&parts[1], &shares[1].key, ct))
    return 0;
  g1_mul(&k, &parts[0].d, l1);
  g1_mul(&q, &parts[1].d, l2);
  g1_add(&k, &k, &q);
  g1_neg(&neg, &k);
  g1_add(&q, &ct->c2, &neg);
  return elg_log(m, &q, BITS) == ELG_FOUND;
}

/* The word of HEX (64 digits or fewer, no prefix; the word of secptool.c). */
static U256 from_hex(const char *hex) {
  U256 x;
  u256_from_u64(&x, 0);
  for (; *hex != '\0'; hex++)
    u256_scale_add(&x, 16u, (uint32_t)(*hex <= '9' ? *hex - '0' : *hex - 'a' + 10));
  return x;
}

/* The address of the point Q: the low 160 bits of keccak256(Q.x, Q.y) (the
   address_of of secptool.c). */
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

/* The secp256k1 generator. */
static void secp_generator(G1 *g) {
  U256 gx = from_hex("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
  U256 gy = from_hex("483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");
  fe_from_u256(&g->x, &gx, &SECP_P);
  fe_from_u256(&g->y, &gy, &SECP_P);
}

/* The address of the key D. */
static void secp_address(U256 *addr, const U256 *d) {
  G1 g;
  G1 q;
  secp_generator(&g);
  ec0_mul(&q, &g, d, &SECP_P);
  address_of(addr, &q);
}

/* The low-s signature (V, R, S) of the hash H by the key D with the nonce K
   (the sign of secptool.c, no output). 0 when D, K or H is n or more, or
   k G has an x of n or more. */
static int secp_sign(U256 *v, U256 *rw, U256 *sw, const U256 *d, const U256 *k, const U256 *h) {
  U256 n = from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  U256 yw;
  U256 high;
  Fe fd;
  Fe fk;
  Fe fr;
  Fe fe;
  Fe fs;
  G1 g;
  G1 rp;
  uint64_t parity;
  if (!fe_from_u256(&fd, d, &SECP_N) || !fe_from_u256(&fk, k, &SECP_N) || !fe_from_u256(&fe, h, &SECP_N))
    return 0;
  secp_generator(&g);
  ec0_mul(&rp, &g, k, &SECP_P);
  fe_to_u256(rw, &rp.x);
  if (!fe_from_u256(&fr, rw, &SECP_N))
    return 0;
  fe_to_u256(&yw, &rp.y);
  parity = yw.limb[0] & 1u;
  /* s = k^-1 (h + r d) mod n; a high s flips to n - s and the other v. */
  fe_mul(&fs, &fr, &fd, &SECP_N);
  fe_add(&fs, &fs, &fe, &SECP_N);
  fe_inv(&fk, &fk, &SECP_N);
  fe_mul(&fs, &fs, &fk, &SECP_N);
  fe_to_u256(sw, &fs);
  u256_sub(&high, &n, sw);
  if (u256_cmp(sw, &high) > 0) {
    *sw = high;
    parity ^= 1u;
  }
  u256_from_u64(v, 27u + parity);
  return 1;
}

/* The fill intent under the election key PK. Fixed entropy for
   reproducible vectors only. 1, or 0 when a signature refuses. */
static int fill_intent(Fill *f, const G1 *pk) {
  static const uint64_t amounts[3] = {3u, 8u, 4u};
  static const uint64_t decoys[4] = {3u, 1u, 4u, 1u};
  U256 ct[16];
  U256 m;
  U256 r;
  U256 w;
  U256 d;
  U256 k;
  uint32_t i;
  for (i = 0; i < 3u; i++) {
    u256_from_u64(&m, amounts[i]);
    blsag_key(&r, 60u + i);
    elg_encrypt(&f->amt[i], pk, &m, &r);
    g1_to_words(&ct[4u * i], &ct[4u * i + 1u], &f->amt[i].c1);
    g1_to_words(&ct[4u * i + 2u], &ct[4u * i + 3u], &f->amt[i].c2);
  }
  for (i = 0; i < 4u; i++) {
    u256_from_u64(&f->decoy[i], decoys[i]);
    ct[12u + i] = f->decoy[i];
  }
  /* cipherDigest: the hash chain from a1x over the other 15 words. */
  f->digest = ct[0];
  for (i = 1; i < 16u; i++)
    thr_hash(&f->digest, &f->digest, &ct[i]);
  u256_from_u64(&d, OWNER_KEY);
  secp_address(&f->owner, &d);
  for (i = 0; i < 2u; i++) {
    /* intentHash: the hash chain from intentTag over inTok, outTok, the
       digest, owner, nonce and start; the nonce K of the signature is
       0x13 (as in intentsig.lang) and 0x14. */
    u256_from_u64(&f->hash[i], INTENT_TAG);
    u256_from_u64(&w, IN_TOK);
    thr_hash(&f->hash[i], &f->hash[i], &w);
    u256_from_u64(&w, OUT_TOK);
    thr_hash(&f->hash[i], &f->hash[i], &w);
    thr_hash(&f->hash[i], &f->hash[i], &f->digest);
    thr_hash(&f->hash[i], &f->hash[i], &f->owner);
    u256_from_u64(&w, 7u + i);
    thr_hash(&f->hash[i], &f->hash[i], &w);
    u256_from_u64(&w, START);
    thr_hash(&f->hash[i], &f->hash[i], &w);
    /* replayKey owner nonce. */
    u256_from_u64(&w, 7u + i);
    thr_hash(&f->key[i], &f->owner, &w);
    u256_from_u64(&k, 0x13u + i);
    if (!secp_sign(&f->v[i], &f->r[i], &f->w[i], &d, &k, &f->hash[i]))
      return 0;
  }
  return 1;
}

/* The full words of the ballot B of voter V. */
static void ballot_words(uint32_t v, const Ballot *b) {
  char p[8];
  snprintf(p, sizeof p, "b%u", v);
  word(p, "ix", &b->sig[N + 1u]);
  word(p, "iy", &b->sig[N + 2u]);
  word(p, "c1", &b->sig[0]);
  word(p, "s1", &b->sig[1]);
  word(p, "s2", &b->sig[2]);
  word(p, "s3", &b->sig[3]);
  snprintf(p, sizeof p, "b%uj0", v);
  slot_words(p, &b->cts[0], &b->slots[0]);
  snprintf(p, sizeof p, "b%uj1", v);
  slot_words(p, &b->cts[1], &b->slots[1]);
  snprintf(p, sizeof p, "b%usum", v);
  point(p, "ax", "ay", &b->sum.a);
  point(p, "bx", "by", &b->sum.b);
  word(p, "z", &b->sum.z);
}

/* `ballottool dao`: the constants of examples/daokit.lang. */
static int dao(const G1 *pk, const ThrShare *shares, const Ballot *b) {
  static const char *const amount[3] = {"fa", "fs", "fm"};
  static const char *const opening[3] = {"oa", "os", "om"};
  ElgCipher yes;
  Fill f;
  U256 l1;
  U256 l2;
  U256 w;
  uint32_t i;
  if (!fill_intent(&f, pk)) {
    fputs("FAIL dao: the intent signature refuses\n", stderr);
    return 1;
  }
  slot_sum(&yes, b, 0u);
  lagrange(&l1, &l2, shares, &yes);
  point("", "pkx", "pky", pk);
  word("", "l1", &l1);
  word("", "l2", &l2);
  for (i = 0; i < 3u; i++)
    ballot_words(i, &b[i]);
  open_words("ty", shares, &yes, &l1, &l2);
  u256_from_u64(&w, IN_TOK);
  word("", "inTok", &w);
  u256_from_u64(&w, OUT_TOK);
  word("", "outTok", &w);
  word("", "owner", &f.owner);
  u256_from_u64(&w, START);
  word("", "start", &w);
  for (i = 0; i < 3u; i++)
    cipher_words(amount[i], &f.amt[i]);
  word("", "d0", &f.decoy[0]);
  word("", "d1", &f.decoy[1]);
  word("", "d2", &f.decoy[2]);
  word("", "d3", &f.decoy[3]);
  word("", "digest", &f.digest);
  for (i = 0; i < 3u; i++)
    share_words(opening[i], shares, &f.amt[i], &l1, &l2);
  for (i = 0; i < 2u; i++) {
    char p[4];
    snprintf(p, sizeof p, "n%u", 7u + i);
    word(p, "hash", &f.hash[i]);
    word(p, "key", &f.key[i]);
    word(p, "v", &f.v[i]);
    word(p, "r", &f.r[i]);
    word(p, "w", &f.w[i]);
  }
  return 0;
}

/* The dao regressions: the chain opening of the tally gives yes 2 (no =
   casts - yes = 1), each amount opening gives its amount, the digest and
   the hashes are the known answers, each signature recovers the owner and
   the nonce 7 signature over the nonce 8 hash does not. */
static int dao_regressions(const G1 *pk, const ThrShare *shares, const Ballot *b) {
  static const uint64_t amounts[3] = {3u, 8u, 4u};
  ElgCipher yes;
  Fill f;
  U256 l1;
  U256 l2;
  U256 out;
  char text[67];
  uint64_t m = 0;
  uint32_t i;
  int ok = 1;
  if (!fill_intent(&f, pk))
    return !fail("dao intent signature");
  slot_sum(&yes, b, 0u);
  lagrange(&l1, &l2, shares, &yes);
  ok &= (chain_open(&m, shares, &yes, &l1, &l2) && m == 2u && 3u - m == 1u) || fail("dao tally opening");
  for (i = 0; i < 3u; i++)
    ok &= (chain_open(&m, shares, &f.amt[i], &l1, &l2) && m == amounts[i]) || fail("dao amount opening");
  hex(text, sizeof text, &f.digest);
  ok &= strcmp(text, DAO_DIGEST) == 0 || fail("dao cipherDigest");
  hex(text, sizeof text, &f.hash[0]);
  ok &= strcmp(text, DAO_HASH7) == 0 || fail("dao intentHash nonce 7");
  hex(text, sizeof text, &f.hash[1]);
  ok &= strcmp(text, DAO_HASH8) == 0 || fail("dao intentHash nonce 8");
  for (i = 0; i < 2u; i++) {
    secp_recover(&out, &f.hash[i], &f.v[i], &f.r[i], &f.w[i]);
    hex(text, sizeof text, &out);
    ok &= strcmp(text, DAO_OWNER) == 0 || fail("dao signature recovers the owner");
  }
  secp_recover(&out, &f.hash[1], &f.v[0], &f.r[0], &f.w[0]);
  hex(text, sizeof text, &out);
  ok &= strcmp(text, DAO_OWNER) != 0 || fail("dao signature over another hash");
  if (!ok)
    return 1;
  puts("dao regressions: passed");
  return 0;
}

static void words(const G1 *pk, const G1 *ring, const ThrShare *shares, const Ballot *b) {
  ElgCipher yes;
  ElgCipher no;
  ThrPart parts[2];
  U256 l1;
  U256 l2;
  slot_sum(&yes, b, 0u);
  slot_sum(&no, b, 1u);
  quorum(parts, shares, &yes);
  thr_lagrange(&l1, parts, 2u, 0u);
  thr_lagrange(&l2, parts, 2u, 1u);
  point("", "pkx", "pky", pk);
  point("", "p0x", "p0y", &ring[0]);
  point("", "p1x", "p1y", &ring[1]);
  point("", "p2x", "p2y", &ring[2]);
  point("", "k1x", "k1y", &shares[0].key);
  point("", "k2x", "k2y", &shares[1].key);
  word("", "b0ix", &b[0].sig[N + 1u]);
  word("", "b0iy", &b[0].sig[N + 2u]);
  word("", "b0ctx", &b[0].ctx);
  word("", "b0digest", &b[0].digest);
  word("", "b0c1", &b[0].sig[0]);
  word("", "b0s1", &b[0].sig[1]);
  word("", "b0s2", &b[0].sig[2]);
  word("", "b0s3", &b[0].sig[3]);
  slot_words("b0j0", &b[0].cts[0], &b[0].slots[0]);
  slot_words("b0j1", &b[0].cts[1], &b[0].slots[1]);
  point("b0sum", "ax", "ay", &b[0].sum.a);
  point("b0sum", "bx", "by", &b[0].sum.b);
  word("b0sum", "z", &b[0].sum.z);
  word("", "b1ix", &b[1].sig[N + 1u]);
  word("", "b1iy", &b[1].sig[N + 2u]);
  cipher_words("b1j0", &b[1].cts[0]);
  cipher_words("b1j1", &b[1].cts[1]);
  cipher_words("b2j0", &b[2].cts[0]);
  cipher_words("b2j1", &b[2].cts[1]);
  word("", "l1", &l1);
  word("", "l2", &l2);
  open_words("ty", shares, &yes, &l1, &l2);
  open_words("tn", shares, &no, &l1, &l2);
}

int main(int argc, char **argv) {
  static const uint32_t choice[3] = {0u, 0u, 1u};
  Ballot b[3];
  ThrShare shares[N];
  ThrPart parts[2];
  G1 ring[N];
  G1 keys[N];
  G1 pk;
  U256 sk;
  U256 seed;
  U256 poll;
  U256 x;
  U256 copied;
  U256 tampered;
  ElgCipher yes;
  ElgCipher no;
  char text[67];
  uint64_t m_yes = 0;
  uint64_t m_no = 0;
  uint32_t v;
  int ok = 1;
  blsag_key(&sk, 5u);
  blsag_key(&seed, 17u);
  if (!thr_deal(shares, &pk, &sk, &seed, T, N)) {
    fputs("FAIL ballot: the deal refuses\n", stderr);
    return 1;
  }
  for (v = 0; v < N; v++) {
    blsag_key(&x, v);
    elg_public(&ring[v], &x);
    keys[v] = shares[v].key;
  }
  u256_from_u64(&poll, 0u);
  for (v = 0; v < 3u; v++)
    if (!cast(&b[v], ring, &pk, &poll, v, choice[v])) {
      fprintf(stderr, "FAIL ballot: the cast of voter %u refuses\n", v);
      return 1;
    }
  if (argc == 2 && strcmp(argv[1], "words") == 0) {
    words(&pk, ring, shares, b);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "dao") == 0)
    return dao(&pk, shares, b);
  /* The ctx binding: voter 0's ctx is the pinned word and the ballot
     verifies under it. */
  hex(text, sizeof text, &b[0].ctx);
  ok &= (strcmp(text, CTX0) == 0 && sig_onehot_verify(b[0].cts, b[0].slots, &b[0].sum, &pk, K, &b[0].ctx) == SIG_OK)
        || fail("ctx binding");
  /* The digest signature: each ring signature verifies over its own
     digest, and voter 0's signature over the digest of voter 1's slots
     does not. */
  ballot_digest(&tampered, b[1].cts);
  ok &= (blsag_verify(ring, N, &poll, &b[0].digest, b[0].sig) == BLSAG_ACCEPT
         && blsag_verify(ring, N, &poll, &b[1].digest, b[1].sig) == BLSAG_ACCEPT
         && blsag_verify(ring, N, &poll, &b[2].digest, b[2].sig) == BLSAG_ACCEPT
         && blsag_verify(ring, N, &poll, &tampered, b[0].sig) == BLSAG_REJECT)
        || fail("digest signature");
  /* A copy of voter 0's slots and proofs under voter 1's key image. */
  ballot_ctx(&copied, &b[1].sig[N + 1u], &b[1].sig[N + 2u], &poll);
  ok &= sig_onehot_verify(b[0].cts, b[0].slots, &b[0].sum, &pk, K, &copied) == SIG_BAD_SLOT || fail("copy under a second key image");
  /* The tally: the quorum {1, 2} opens the slot sums to (2, 1). */
  slot_sum(&yes, b, 0u);
  slot_sum(&no, b, 1u);
  quorum(parts, shares, &yes);
  ok &= (thr_open(&m_yes, parts, 2u, keys, T, N, &yes, BITS) == THR_FOUND && m_yes == 2u) || fail("tally yes");
  quorum(parts, shares, &no);
  ok &= (thr_open(&m_no, parts, 2u, keys, T, N, &no, BITS) == THR_FOUND && m_no == 1u) || fail("tally no");
  if (!ok)
    return 1;
  puts("ballot regressions: passed");
  return dao_regressions(&pk, shares, b);
}
