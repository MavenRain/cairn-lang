# cairn-lang plan

cairn-lang is a Cubical TT language for one job: an append-only public
record of content hashes, governed off chain by a threshold DAO that
encrypts its ballots (FHE), proves each step (ZK) and opens only by
threshold (MPC). The DAO can amend its own rules. There is no clock.

The design is `design/DESIGN.md` (the request, verbatim). This file gives
the readings of that request, the route, the slices and the open
questions. Section 11 lists the questions for the USER. A reading that is
not ruled stays a recommendation.

Status: S0 DONE (seed e222eba). S1 DONE: the USER took each
recommendation of section 11 (section 1). S2 DONE: the intent-lang kernel
at 67445ae is ported, without the EVM writer and the swap (section 9).
S3 DONE: the clock parts are gone, with `REFUSE_TIME`, `OpPos` and
`len` (sections 4 and 9). S4 DONE: the Sigma, ElGamal, threshold and bLSAG
code runs on secp256k1 through `group.h`, the hash is SHA-256, and the
BN254 and Keccak code is gone (sections 6.1 and 9). Next: S5 (Part R, the
record).

## 1. Rulings

| Date | Ruling | Source |
|---|---|---|
| 2026-10-25 | Targets: Wasm MVP (tcc-wasm kit), ES module + JSON (tcc-js kit), WGSL compute (tcc-webgpu kit). The first pick, Wasm, is the primary target. The gate runs it first. | DESIGN.md, S0 answers |
| 2026-10-25 | Name: cairn-lang. | DESIGN.md, S0 answers |
| 2026-10-25 | Host: TinyCC. All of `langc` is C99 that TinyCC builds, `langc mcp` too. | DESIGN.md, request |
| 2026-10-25 | S1, verbatim: "Take recommendations". So each S0 question takes (a): Q-S0-1 additive exponential ElGamal; Q-S0-2 secp256k1; Q-S0-3 SHA-256; Q-S0-4 the `tOpen`-th valid share closes a ballot; Q-S0-5 one threshold Schnorr signature (FROST style) under `keyAppend`; Q-S0-6 the hash chain; Q-S0-7 Pedersen DKG with Feldman commitments and a proof of knowledge, and the same for reshare; Q-S0-8 the Anthropic `count_tokens` endpoint; Q-S0-9 the Python and Rust programs run, with equal outputs. | USER, S1 |
| 2026-10-26 | S3: Q-S3-1 to Q-S3-3 each take (a). Q-S3-1: the kit prelude keeps `family Env := makeEnv (caller : Addr)` without `now` and `block`; `contract.c` and `contract.h` go, the prelude text moves into `src/front/front.c`, and `langc run` calls are `CALLER NAME ARGS`. Q-S3-2: `Out` (`pay`, `pull`, `emit`) stays until S5 (O15). Q-S3-3: the bag family `Pos` stays, and the trace position is `def OpPos : Type 0 := Nat` (section 4). | USER, S3 |
| 2026-10-26 | S4, verbatim: "Take all recommendations for Q-S4-*". So Q-S4-1 to Q-S4-7 each take (a). Q-S4-1: the bLSAG key image keeps try-and-increment on secp256k1 (x = H(...) mod p, accept when x^3 + 7 is a square, sqrt = a^((p + 1) / 4)). Q-S4-2: `ecPairing` goes (`OP_EC_PAIRING`, the rows `base-ecpairing` and `pairing-list`, the precompile `g2`, `pairOk`, `pairNil`). Q-S4-3: each example writes G as a `pair` of the two SEC 2 U256 words; there is no `ecGen` op. Q-S4-4: `examples/bn254.lang` becomes `examples/secp256k1.lang` with the SEC 2 p and n; `precompile.lang` stays, less its `ecPairing` and `ecrecover` defs. Q-S4-5: the SHA-256 vector of the empty string comes from the NIST CAVP file `SHA256ShortMsg.rsp` (`Len = 0`). Q-S4-6: the bLSAG tag is "cairn" (O5). Q-S4-7: a new `test/shatool.c` (the FIPS vectors), and `test/secptool.c` is the group tool (the SEC 2 checks). Later in S4, Q-S4-8 (a): the `range2` row of `zk.lang` uses s = r = n, the smallest scalar out of range. Q-S4-9 (b): the row for a point on the curve that is not the key image uses I = n G through the new def `curveG : Nat -> Flag`; the row `zk curveG 1 => 0` replaces `zk curve2 2 => 0`, because (1, y) with y^2 = 8 needs a 77-digit y and an eval Nat argument stops at 2^64 - 1. | USER, S4 |

The seed is `bin/new-lang.sh cairn-lang tcc-wasm` from lang-template
5265a2a. Native code is not a target (the USER did not pick it).

## 2. The reading of R1

R1: the type constructors are the cell monads and their duals in a locally
presentable, locally cartesian closed (infinity,1)-category, as realized in
Cubical TT.

intent-lang (MavenRain/intent-lang, plan CLOSED at 67445ae) has the same
R1. cairn-lang takes its reading and its kernel without change:

- The ambient structure is not limited: Pi, Sigma, `Path`, `PathP`, the
  interval `I`, `transp`, `hcomp`, the universe `U`, `Glue` and `ua`. These
  are the internal language of the category (intent-lang PLAN 2.1).
- A declared type constructor (a declaration with a type parameter) must be
  one of two classes (intent-lang PLAN 2.2):
  - CM, cell monad: the free algebra monad of a cell signature. The
    parameter occurs only in the generator `eta : A -> T A`. Each other
    cell takes closed base data and arguments of type `T A`. Unit is `eta`,
    bind is the recursor, and the laws are paths by induction.
  - CC, cocell comonad: the cofree coalgebra comonad of a cocell signature.
    One observation `eps : D A -> A`, each other field `D A -> D A` or
    `D A -> E -> D A`. Counit is `eps`, extend is the corecursor.
- `REFUSE_CELL` refuses each other parameterized declaration. The cons
  list is refused. The free monoid `FreeMon` is the admitted list.
- Locally presentable gives the free algebra of each cell signature (small
  object argument). Locally cartesian closed gives Pi and Sigma by base
  change, so `Path A a b` is a fiber of `I -> A`.

cairn-lang uses the duality in one more place: time (section 4).

## 3. The minimal base signature (R2)

R2: admit only the base types and operations that an off-chain ZK + FHE +
MPC protocol needs for (R) an append-only public record of content hashes,
governed by (D) a threshold-authorized, self-amending DAO, with no time.

Each name has one of three kinds, as in intent-lang:

| Kind | Meaning |
|---|---|
| slot | The program gives the type from formers and kit base data. |
| defined | The prelude defines it from slots, formers and kit base data. |
| primitive | Opaque. The kit implements it. Only the listed operations use it. |

Kit base data (all of it, O1): `Scalar` (the field of the group order),
`Point` (the group), `Digest` (32 bytes). `Nat` stays internal to the
kernel, as in intent-lang. There is no `U256`, `Addr`, `Env`, `now`,
`block`, `caller` or `Out`. Those are chain data, and cairn-lang has no
chain.

### 3.1 Part R: the record

| Name | Kind | Reading |
|---|---|---|
| Entry | defined | `Digest`: one content hash. The record holds hashes, never content. |
| Record | defined | `FreeMon Entry` (a CM). The only producer is `append`. |
| Auth | primitive | One threshold Schnorr signature of the current append committee on `(head r, e)`, under `keyAppend` (Q-S0-5 (a): FROST style, two MPC rounds; the check is one Schnorr verification). |
| append | defined | `Constitution -> Auth -> Entry -> Record -> Option Record`. It checks `Auth` and then adds `e` at the end. There is no delete and no update: no operation takes a record to a shorter record. |
| head | defined | `Record -> Digest`. The hash chain: `head nil = H(tag_genesis)` and `head (append e r) = H(tag_append, head r, e)`, with `H` = SHA-256 (Q-S0-3 (a), Q-S0-6 (a)). |
| Pos | defined | `Nat`, by `len : Record -> Nat`, the monoid map of `FreeMon` to `Nat`. |
| verify | defined | `Digest -> Record -> Option Pos`: the first position of a digest. |
| Inclusion | defined | The hash-chain suffix from position `p` to the head. `verifyAt : Digest -> Inclusion -> Digest -> Bool` checks a digest against a published head without the full record (Q-S0-6 (a): the proof size grows with the number of later entries). |

### 3.2 Part D: the DAO

From intent-lang Part D (and orbit-lang before it), with the chain parts
removed:

| Name | Kind | Reading |
|---|---|---|
| Member | defined | `Point`: one public key. |
| Constitution | defined record | `members : FreeMon Member`, `tAppend`, `tAmend`, `tOpen` (`Nat` thresholds), `keyOpen : Point` (the DAO decryption key), `keyAppend : Point` (the append committee key, Q-S0-5). |
| Proposal | defined | `Constitution`: the full new constitution. A proposal can change each field, the thresholds of amendment too. |
| Ballot | defined | `Enc Bool` under `keyOpen`, with a ZK proof that the plaintext is 0 or 1 (a disjunctive Chaum-Pedersen proof) and a bLSAG ring signature over `members` (anonymity, one link tag for each member and proposal). |
| cast | defined | `Proposal -> Ballot -> Box -> Option Box`. It refuses a bad proof or a used link tag. |
| tallyEnc | defined | The homomorphic sum of the ballots in a box: `Enc Nat`. |
| Share | primitive | One partial decryption with a DLEQ proof. |
| open | primitive | `Quorum -> Enc A -> A`: `tOpen` valid shares, combined by Lagrange (MPC). |
| decide | defined | `yes >= tAmend` on the opened tally. |
| enact | defined | Replaces the constitution with the proposal. When `members` or a threshold changes, it needs a reshare (Q-S0-7). |
| Deal, Reshare | primitive | Distributed key generation and resharing (Q-S0-7 (a)): a Pedersen DKG with Feldman commitments and a Schnorr proof of knowledge for each dealt secret, as in FROST KeyGen. Reshare preserves the existing public key with the old quorum's Lagrange weights (below). One run for `keyOpen` and one for `keyAppend`. intent-lang has only a trusted dealer (`thr_deal`); an off-chain DAO with no trusted party cannot use one. |

For each key, a reshare uses a quorum `Q` meeting that key's old threshold
(`tOpen` or `tAppend`), with distinct nonzero share identifiers. Dealer
`i` shares `lambda_i(Q) * s_i` using a polynomial of degree
`new_threshold - 1`. Its constant commitment must equal `lambda_i(Q) * Y_i`,
where `Y_i` is its old public verification share. The sum of the constant
commitments must equal the existing public key; otherwise abort. A fresh
DKG is needed for an explicit key change. S6 tests changed membership and
thresholds for both keys, including a mutant that omits the weights.

Self-reference: the proposal is a whole `Constitution`, so the DAO can
amend `tAmend`, the rule that decides amendments. Each proposal is decided
by the constitution in force when it opens. So the step from one
constitution to the next is a function, and a chain of amendments is a fold
over a `FreeMon Proposal` (orbit-lang `amend`).

### 3.3 Enc, ZK and MPC

The readings of intent-lang PLAN 3.4 hold:

- `Enc` is the identity cell monad behind a sealed eliminator. `eta` is the
  ideal encryption. `bind` is homomorphic evaluation. `decrypt` is a
  law-only monad morphism `Enc -> Id`.
- `open q c ≡ decrypt c` is the MPC law.
- `ZKProof W := ‖ W ‖` (a CM): zero knowledge is the `squash` cell.
- `observe := |_| . decrypt`. Privacy is `observe c ≡ observe c'` by
  `squash`. The real guarantee is IND-CPA of the scheme, a VALIDATION
  assumption.

The FHE reading (Q-S0-1, RULED (a)). The only computation under
encryption that R2 needs is the sum of the ballots. A sum needs an
additive homomorphism only. So the scheme is exponential ElGamal
(intent-lang `elgamal.c`, moved to secp256k1 in S4), with `bind` limited
to affine maps. The opened tally shows the yes count; that is the cost of
this ruling. Decryption of a sum ends with a discrete log in the inclusive
range `0..len members`, so a table needs `len members + 1` entries,
including the identity point for zero and the all-yes endpoint (S6). S6
tests zero and all-yes tallies, including the one-member case. A full FHE
scheme is not in the kit.

TCM law of the tally (from intent-lang): `open q (tallyEnc bs) ≡ count
(map decrypt bs)`.

## 4. Time

R2 prohibits the usual concepts of time. Time is only the succession of
operation applications. The reading:

- A run is a trace: the free monoid on the applied operations, `FreeMon Op`
  (a CM, isomorphic to the free monad of `Op * -`). Its length `len` is the
  only measure of duration.
- The behaviour of the record is the dual: the cofree comonad of `Op -> -`
  (a CC). Its fields are `eps : D A -> A` (the current state) and `step :
  D A -> Op -> D A` (the state after one more operation), the cocell form
  of section 2. "Later" means "after more applications". `Op * -` pairs
  with `Op -> -`, so the free monad pairs with the cofree comonad, and that
  pairing runs a trace on a record.
- So a position is an `OpPos`, the value of `len r`, and a deadline is a
  count of later operations, never a clock value. The domain has
  `def OpPos : Type 0 := Nat` (Q-S3-3 (a)); the name `Pos` stays with the
  bag positions of the domain.

Consequences:

- The kit has no clock, so no program can name one. S3 removed the
  intent-lang clock parts: `Env.now`, `Env.block` (its O17), the
  `NOW BLOCK` script columns and the NUMBER lowering (it left with
  `lower.c` in S2). The kit prelude keeps
  `family Env := makeEnv (caller : Addr)` (Q-S3-1 (a)) as a static text in
  `src/front/front.c`, and `langc run` calls are `CALLER NAME ARGS`.
- New refusal code `REFUSE_TIME`: a program uses one of the removed kit
  words (`now`, `block`, `timestamp`, `time`, `clock`, `epoch`). The fix
  hint names `len` and the op-count form. S3 checks the words at a binder
  and before `NAME_UNKNOWN` (`TIME_WORDS` in `src/front/check.c`).
- `len : (A : Type 0) -> FreeMon A -> Nat` is a C builtin (`OP_FM_LEN`),
  not the fold `fmFold A Nat isSetNat 0 natAdd ...`, because the domain
  cannot prove the Nat monoid laws that `fmFold` needs: `Nat` has only the
  non-dependent `fold`, `natAdd` reduces only on two literals (so `refl`
  cannot prove the unit and associativity laws for a variable), and the
  domain has no axioms and no `isSet Nat` (S3 survey section 7). The
  builtin counts `fmEta` 1, `fmUnit` 0, `fmMul` the sum of its sides, a
  unit or associativity path constructor from its trace fields, and a
  formal hcomp its base when its face is false. Open `fmSquash` cells and
  hcomps with active faces stay stuck so neutral path boundaries survive
  normalization. O16 gives the way back to a fold.
- A ballot has no window. Q-S0-4 RULED (a): a ballot closes when
  the `tOpen`-th valid share for it arrives. Casts that come after that
  share do not count. A threshold of members can always decrypt (they hold
  the key), so this rule adds no trust.

## 5. Laws

Each law is a path that the kernel checks. "VALIDATION" marks a
computational assumption that no path can prove (docs/VALIDATION.md).

| Law | Statement |
|---|---|
| L1 append only | `prefix r (append a e r)` for each `r`, `a`, `e` that `append` accepts |
| L2 length | `len (append a e r) ≡ suc (len r)` |
| L3 chain | `head (append a e r) ≡ H (tag_append, head r, e)` |
| L4 verify | `verify e (append a e r) ≡ some (len r)` when `verify e r ≡ none` |
| L5 inclusion | `verifyAt e (inclusion p r) (head r) ≡ true` when `verify e r ≡ some p` |
| L6 tally | `open q (tallyEnc bs) ≡ count (map decrypt bs)` |
| L7 MPC | `open q c ≡ decrypt c` |
| L8 enact | `constitution (enact p s) ≡ p` when `decide` accepts `p` under `s` |
| L9 amend | `amend` over `FreeMon Proposal` is a fold: `amend (ps ++ qs) ≡ amend qs . amend ps` |
| V1 to V4 | VALIDATION: collision resistance of `H` (SHA-256); IND-CPA of ElGamal (DDH on secp256k1); soundness and zero knowledge of the Sigma proofs, DLEQ and bLSAG in the ROM; unforgeability of the threshold Schnorr signature |
| V5 | VALIDATION note (Q-S0-7 (a)): the Pedersen DKG lets a member bias the distribution of the public key (Gennaro, Jarecki, Krawczyk, Rabin). The security of ElGamal and of the threshold Schnorr signature under that bias is an assumption. |

## 6. Route

### 6.1 Kernel

S2 ports the intent-lang kernel at 67445ae, the same way intent-lang ported
orbit-lang (`git archive ... | tar -x`, then `make -B check`, because
`git archive` sets old mtimes). Keep: the Cubical TT checker and evaluator
(`src/front/check.c`, `eval.c`, `core.h`, `parser.c`, `lexer.c`),
`REFUSE_CELL`, `FreeMon`, `Enc`, the Sigma, DLEQ, threshold and bLSAG code.
S2 removes only the EVM writer (`src/evm.c`, `src/asm.c`, `src/lower.c`,
`src/front/writer.c`, `test/asmtool.c` and the EVM rows) and the swap files
(Part S) of the domain, the examples and the tests. The front end at
67445ae still calls the other chain files, so later slices remove them
(S1 check of the intent-lang Makefile and sources):

| File | Users at 67445ae | Leaves in |
|---|---|---|
| `contract.c` (`Env`, `now`, `caller`) | the front end | S3 (no time): DONE, the prelude text is in `front.c` |
| `keccak.c` | `eval.c`, `blsag.c`, `elgamal.c`, `threshold.c` (Fiat-Shamir) | S4 (SHA-256, Q-S0-3 (a)): DONE, `src/sha256.c` replaces it |
| `bn254.c` | the Sigma, ElGamal, threshold and bLSAG code | S4 (secp256k1, Q-S0-2 (a)): DONE, `src/front/group.h` replaces it, on `field.c` and `secp256k1.c` |
| `secp256k1.c` (`ecrecover`) | the front end, `test/ballottool.c` | S4: DONE, the field code is in `src/front/field.c`, `secp256k1.c` is the group backend of `group.h`, and `ecrecover` is gone |

The tcc-wasm `src/front/lower.c`, `src/wasm.c` and `src/ir.h` are written
against the kit front end, not against the ported one. In S2 they leave
the build, but the files stay (`src/ir.h` is restored from e222eba after
the archive). `langc build` then refuses with `REFUSE_LOWER` ("target
pending S8"). S8 adapts the three files to the ported core and puts the
Wasm rows back in the gate.

### 6.2 The three targets

The three kit front ends are different from each other and from the
intent-lang front end (`diff -rq` at S0). So cairn-lang has ONE front end
(the ported kernel) and three writers that read its output:

| Target | Kit source | Reads | Crypto at run time | Use |
|---|---|---|---|---|
| Wasm MVP (primary) | tcc-wasm `src/wasm.c`, `src/front/lower.c`, `src/ir.h` | the first-order IR | a prelude of Wasm functions that `wasm.c` emits: field and scalar arithmetic on i64 limbs, point add and multiply, the hash | every party: members, appenders, verifiers |
| ES module + JSON | tcc-js `src/js.c`, `src/json.c` | the evaluated program | BigInt in a JS prelude | public verification and the record as JSON only. JS is not constant time, so a program that touches a key share is refused on this target (new code `REFUSE_TARGET_SECRET`). |
| WGSL compute | tcc-webgpu `src/wgsl.c` | the entries | u32 limbs in WGSL | batch work: check many ballot proofs, rehash a long chain |

The kit HOST-LIMITs stay: no closure at run time
(`REFUSE_RUNTIME_CLOSURE`) and no fold over a tree at run time
(`REFUSE_RUNTIME_TREE`). A `Record` is a `FreeMon`, which is a tree. So the
Wasm form of a record at run time is the hash chain plus a flat array of
entries, and S8 must give a lowering for `FreeMon Entry` folds that are
linear (O8).

Differential tests: each target prelude runs the same vectors as the C
reference in the front end (`langc eval`). The kit already uses node for
`test/run-wasm.mjs`. That harness stays. No new node, Python or shell
harness: new tests are guest `.lang` files and C tools, as in the kit.

## 7. Agent adoption (A1 to A6)

The request asks for six items so that agents reach for cairn-lang when it
fits.

| Item | Deliverable | Gate |
|---|---|---|
| A1 | One canonical form for each operation: no aliases, no second syntax. `langc fmt FILE` prints the canonical form. `langc fmt --check` exits 1 when a file is not canonical. | `fmt` is idempotent; `parse (fmt x)` has the same core term as `x`; every example and test file is canonical. |
| A2 | `langc check --json FILE`: one JSON object with `diagnostics` (code, span, message, `fix`: a concrete edit) and `progress` (`parsed`, `typed`, `laws_discharged`, `laws_total`). Schema in `docs/CHECK-JSON.md`. | Rows in `test/json/` with the expected JSON, byte equal. Each refusal code has a `fix` row. |
| A3 | `SKILL.md` with five graded examples (G1 to G5 below). Each example is a file in `examples/skill/`, and SKILL.md quotes it. | The gate checks each example and checks that the SKILL.md block and the file are byte equal. |
| A4 | `langc mcp`: an MCP server over stdio (JSON-RPC 2.0, one message on each line), in C, built by TinyCC into the same `langc`. Tools: `check`, `fmt`, `eval`, `build` (with a target), `explain` (a refusal code). The MCP spec version is pinned in `docs/MCP.md`. | A C tool `test/mcptool.c` sends the request lines in `test/mcp/*.in` and compares the replies with `*.out`. |
| A5 | `AGENTS.md` (how an agent works in this repo, with the gate command) and a README table: when to use cairn-lang, when not to use it, and what to use then. | Review only. |
| A6 | Token counts: G1 to G5 in cairn-lang, Python and Rust. Report `docs/TOKEN-BASELINE.md` with the model id, the date, each count and each file hash. Tokenizer: the Anthropic `count_tokens` endpoint for the newest Claude model (Q-S0-8 (a)). The Python and Rust programs run with pinned libraries, and their output is equal to the cairn-lang output (Q-S0-9 (a)). | `make tokens`, not in `make check`: it needs an API key, the network, Python and Rust. |

Graded examples:

| Grade | Example | Uses |
|---|---|---|
| G1 | Append two digests and verify one | Record, append, head, verify (one member, threshold 1) |
| G2 | A 2-of-3 append committee | Auth, threshold Schnorr, refusal of a bad signature |
| G3 | One encrypted vote | Ballot, cast, tallyEnc, open, the L6 law |
| G4 | Amend `tAppend` from 2 to 3 | Proposal, decide, enact, reshare |
| G5 | Amend `tAmend` (the rule amends itself) | self-reference, `amend` as a fold, L9 |

## 8. Tests

The kit layout stays: `test/parse`, `test/check`, `test/eval`, `test/ir`,
`test/fronttool.c`, `test/review.sh`, `test/gate.sh`. The ports add the
intent-lang tools that stay (`test/elgamaltool.c`, `test/thresholdtool.c`
and the Sigma and bLSAG tools). New: `test/json/` (A2), `test/mcp/` and
`test/mcptool.c` (A4), `test/fmt/` (A1), the target vector rows (S8 to
S10). Mutation: each slice that adds a refusal or a law kills at least one
mutant of it, as in intent-lang.

## 9. Slices

| Slice | Work | Status |
|---|---|---|
| S0 | Seed from tcc-wasm, DESIGN.md verbatim, this plan, README; gate; seed commit; private repo | DONE e222eba |
| S1 | The USER rules section 11. Record the rulings here. | DONE (all (a)) |
| S2 | Port the intent-lang kernel at 67445ae (6.1). Survey first: SAME, CHANGED, INTENT-ONLY. Targets = the intent-lang counts less the EVM and swap rows. | DONE: `make -B check` EXIT 0; examples 53, parse refusals 19, check refusals 189, kit-only refusals 24, evals 215, proves 2, build refusals 1 (new), guest differential 28, runs 7, run refusals 11, 11 tool regressions passed. Each count is at its target (`cairn-lang-work/S2-survey.md`). |
| S3 | No time: remove the clock parts (4), add `REFUSE_TIME`, `OpPos` and `len`. | DONE: `make -B check` EXIT 0; examples 53, parse refusals 19, check refusals 195, kit-only refusals 24, evals 218, proves 2, build refusals 1, guest differential 28, runs 6, run refusals 11, time hint 1 (new), 11 tool regressions passed. Each count is at its target (`cairn-lang-work/S3-survey.md` section 4; examples 53 with the new `examples/oppos.lang`). Mutants KILLED: `epoch` out of `TIME_WORDS`; `fmEta` counts 2. `len` is a C builtin (section 4). |
| S4 | The group and the hash (Q-S0-2 (a), Q-S0-3 (a)): a group interface `group.h` for the Sigma, ElGamal, threshold and bLSAG code; the secp256k1 field and group; SHA-256 (FIPS 180-4 vectors); remove `bn254.c`. | DONE: `make -B check` EXIT 0; examples 53, parse refusals 19, check refusals 192, kit-only refusals 22, evals 216, proves 2, build refusals 1, guest differential 28, runs 6, run refusals 11, time hint 1, 11 interpreter regressions and 12 tool regressions passed (new: `shatool`). Each count is at its target (`cairn-lang-work/S4-survey.md` section 9). The drops are the ruled removals: check refusals 195 to 192 and kit-only refusals 24 to 22 (`base-ecpairing`, `pairing-list`, `base-ecrecover`), evals 218 to 216 (`recoverGood`, `recoverBad`). Mutants 2/2 KILLED: SHA-256 `K[0]` + 1 (`shatool`, 4 FAIL lines); the curve constant 7 to 5 (`secptool`, 3 FAIL lines). |
| S5 | Part R: Record, append, head, verify, Inclusion (hash chain); laws L1 to L5. | NEXT |
| S6 | Part D: Constitution, Ballot, cast, tallyEnc, open (the share that closes the ballot), decide, enact; the DKG and reshare; laws L6 to L9, V5. | |
| S7 | Threshold Schnorr for appends: FROST-style signing in two rounds under `keyAppend`. | |
| S8 | Wasm: lower the ported core to the IR, the crypto prelude, differential vectors. | |
| S9 | ES module + JSON: port `js.c` and `json.c`, the BigInt prelude, `REFUSE_TARGET_SECRET`. | |
| S10 | WGSL: port `wgsl.c`, batch proof check and chain rehash kernels. | |
| S11 | A1: canonical forms and `langc fmt`. | |
| S12 | A2: `langc check --json`. | |
| S13 | A4: `langc mcp`. | |
| S14 | A3 and A5: SKILL.md, G1 to G5, AGENTS.md, the README table. | |
| S15 | A6: the token comparison. | |
| S16 | Close: SPEC.md, probe/CAPABILITY.md, VALIDATION.md, final gate. | |

## 10. Open items

- O1 The kit base data is `Scalar`, `Point`, `Digest`. If a slice needs
  more, it records why here first.
- O2 `REFUSE_CELL` comes with the port (S2). The S2 gate must show the
  intent-lang refusal rows for it.
- O3 DONE (S3). The intent-lang `Env.block` (its O17) is removed, not
  ported: `Env` is `makeEnv (caller : Addr)`, and `REFUSE_TIME` refuses
  the time words.
- O4 The S2 port must not take `.kanon-exec/` or `.kanon-wait/` (they are
  untracked in intent-lang).
- O5 DONE (S4, Q-S4-6 (a)). The bLSAG domain tag is the bytes of "cairn"
  (`BLSAG_TAG` = 0x636169726e in `src/front/blsag.c`). The intent-lang tag
  was "orbit".
- O6 `REFUSE_TARGET_SECRET` (S9) needs a sound mark of the secret values:
  the shares, the nonces and the decryption key. S6 marks them in the
  types.
- O7 DONE (S4). The SHA-256 test vectors come from the published standard (FIPS
  180-4 and the NIST example values), not from another implementation
  that the gate would trust. The same holds for the secp256k1 constants
  (SEC 2). `test/shatool.c` has "abc", the 448-bit message and one million
  "a" from FIPS 180-2 Appendix B (the NIST example file that FIPS 180-4
  points to), and the empty string from the NIST CAVP file
  `SHA256ShortMsg.rsp` (Q-S4-5 (a)). `test/secptool.c` has G and n as the
  SEC 2 version 2 section 2.4.1 words, and checks `group_gen` against them.
- O8 A linear fold over `FreeMon Entry` at run time (S8). The kit refuses
  a run-time fold over a tree; a record is a tree with one spine. Either
  the lowering accepts the linear case, or the record lowers to an array
  and a count.
- O9 The pinned MCP spec version and the JSON-RPC subset (A4) go in
  `docs/MCP.md` at S13.
- O10 The `fix` of each refusal code (A2) is one concrete edit. A code
  with no edit gives `fix: null` and a reason.
- O11 DONE (S4). `test/ballottool.c` now uses `group.h` and SHA-256, and the
  swap vectors are gone. The S2 text:
  (S2) `test/ballottool.c` keeps the swap vectors of intent-lang
  (`fill_intent`: `intentHash`, `replayKey`, `cipherDigest`; its comments
  name `intent.lang` and `intentsig.lang`, which S2 removed). S2 did not
  edit the tool. S4 rebuilds the tool on `group.h` and SHA-256, and removes
  the swap vectors then.
- O12 (S2) The seed example `examples/inventory.lang` uses the seed sample
  domain (`Color`, `Item`, `Stack`, `Lot`). Only `domain/domain.lang` can
  declare a family, so the example cannot check under the ported domain.
  S2 moved it, byte for byte, to `examples/seed/inventory.lang`, out of the
  `examples/*.lang` rows. `domain/README.md` still describes the seed
  domain. S5 writes a Part R example and the domain README, and removes
  both seed files.
- O13 (S2) Out of the build until S8: `src/front/lower.c`,
  `src/front/lower.h`, `src/wasm.c`, `src/ir.h` (the e222eba copy),
  `test/ir/*.lang` (5 rows), `test/run-wasm.mjs`, `test/grid.awk` and the
  kit row `test/check/domain-name.lang` (no expect line). S8 puts each back
  in the gate or removes it.
- O14 (S2) `langc build` and `langc abi` refuse with `REFUSE_LOWER`
  ("target pending S8"). Only `build` has a gate row. `-o`, `--runtime` and
  `abi --text` still parse, and the `Options` fields `out_path`, `runtime`
  and `json` have no use until S8. `langc contract` lived in the removed
  `src/front/writer.c`, so it is now an unknown command (exit 2).
- O15 (S3, Q-S3-2 (a)) The kit prelude keeps `Out` (`pay`, `pull`,
  `emit`) of intent-lang. Off chain there is no token. S5 (Part R)
  replaces `Out` with the record append, and `pay` and `pull` leave then.
- O16 (S3) `len` is a C builtin (section 4). If a later slice gives the
  domain `isSet Nat` and the `natAdd` paths, `len` becomes a domain fold
  over `fmFold`, and `OP_FM_LEN` goes.
- O17 (S4) `src/front/lower.c` does not compile with clang: `OP_EQ` and
  `OP_REFL` are not declared in the ported `core.h`. This was so before S4.
  The file is out of the build (O13), so the gate does not see it. S8
  adapts the file (6.1).

## 11. Questions for the USER (S0)

Each question has a recommendation (rec). S1 records the rulings: the
USER took each rec on 2026-10-25 ("Take recommendations"), so all nine
are RULED (a).

- Q-S0-1 FHE scheme. RULED (a). (a) rec: additive only (exponential ElGamal, reused
  from intent-lang). It is the minimum for a sum, but the opened tally
  shows the yes count. (b) A lattice FHE scheme (for example TFHE), so that
  `decide` runs under encryption and only the decision bit opens. This is
  much more code, and the WGSL target then also carries the NTT.
- Q-S0-2 Group. RULED (a). (a) rec: secp256k1. Prime order, about 128-bit security,
  but the intent-lang crypto code calls the BN254 `g1_*` API, so S4 adds
  a group interface and a secp256k1 field. (b) BN254 G1, reused as is. Its
  pairing (embedding degree 12) moves a discrete log in G1 into F_p^12,
  where the number field sieve attacks give about 100 bits. Off chain,
  there is no precompile that forces BN254. (c) Ristretto255: prime order,
  about 128 bits, all new code.
- Q-S0-3 Hash. RULED (a). (a) rec: SHA-256. Content hashes are usually SHA-256
  (sha256sum, git SHA-256 mode, IPFS). (b) keccak-256, reused from
  intent-lang.
- Q-S0-4 Ballot close with no clock. RULED (a). (a) rec: the `tOpen`-th valid share
  closes the ballot (section 4). (b) A ballot closes after `k` later
  operations of the record (`k` in the constitution).
- Q-S0-5 Append authorization. RULED (a). (a) rec: one threshold Schnorr signature
  (FROST style, two MPC rounds) under `keyAppend`, a key apart from
  `keyOpen`. The signers stay hidden, and the check is one signature. (b)
  `tAppend` plain Schnorr signatures, one for each signer. No MPC, and the
  record shows who signed.
- Q-S0-6 Inclusion proofs. RULED (a). (a) rec: the hash chain only. A proof is the
  chain suffix, so its size grows with the number of later entries. (b) A
  Merkle mountain range: proofs of log size, more code and more laws.
- Q-S0-7 Key setup. RULED (a). (a) rec: a Pedersen DKG with Feldman commitments and a
  Schnorr proof of knowledge for each dealt secret (as in FROST KeyGen), and
  the same for reshare. Note: this DKG lets a member bias the public key
  (Gennaro et al.); the plan records that as a VALIDATION note. (b) The
  intent-lang trusted dealer, which puts one trusted party in the system.
- Q-S0-8 Tokenizer for A6. RULED (a). (a) rec: the Anthropic `count_tokens` endpoint
  for the newest Claude model, with the model id and the date in the
  report. The agents that this targets read Claude tokens. It needs an API
  key and the network, so it is a manual step. (b) A pinned offline
  tokenizer (tiktoken `o200k_base` with a vocabulary hash). (c) Both.
- Q-S0-9 Python and Rust programs for A6. RULED (a). (a) rec: they run, and their
  output is equal to the cairn-lang output, with pinned libraries. A short
  wrong program cannot then win the count. (b) Text only, not run.
