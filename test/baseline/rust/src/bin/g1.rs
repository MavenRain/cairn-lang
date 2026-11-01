//! G1: an append-only record of content hashes (g1-record.lang).
//!
//! A record is a vector of entries, each a digest. The head of a record runs
//! the hash chain from the genesis digest; e2 is the head of the record [e1].
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

/// The position of the first entry equal to entry, or None.
fn verify(entry: &Word, record: &[Word]) -> Option<usize> {
    record.iter().position(|e| e == entry)
}

fn show_option(value: Option<usize>) -> String {
    value.map_or("none".to_string(), |j| format!("some {j}"))
}

fn main() {
    let r0: Vec<Word> = Vec::new();
    let e1 = head(&r0);
    let r1 = [r0, vec![e1]].concat();
    let e2 = head(&r1);
    let r2 = [r1, vec![e2]].concat();
    println!("{}", show_option(verify(&e2, &r2)));
}
