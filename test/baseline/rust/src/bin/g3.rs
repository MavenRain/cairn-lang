//! G3: an encrypted vote with a sealed tally (g3-vote.lang).
//!
//! Sealed is the cell of an encrypted value: map2 joins two sealed values
//! under the seal, thus the tally of a box stays sealed. A ballot is a sealed
//! vote and a link tag; the tags are head digests.
use sha2::{Digest, Sha256};

type Word = [u8; 32];

const TAG_GENESIS: &[u8] = b"cairn-genesis";
const TAG_APPEND: &[u8] = b"cairn-append";

/// A big-endian 32-byte word with the bytes at the end.
fn word(bytes: &[u8]) -> Word {
    let mut w = [0; 32];
    w.iter_mut().rev().zip(bytes.iter().rev()).for_each(|(d, s)| *d = *s);
    w
}

/// SHA-256 of two big-endian 32-byte words, read as a big-endian word.
fn word_hash(a: &Word, b: &Word) -> Word {
    Sha256::new().chain_update(a).chain_update(b).finalize().into()
}

fn head(record: &[Word]) -> Word {
    record.iter().fold(word_hash(&word(TAG_GENESIS), &[0; 32]), |h, e| {
        word_hash(&word_hash(&word(TAG_APPEND), &h), e)
    })
}

struct Sealed<T>(T);

impl<T> Sealed<T> {
    fn map2<U, V>(self, f: impl FnOnce(T, U) -> V, other: Sealed<U>) -> Sealed<V> {
        Sealed(f(self.0, other.0))
    }
}

struct Ballot {
    vote: Sealed<bool>,
    tag: Word,
}

fn tally_box(ballots: Vec<Ballot>) -> Sealed<u64> {
    ballots.into_iter().fold(Sealed(0), |t, b| {
        t.map2(|n, yes| if yes { n + 1 } else { n }, b.vote)
    })
}

fn main() {
    let r0: Vec<Word> = Vec::new();
    let k1 = head(&r0);
    let r1 = [r0, vec![k1]].concat();
    let k2 = head(&r1);
    let r2 = [r1, vec![k2]].concat();
    let k3 = head(&r2);
    let r3 = [r2, vec![k3]].concat();
    let k4 = head(&r3);
    let box3 = vec![
        Ballot { vote: Sealed(true), tag: k2 },
        Ballot { vote: Sealed(false), tag: k3 },
        Ballot { vote: Sealed(true), tag: k4 },
    ];
    println!("encEta Nat {}", tally_box(box3).0);
}
