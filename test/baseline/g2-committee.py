"""G2: a 2 of 3 committee signs each append (g2-committee.lang).

An Auth (rx, z) is a Schnorr signature on secp256k1 of the key kA on the
message (head r, e). A point is lifted from x with the smaller y. append
puts e at the end of r only when the signature holds.
"""
import functools
import hashlib

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (
    0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
    0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8,
)
TAG_GENESIS = int.from_bytes(b"cairn-genesis", "big")
TAG_APPEND = int.from_bytes(b"cairn-append", "big")
TAG_AUTH = int.from_bytes(b"cairn-auth", "big")


def word_hash(a, b):
    """SHA-256 of two big-endian 32-byte words, read as a big-endian word."""
    data = a.to_bytes(32, "big") + b.to_bytes(32, "big")
    return int.from_bytes(hashlib.sha256(data).digest(), "big")


def head(record):
    return functools.reduce(
        lambda h, e: word_hash(word_hash(TAG_APPEND, h), e),
        record,
        word_hash(TAG_GENESIS, 0),
    )


def point_add(a, b):
    """The affine sum of two points; None is the point at infinity."""
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and (a[1] + b[1]) % P == 0:
        return None
    if a == b:
        s = 3 * a[0] * a[0] * pow(2 * a[1], -1, P) % P
    else:
        s = (b[1] - a[1]) * pow(b[0] - a[0], -1, P) % P
    x = (s * s - a[0] - b[0]) % P
    return (x, (s * (a[0] - x) - a[1]) % P)


def point_mul(k, point):
    return functools.reduce(
        lambda acc, bit: point_add(point_add(acc, acc), point if bit == "1" else None),
        bin(k)[2:],
        None,
    )


def lift_x(x):
    """The point (x, y) with the smaller y, or None."""
    if x >= P:
        return None
    rhs = (pow(x, 3, P) + 7) % P
    y = pow(rhs, (P + 1) // 4, P)
    return (x, min(y, P - y)) if y * y % P == rhs else None


def schnorr_ok(px, rx, z, digest, entry):
    key, nonce = lift_x(px), lift_x(rx)
    if z >= N or key is None or nonce is None:
        return False
    c = word_hash(word_hash(word_hash(word_hash(TAG_AUTH, px), rx), digest), entry)
    return point_mul(z, G) == point_add(nonce, point_mul(c % N, key))


def append(key, auth, entry, record):
    rx, z = auth
    return record + (entry,) if schnorr_ok(key, rx, z, head(record), entry) else None


def show_record(record):
    return functools.reduce(
        lambda s, e: f"fmMul U256 ({s}) (fmEta U256 {e}u)", record, "fmUnit U256"
    )


def show_option(record):
    return "none" if record is None else f"some ({show_record(record)})"


K_APPEND = 0x987FB549D982C26644AE1A92FBDF96FD1C767CC63A42014959CCDB676F405889
GOOD = (
    0x63C55EE48262D59010D57436EB0B08A2EBFBDB4260C79782B14CE492739D695A,
    0xB5B183AB4AE8DE8398AF4513F5E2547ED261B7CC140B3379F9923C8FBA54A82E,
)
BAD = (GOOD[0], GOOD[1] + 1)
print(show_option(append(K_APPEND, GOOD, 42, ())))
print(show_option(append(K_APPEND, BAD, 42, ())))
