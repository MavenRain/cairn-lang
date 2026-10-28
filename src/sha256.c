/* SHA-256 (FIPS 180-4 sections 4.2.2, 5.1.1, 5.3.3 and 6.2). Words are
 * read and written big-endian byte by byte, so the result does not depend
 * on the byte order of the host. */
#include "sha256.h"
#include <stdint.h>

enum { SHA256_BLOCK = 64, SHA256_ROUNDS = 64 };

const uint32_t sha256_round_constant[SHA256_ROUNDS] = {
  0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
  0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
  0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
  0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
  0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
  0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
  0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
  0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

const uint32_t sha256_initial_hash[8] = {
  0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};

static uint32_t rotate(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32 - bits));
}

/* One 64-byte block into STATE (section 6.2.2). */
static void compress(uint32_t state[8], const unsigned char *block) {
  uint32_t w[SHA256_ROUNDS];
  uint32_t v[8];
  for (unsigned t = 0; t < 16; t++)
    w[t] = ((uint32_t)block[4 * t] << 24) | ((uint32_t)block[4 * t + 1] << 16) |
           ((uint32_t)block[4 * t + 2] << 8) | (uint32_t)block[4 * t + 3];
  for (unsigned t = 16; t < SHA256_ROUNDS; t++) {
    uint32_t s0 = rotate(w[t - 15], 7) ^ rotate(w[t - 15], 18) ^ (w[t - 15] >> 3);
    uint32_t s1 = rotate(w[t - 2], 17) ^ rotate(w[t - 2], 19) ^ (w[t - 2] >> 10);
    w[t] = s1 + w[t - 7] + s0 + w[t - 16];
  }
  for (unsigned i = 0; i < 8; i++)
    v[i] = state[i];
  for (unsigned t = 0; t < SHA256_ROUNDS; t++) {
    uint32_t a = v[0];
    uint32_t e = v[4];
    uint32_t ch = (e & v[5]) ^ (~e & v[6]);
    uint32_t maj = (a & v[1]) ^ (a & v[2]) ^ (v[1] & v[2]);
    uint32_t t1 = v[7] + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ch + sha256_round_constant[t] + w[t];
    uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + maj;
    v[7] = v[6];
    v[6] = v[5];
    v[5] = v[4];
    v[4] = v[3] + t1;
    v[3] = v[2];
    v[2] = v[1];
    v[1] = v[0];
    v[0] = t1 + t2;
  }
  for (unsigned i = 0; i < 8; i++)
    state[i] += v[i];
}

void sha256(const unsigned char *data, size_t size, unsigned char digest[32]) {
  uint32_t state[8];
  unsigned char block[SHA256_BLOCK];
  uint64_t bits = (uint64_t)size * 8u;
  size_t done = 0;
  size_t rest;
  for (unsigned i = 0; i < 8; i++)
    state[i] = sha256_initial_hash[i];
  for (; size - done >= SHA256_BLOCK; done += SHA256_BLOCK)
    compress(state, data + done);
  /* The pad (section 5.1.1): 0x80, zero bytes, then the 64-bit length in
     bits. A tail of 56 bytes or more leaves no room for the length, so the
     pad takes a second block. */
  rest = size - done;
  for (size_t i = 0; i < rest; i++)
    block[i] = data[done + i];
  block[rest] = 0x80u;
  for (size_t i = rest + 1; i < SHA256_BLOCK; i++)
    block[i] = 0;
  if (rest + 9 > SHA256_BLOCK) {
    compress(state, block);
    for (size_t i = 0; i < SHA256_BLOCK; i++)
      block[i] = 0;
  }
  for (unsigned i = 0; i < 8; i++)
    block[SHA256_BLOCK - 1 - i] = (unsigned char)(bits >> (8 * i));
  compress(state, block);
  for (unsigned i = 0; i < 32; i++)
    digest[i] = (unsigned char)(state[i / 4] >> (24 - 8 * (i % 4)));
}

void sha256_words(U256 *out, const U256 *a, const U256 *b) {
  unsigned char data[64];
  unsigned char digest[32];
  for (unsigned i = 0; i < 32; i++) {
    data[i] = (unsigned char)(a->limb[3u - i / 8u] >> (56u - 8u * (i % 8u)));
    data[32 + i] = (unsigned char)(b->limb[3u - i / 8u] >> (56u - 8u * (i % 8u)));
  }
  sha256(data, sizeof data, digest);
  for (unsigned k = 0; k < 4; k++) {
    out->limb[3u - k] = 0;
    for (unsigned j = 0; j < 8; j++)
      out->limb[3u - k] = (out->limb[3u - k] << 8) | digest[8u * k + j];
  }
}
