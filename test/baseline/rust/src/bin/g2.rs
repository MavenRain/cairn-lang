//! G2: a 2 of 3 committee signs each append (g2-committee.lang).
//!
//! An Auth (rx, z) is a Schnorr signature on secp256k1 of the key kA on the
//! message (head r, e). A point is lifted from x with the smaller y. append
//! puts e at the end of r only when the signature holds.
use k256::elliptic_curve::bigint::Encoding;
use k256::elliptic_curve::ops::Reduce;
use k256::elliptic_curve::point::DecompressPoint;
use k256::elliptic_curve::sec1::ToEncodedPoint;
use k256::elliptic_curve::subtle::Choice;
use k256::elliptic_curve::PrimeField;
use k256::{AffinePoint, FieldBytes, ProjectivePoint, Scalar, U256};
use sha2::{Digest, Sha256};

type Word = [u8; 32];

const TAG_GENESIS: &[u8] = b"cairn-genesis";
const TAG_APPEND: &[u8] = b"cairn-append";
const TAG_AUTH: &[u8] = b"cairn-auth";

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

fn head(record: &[u64]) -> Word {
    record.iter().fold(word_hash(&word(TAG_GENESIS), &[0; 32]), |h, e| {
        word_hash(&word_hash(&word(TAG_APPEND), &h), &word(&e.to_be_bytes()))
    })
}

/// The point (x, y) with the smaller y, or None.
fn lift_x(x: &Word) -> Option<AffinePoint> {
    let lift = |odd: u8| -> Option<AffinePoint> {
        AffinePoint::decompress(&FieldBytes::from(*x), Choice::from(odd)).into()
    };
    let y = |p: &AffinePoint| p.to_encoded_point(false).y().map(|y| y.to_vec());
    lift(0).zip(lift(1)).map(|(a, b)| if y(&a) <= y(&b) { a } else { b })
}

fn schnorr_ok(px: &Word, rx: &Word, z: &Word, digest: &Word, entry: &Word) -> bool {
    let c = word_hash(&word_hash(&word_hash(&word_hash(&word(TAG_AUTH), px), rx), digest), entry);
    let c = <Scalar as Reduce<U256>>::reduce_bytes(&FieldBytes::from(c));
    let z: Option<Scalar> = Scalar::from_repr(FieldBytes::from(*z)).into();
    lift_x(px).zip(lift_x(rx)).zip(z).is_some_and(|((key, nonce), z)| {
        ProjectivePoint::GENERATOR * z == ProjectivePoint::from(nonce) + ProjectivePoint::from(key) * c
    })
}

fn append(key: &Word, (rx, z): (Word, Word), entry: u64, record: Vec<u64>) -> Option<Vec<u64>> {
    schnorr_ok(key, &rx, &z, &head(&record), &word(&entry.to_be_bytes()))
        .then(|| [record, vec![entry]].concat())
}

fn show_record(record: &[u64]) -> String {
    record.iter().fold("fmUnit U256".to_string(), |s, e| {
        format!("fmMul U256 ({s}) (fmEta U256 {e}u)")
    })
}

fn show_option(record: Option<Vec<u64>>) -> String {
    record.map_or("none".to_string(), |r| format!("some ({})", show_record(&r)))
}

const K_APPEND: U256 =
    U256::from_be_hex("987FB549D982C26644AE1A92FBDF96FD1C767CC63A42014959CCDB676F405889");
const GOOD_RX: U256 =
    U256::from_be_hex("63C55EE48262D59010D57436EB0B08A2EBFBDB4260C79782B14CE492739D695A");
const GOOD_Z: U256 =
    U256::from_be_hex("B5B183AB4AE8DE8398AF4513F5E2547ED261B7CC140B3379F9923C8FBA54A82E");

fn main() {
    let key = K_APPEND.to_be_bytes();
    let good = (GOOD_RX.to_be_bytes(), GOOD_Z.to_be_bytes());
    let bad = (good.0, GOOD_Z.wrapping_add(&U256::ONE).to_be_bytes());
    println!("{}", show_option(append(&key, good, 42, Vec::new())));
    println!("{}", show_option(append(&key, bad, 42, Vec::new())));
}
