"""G4: a rule change (g4-amend.lang).

The polity s0 has 3 members and each threshold two. pAppend sets tAppend
to three. A step enacts a proposal when its yes count meets tAmend, else
it keeps the polity.
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


class Polity(NamedTuple):
    members: tuple
    t_append: int
    t_amend: int
    t_open: int
    key_open: int
    key_append: int


def step(polity, decided):
    proposal, yes = decided
    return proposal if polity.t_amend <= yes else polity


r0 = ()
k1 = head(r0)
r1 = r0 + (k1,)
k2 = head(r1)
r2 = r1 + (k2,)
k3 = head(r2)
crew = (k1, k2, k3)
s0 = Polity(crew, 2, 2, 2, k1, k2)
p_append = Polity(crew, 3, 2, 2, k1, k3)
print(step(s0, (p_append, 2)).t_append)
print(step(s0, (p_append, 1)).t_append)
