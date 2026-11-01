"""G1: an append-only record of content hashes (g1-record.lang).

A record is a tuple of entries, each a digest. The head of a record runs
the hash chain from the genesis digest; e2 is the head of the record [e1].
"""
import functools
import hashlib

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


def verify(entry, record):
    """The position of the first entry equal to entry, or None."""
    return next((j for j, e in enumerate(record) if e == entry), None)


def show_option(value):
    return "none" if value is None else f"some {value}"


r0 = ()
e1 = head(r0)
r1 = r0 + (e1,)
e2 = head(r1)
r2 = r1 + (e2,)
print(show_option(verify(e2, r2)))
