---
name: cairn-lang
description: Write, check and repair cairn-lang programs (a Cubical TT language for an append-only public record of content hashes, governed off chain by a threshold DAO) with the langc checker. Use it when the task is in a yes row of the cairn-lang README table.
---

# cairn-lang

## Routing rule

Use cairn-lang when the task is in a "yes" row of the README table: an
append-only record of content hashes with inclusion proofs (G1), a
threshold committee that signs each append (G2), an encrypted vote with a
threshold tally (G3), a rule change (G4) or a change of the rule that
changes the rules (G5). The DAO encrypts its ballots (FHE), proves each
step (ZK) and opens only by threshold (MPC). Do not use cairn-lang for a
token or a general contract, for a wall-clock deadline (there is no
clock: time is the number of operations applied), or for state with
deletes or edits.

## The loop

1. Write FILE.lang: one `def NAME : TYPE := TERM` for each value. A guest
   file names the domain defs only (design/DESIGN.md). A word literal (for
   example a key or a signature word) is a kit item: a kit list names the
   file, and `langc --kit` checks it (G2).
2. Run `build/langc check --json FILE.lang` (`build/langc --kit check
   --json FILE.lang` for a kit file). It writes one JSON line on stdout,
   with the `diagnostics` array and the `progress` object
   (docs/CHECK-JSON.md). Exit 0: the checker accepts the file. Exit 1: the
   checker refuses the file.
3. For each diagnostic, `build/langc explain CODE` gives the row of its
   `code`. Change the file, then do step 2 again.
4. Run `build/langc fmt FILE.lang --check`. No output and exit 0: the
   text is canonical. Else write the output of `build/langc fmt FILE.lang`
   to a new file, and replace FILE.lang with it.
5. Run `build/langc eval FILE.lang NAME` for the value of the def NAME. It
   prints a Flag or a Verdict as `1` or `0`, an Option as `some X` or
   `none`, an Enc as `encEta Nat N` and a record as `fmMul ...`.

For a kit file, use `build/langc --kit eval FILE.lang NAME` in step 5.

`build/langc mcp` gives the same verbs to an agent as a Model Context
Protocol server on stdin and stdout (docs/MCP.md). Its 5 tools are
`check`, `fmt`, `eval`, `build` and `explain`. With `--kit`, each tool runs in the kit mode.

## Verbs

`build/langc` with no argument prints this usage text:

```text
usage: langc [--kit] check [--json] PROG
       langc [--kit] eval PROG NAME [ARGS...]
       langc [--kit] run PROG SCRIPT
       langc [--kit] ir PROG
       langc [--kit] abi PROG [--text]
       langc [--kit] build PROG (-o OUT | --js OUT.mjs | --wgsl OUT.wgsl) [--runtime]
       langc [--kit] prove PROG NAME [POLL]
       langc fmt PROG [--check]
       langc explain CODE
       langc [--kit] mcp
--kit: the kit mode, the trusted test scope of the kit gate

```

- `check [--json] PROG`: check PROG. Text: `ok` on stdout, or one line on
  stderr.
- `eval PROG NAME [ARGS...]`: the value of the def NAME.
- `fmt PROG [--check]`: print the canonical text of PROG; with `--check`,
  no output and exit 0 when PROG is canonical.
- `explain CODE`: the row of the diagnostic code CODE.
- `mcp`: the MCP server (docs/MCP.md).
- `run`, `ir`, `abi`, `build` and `prove`: the back end and the targets
  (README.md, docs/PLAN.md). This skill does not use them.

## Examples

The examples go from easy to hard. Run each command from the repository
root. The block below each command is its stdout and its stderr. The gate
(test/skilltool.c) runs each command and compares the output and the exit
code with the text.

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

```text
ok
```

The exit code is 0.

`verify e2 r2` gives the position of the entry e2 in the record r2.

`build/langc eval examples/skill/g1-record.lang verifyHit` gives:

```text
some 1
```

The exit code is 0.

### G2

A committee of 3 signs each append, and 2 signatures are enough. The key
and the signature words are word literals from build/frosttool, and a
word literal is a kit item. Thus it is a kit file: examples/skill/kit.txt
names it, and `langc --kit` checks it.

File: examples/skill/g2-committee.lang

```lang
-- G2 (S14, Q-S14-4 (a)): a 2 of 3 committee signs each append. A kit file
-- (examples/skill/kit.txt): the key and the signature words are word
-- literals from build/frosttool (test/frosttool.c), and a word literal is a
-- kit item. The message is (head r0, 42u), with r0 the empty record. badA is
-- goodA with z + 1, thus append refuses it.
def kA : Point := 0x987fb549d982c26644ae1a92fbdf96fd1c767cc63a42014959ccdb676f405889u

def kO : Point := 0xf7f36a2ad3963a54863980b0ddf4c0cab710f2f1d88e3102eca3ec0429bc67e7u

def pol : Polity := makePolity (fmUnit Point) 2 2 2 kO kA

def r0 : Record := fmUnit Entry

def goodA : Auth :=
  authOf 0x63c55ee48262d59010d57436eb0b08a2ebfbdb4260c79782b14ce492739d695au 0xb5b183ab4ae8de8398af4513f5e2547ed261b7cc140b3379f9923c8fba54a82eu

def badA : Auth :=
  authOf 0x63c55ee48262d59010d57436eb0b08a2ebfbdb4260c79782b14ce492739d695au 0xb5b183ab4ae8de8398af4513f5e2547ed261b7cc140b3379f9923c8fba54a82fu

-- L10: a good Auth puts the entry at the end of the record.
def l10 : Path (Option Record) (append pol goodA 42u r0) (some (snoc 42u r0)) := refl

-- L11: the same Auth with z + 1 gives none.
def l11 : Path (Option Record) (append pol badA 42u r0) none := refl

-- Eval rows: the good Auth and the refused Auth (z + 1).
def appGood : Option Record := append pol goodA 42u r0

def appBad : Option Record := append pol badA 42u r0
```

`build/langc --kit check examples/skill/g2-committee.lang` gives:

```text
ok
```

The exit code is 0.

The guest check refuses the file at its first word literal (kA),
because a word literal is a kit item.

`build/langc check examples/skill/g2-committee.lang` gives:

```text
langc: REFUSE_KIT: kA: a word literal is a kit item outside the base span (PLAN section 4); only the kit mode langc --kit admits it
```

The exit code is 1.

`append` with the good signature goodA gives the new record.

`build/langc --kit eval examples/skill/g2-committee.lang appGood` gives:

```text
some (fmMul U256 (fmUnit U256) (fmEta U256 42u))
```

The exit code is 0.

badA is goodA with z + 1. `append` refuses the signature and gives
`none`.

`build/langc --kit eval examples/skill/g2-committee.lang appBad` gives:

```text
none
```

The exit code is 0.

### G3

Seal 3 votes (yes, no, yes) as encrypted ballots, add them under the
encryption, and open only the sum. It is a guest file.

File: examples/skill/g3-vote.lang

```lang
-- G3 (S14, Q-S14-2 (a)): an encrypted vote with a threshold tally. A guest
-- file: it names the domain defs only. b1, b2 and b3 seal a yes, a no and a
-- yes vote; the points are head digests (no literal). tallyBox adds the
-- sealed votes of a box under encryption, and a quorum opens the sum.
def r0 : Record := fmUnit Entry

def k1 : Point := head r0

def r1 : Record := snoc k1 r0

def k2 : Point := head r1

def r2 : Record := snoc k2 r1

def k3 : Point := head r2

def r3 : Record := snoc k3 r2

def k4 : Point := head r3

def b1 : SealedBallot := mkBallot verdictYes k2

def b2 : SealedBallot := mkBallot verdictNo k3

def b3 : SealedBallot := mkBallot verdictYes k4

def box0 : Box := fmUnit SealedBallot

def box1 : Box := fmMul SealedBallot box0 (fmEta SealedBallot b1)

def box3 : Box :=
  fmMul SealedBallot (fmMul SealedBallot box1 (fmEta SealedBallot b2)) (fmEta SealedBallot b3)

-- L6 on closed boxes of 0, 1 and 3 ballots: the opened tally is the count of
-- the plain votes.
def l6Zero : (q : Quorum) -> Path Count (open Count q (tallyBox box0)) (countVotes (fmMap SealedBallot Verdict (fun (x : SealedBallot) => decrypt Verdict (ballotVote x)) box0)) :=
  fun (q : Quorum) => refl

def l6One : (q : Quorum) -> Path Count (open Count q (tallyBox box1)) (countVotes (fmMap SealedBallot Verdict (fun (x : SealedBallot) => decrypt Verdict (ballotVote x)) box1)) :=
  fun (q : Quorum) => refl

def l6Three : (q : Quorum) -> Path Count (open Count q (tallyBox box3)) (countVotes (fmMap SealedBallot Verdict (fun (x : SealedBallot) => decrypt Verdict (ballotVote x)) box3)) :=
  fun (q : Quorum) => refl

-- L7: open is decrypt for each quorum and ciphertext.
def l7 : (q : Quorum) -> (c : Enc Count) -> Path Count (open Count q c) (decrypt Count c) :=
  fun (q : Quorum) (c : Enc Count) => refl

-- Eval rows: the tally of 0, 1 and 3 ballots and the plain count of the 3
-- votes.
def tallyZero : Enc Count := tallyBox box0

def tallyOne : Enc Count := tallyBox box1

def tallyThree : Enc Count := tallyBox box3

def countThree : Count :=
  countVotes (fmMul Verdict (fmMul Verdict (fmEta Verdict verdictYes) (fmEta Verdict verdictNo)) (fmEta Verdict verdictYes))
```

`build/langc check examples/skill/g3-vote.lang` gives:

```text
ok
```

The exit code is 0.

`tallyBox box3` adds the 3 sealed votes. The sum is 2 yes votes.

`build/langc eval examples/skill/g3-vote.lang tallyThree` gives:

```text
encEta Nat 2
```

The exit code is 0.

### G4

Change a rule. The polity s0 has 3 members, and each threshold is two.
The proposal pAppend sets tAppend to three. stepD enacts a proposal when
its yes count meets tAmend, else it keeps the polity. It is a guest file.

File: examples/skill/g4-amend.lang

```lang
-- G4 (S14, Q-S14-2 (a)): a rule change. A guest file: it names the domain
-- defs only. The polity s0 has 3 members and each threshold two. pAppend
-- sets tAppend to three. stepD enacts a proposal when its yes count meets
-- tAmend, else it keeps the polity.
def zero : Count := len Point (fmUnit Point)

def one : Count := opSucc zero

def two : Count := opSucc one

def three : Count := opSucc two

def r0 : Record := fmUnit Entry

def k1 : Point := head r0

def r1 : Record := snoc k1 r0

def k2 : Point := head r1

def r2 : Record := snoc k2 r1

def k3 : Point := head r2

def crew : FreeMon Point :=
  fmMul Point (fmMul Point (fmEta Point k1) (fmEta Point k2)) (fmEta Point k3)

def s0 : Polity := makePolity crew two two two k1 k2

def pAppend : Proposal := makePolity crew three two two k1 k3

-- L8 on the closed s0 (tAmend two): a yes count of two meets the threshold,
-- so the step enacts p.
def l8 : (p : Proposal) -> Path Polity (stepD s0 (pair p two)) p := fun (p : Proposal) => refl

-- Eval rows: decideYes at and below the threshold, and tAppend after the
-- step with two yes votes (enacted) and with one (refused).
def decideExact : Verdict := decideYes s0 two

def decideShort : Verdict := decideYes s0 one

def g4Append : Count := tAppend (stepD s0 (pair pAppend two))

def g4Refused : Count := tAppend (stepD s0 (pair pAppend one))
```

`build/langc check examples/skill/g4-amend.lang` gives:

```text
ok
```

The exit code is 0.

With two yes votes, stepD enacts pAppend: tAppend is 3.

`build/langc eval examples/skill/g4-amend.lang g4Append` gives:

```text
3
```

The exit code is 0.

With one yes vote, stepD keeps s0: tAppend stays 2.

`build/langc eval examples/skill/g4-amend.lang g4Refused` gives:

```text
2
```

The exit code is 0.

### G5

Change the rule that changes the rules. s0 has each threshold two.
pLower lowers tAmend to one, thus after it pOpen passes with one yes vote
and sets tOpen to three. amendAll steps each decided proposal in turn,
the left factor first. It is a guest file.

File: examples/skill/g5-self-amend.lang

```lang
-- G5 (S14, Q-S14-2 (a)): a self change. A guest file: it names the domain
-- defs only. s0 has each threshold two. pLower lowers tAmend to one, thus
-- after it pOpen passes with one yes vote and sets tOpen to three. amendAll
-- steps each decided proposal in turn, the left factor first (L9).
def zero : Count := len Point (fmUnit Point)

def one : Count := opSucc zero

def two : Count := opSucc one

def three : Count := opSucc two

def r0 : Record := fmUnit Entry

def k1 : Point := head r0

def r1 : Record := snoc k1 r0

def k2 : Point := head r1

def r2 : Record := snoc k2 r1

def k3 : Point := head r2

def crew : FreeMon Point :=
  fmMul Point (fmMul Point (fmEta Point k1) (fmEta Point k2)) (fmEta Point k3)

def s0 : Polity := makePolity crew two two two k1 k2

def pLower : Proposal := makePolity crew two one two k1 k2

def pOpen : Proposal := makePolity crew two one three k1 k2

-- L9: amendAll folds the left factor first.
def l9 : (ps : FreeMon Decided) -> (qs : FreeMon Decided) -> (s : Polity) -> Path Polity (amendAll (fmMul Decided ps qs) s) (amendAll qs (amendAll ps s)) :=
  fun (ps : FreeMon Decided) (qs : FreeMon Decided) (s : Polity) => refl

-- Eval rows: tOpen after pLower (two yes votes) then pOpen (one yes vote),
-- and after pOpen alone (one yes vote, refused).
def g5Open : Count :=
  tOpen (amendAll (fmMul Decided (fmEta Decided (pair pLower two)) (fmEta Decided (pair pOpen one))) s0)

def g5Refused : Count := tOpen (amendAll (fmEta Decided (pair pOpen one)) s0)
```

`build/langc check examples/skill/g5-self-amend.lang` gives:

```text
ok
```

The exit code is 0.

pLower with two yes votes, then pOpen with one yes vote: tOpen is 3.

`build/langc eval examples/skill/g5-self-amend.lang g5Open` gives:

```text
3
```

The exit code is 0.

pOpen alone with one yes vote: tAmend is still two, thus tOpen stays 2.

`build/langc eval examples/skill/g5-self-amend.lang g5Refused` gives:

```text
2
```

The exit code is 0.
