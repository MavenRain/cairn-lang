"""G5: a self change (g5-self-amend.lang).

s0 has each threshold two. pLower lowers tAmend to one, thus after it
pOpen passes with one yes vote and sets tOpen to three. amend_all steps
each decided proposal in turn, the first one first.
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


def amend_all(decided, polity):
    return functools.reduce(step, decided, polity)


r0 = ()
k1 = head(r0)
r1 = r0 + (k1,)
k2 = head(r1)
r2 = r1 + (k2,)
k3 = head(r2)
crew = (k1, k2, k3)
s0 = Polity(crew, 2, 2, 2, k1, k2)
p_lower = Polity(crew, 2, 1, 2, k1, k2)
p_open = Polity(crew, 2, 1, 3, k1, k2)
print(amend_all(((p_lower, 2), (p_open, 1)), s0).t_open)
print(amend_all(((p_open, 1),), s0).t_open)
