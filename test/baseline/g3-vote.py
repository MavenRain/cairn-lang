"""G3: an encrypted vote with a sealed tally (g3-vote.lang).

Sealed is the cell of an encrypted value: map2 joins two sealed values
under the seal, thus the tally of a box stays sealed. A ballot is a sealed
vote and a link tag; the tags are head digests.
"""
import functools
import hashlib
from typing import NamedTuple

TAG_GENESIS = int.from_bytes(b"cairn-genesis", "big")
TAG_APPEND = int.from_bytes(b"cairn-append", "big")


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


class Sealed(NamedTuple):
    value: object

    def map2(self, f, other):
        return Sealed(f(self.value, other.value))


class Ballot(NamedTuple):
    vote: Sealed
    tag: int


def tally_box(box):
    return functools.reduce(
        lambda t, b: t.map2(lambda n, yes: n + 1 if yes else n, b.vote),
        box,
        Sealed(0),
    )


r0 = ()
k1 = head(r0)
r1 = r0 + (k1,)
k2 = head(r1)
r2 = r1 + (k2,)
k3 = head(r2)
r3 = r2 + (k3,)
k4 = head(r3)
box3 = (
    Ballot(Sealed(True), k2),
    Ballot(Sealed(False), k3),
    Ballot(Sealed(True), k4),
)
print(f"encEta Nat {tally_box(box3).value}")
