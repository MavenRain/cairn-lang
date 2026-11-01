### G1

Keep an append-only record of content hashes, and find an entry in it.
The polity has 1 member, and each threshold is 1. It is a guest file: it
names the domain defs only, and it has no literal.

File: examples/skill/g1-record.lang

```lang
-- G1 (S14, Q-S14-3 (a)): an append-only record of content hashes and a polity
-- of 1 member with each threshold 1. A guest file: it names the domain defs
-- only. The entries are head digests and the counts come from len and opSucc
-- (no literal), thus the file needs no kit item.
def r0 : Record := fmUnit Entry

def e1 : Entry := head r0

def r1 : Record := snoc e1 r0

def e2 : Entry := head r1

def r2 : Record := snoc e2 r1

def e3 : Entry := head r2

def p1 : OpPos := opSucc (len Entry r0)

def zero : Count := len Point (fmUnit Point)

def one : Count := opSucc zero

def solo : Polity := makePolity (fmEta Point e1) one one one e1 e1

-- L1: r is a prefix of snoc e r, the rest is fmEta e.
def l1 : (e : Entry) -> (r : Record) -> prefix r (snoc e r) :=
  fun (e : Entry) (r : Record) => pack (fmEta Entry e) refl

-- L4: verify finds e2 at the position len r1 of r2.
def l4 : Path (Option OpPos) (verify e2 r2) (some (len Entry r1)) := refl

-- Eval rows: verify a hit and a miss, verifyAt an inclusion proof, and the
-- one vote of the one member at and below the threshold.
def verifyHit : Option OpPos := verify e2 r2

def verifyMiss : Option OpPos := verify e3 r2

def verifyAtYes : Verdict := verifyAt e2 (inclusion p1 r2) (head r2)

def soloYes : Verdict := decideYes solo one

def soloNo : Verdict := decideYes solo zero
```

`build/langc check examples/skill/g1-record.lang` gives:

The exit code is 0.

`verify e2 r2` gives the position of the entry e2 in the record r2.

`build/langc eval examples/skill/g1-record.lang verifyHit` gives:

```text
some 1
```

The exit code is 0.
