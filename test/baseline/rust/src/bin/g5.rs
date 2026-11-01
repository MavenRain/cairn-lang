//! G5: a self change (g5-self-amend.lang).
//!
//! s0 has each threshold two. pLower lowers tAmend to one, thus after it
//! pOpen passes with one yes vote and sets tOpen to three. amend_all steps
//! each decided proposal in turn, the first one first.
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

#[derive(Clone)]
struct Polity {
    members: Vec<Word>,
    t_append: u64,
    t_amend: u64,
    t_open: u64,
    key_open: Word,
    key_append: Word,
}

fn step(polity: Polity, (proposal, yes): (Polity, u64)) -> Polity {
    if polity.t_amend <= yes { proposal } else { polity }
}

fn amend_all(decided: Vec<(Polity, u64)>, polity: Polity) -> Polity {
    decided.into_iter().fold(polity, step)
}

fn main() {
    let r0: Vec<Word> = Vec::new();
    let k1 = head(&r0);
    let r1 = [r0, vec![k1]].concat();
    let k2 = head(&r1);
    let r2 = [r1, vec![k2]].concat();
    let k3 = head(&r2);
    let crew = vec![k1, k2, k3];
    let s0 = Polity { members: crew.clone(), t_append: 2, t_amend: 2, t_open: 2, key_open: k1, key_append: k2 };
    let p_lower = Polity { members: crew.clone(), t_append: 2, t_amend: 1, t_open: 2, key_open: k1, key_append: k2 };
    let p_open = Polity { members: crew, t_append: 2, t_amend: 1, t_open: 3, key_open: k1, key_append: k2 };
    println!("{}", amend_all(vec![(p_lower, 2), (p_open.clone(), 1)], s0.clone()).t_open);
    println!("{}", amend_all(vec![(p_open, 1)], s0).t_open);
}
