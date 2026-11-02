# cairn-lang specification (draft)

Status: M0 to M2 DONE, M3 PARTIAL (section 10). `cairn-lang` is a working name.

`docs/PLAN.md` holds the rulings and the slices. `docs/VALIDATION.md`
holds the evidence.

## 1. Purpose

cairn-lang is a language for one off-chain system with two parts. The
first part is an append-only public record of content hashes. The second
part is the DAO that governs the record. The DAO is threshold-authorized
and amends its own rules. The two parts use zero knowledge (ZK),
homomorphic encryption and multi-party computation (MPC). No part uses a
blockchain.

A program declares the values of the record and of the DAO, and the laws
that relate them. A law is a term of a `Path` type. The compiler `langc`
checks the program, its types and its laws, and then writes the target.

The host is TinyCC. All of `langc` is C99 that TinyCC builds, `langc mcp`
too (`docs/PLAN.md` section 1). The targets are Wasm MVP (the tcc-wasm
kit, the primary target), an ES module with JSON (the tcc-js kit) and WGSL
compute (the tcc-webgpu kit). Each build writes one target: `-o` for Wasm,
`--js` or `--wgsl` (`probe/CAPABILITY.md`, Consequences for M0, item 5).
Native code is not a target.

Its type formers are F1 to F15 of `formers/FORMERS.md`. Its core data types
and core operations are only the types and operations of
`design/DESIGN.md` (the design).

The base choices are fixed (`docs/PLAN.md` section 1, the S1 ruling). The
host cryptographic code implements additive exponential ElGamal on
secp256k1, distributed key generation (DKG) and resharing. The guest
`Enc`, ballot and quorum types are an ideal model (sections 5 and 6).
Using those types does not invoke the host protocols. The hash is
SHA-256. A program has no clock and no block height (`docs/PLAN.md`
section 4).

The tools for agents are in `docs/PLAN.md` section 7 (A1 to A6):

- `langc fmt` prints the one canonical form of a program.
- `langc check --json` gives fix hints and the progress counts (parsed,
  typed, laws discharged).
- `langc explain` and `langc mcp` (an MCP server in C99) serve agents.
- `SKILL.md` has graded examples. `AGENTS.md` and the README table say
  when to use the language.
- `make tokens` checks the Python and Rust outputs before requesting API
  token counts. S15 ran the output checks offline (10 of 10); the API
  part was not run because no key was present. The report says so.

## 2. Programs

A program is one file, with no `import`. It is a sequence of definitions:

```
def NAME : TYPE := TERM
```

A definition can use the core names, the prelude names and the
definitions before it (`probe/CAPABILITY.md`, Consequences for M0, items 1
and 2). Thus a program has no general recursion. Recursion comes only from
`fold` and `unfold` (F6, F7). The domain needs no fixed first definition:
a size is a term of the domain family `Size` (`sz0`, `szS`,
`domain/domain.lang:57`).

The compiler refuses these host forms in a program (`formers/tcc-wasm.md`):

- A `family` declaration gives `REFUSE_DATA` (rule R1, `data.lang`).
- An `axiom` gives `REFUSE_AXIOM` and exit 1 (rule R2, `axiom.lang`).
- A use of the definition itself or of a later one gives `REFUSE_REC`
  (rule R3, `rec.lang`, `self.lang`, `later.lang`).
- A core name, a domain name, a reserved name or a name that is already
  defined gives `REFUSE_NAME` (rule R4, `core-name.lang`,
  `domain-name.lang`, `reserved.lang`, `duplicate.lang`).

The domain adds these refusals:

- A kit type, kit operation or kit literal (`Nat`, `U256`, `Addr`, `Flag`,
  `Map`) without `--kit` gives `REFUSE_KIT` (22 mutants).
- The kit cons list `List` in a guest program gives `REFUSE_CELL`
  (`guest-list.lang`). A guest program uses `FreeMon`.
- A state that is not the one state of the design gives `REFUSE_STATE`
  (two states, a self reference, a function field, a history flag).
- A history write that is not the append gives `REFUSE_HISTORY_WRITE` (10
  mutants: set, sub, swap, init, partial, shadow, closed, hidden).
- A use of time (`now`, `block`, a timestamp, a clock, an epoch) gives
  `REFUSE_TIME` (6 mutants).
- A bad event field (`event-field.lang`, `event-field-name.lang`,
  `event-max.lang`) gives `REFUSE_EVENT`.
- A map key or map value that is not admitted gives `REFUSE_MAP`.
- A term that leaks a secret gives `REFUSE_LAW` (9 mutants: an encrypted
  value in a fold, a decrypt, an annotation, a projection or a relay, and
  a leak of an open, a ZK proof or a tally).
- A signature that the design does not give gives `REFUSE_SIG` (12
  mutants: a defined or primitive slot, a slot universe, an act type, an
  early act, an orbit definition in guest mode).

The lowering adds `REFUSE_RUNTIME_TREE` and `REFUSE_RUNTIME_CLOSURE`
(HOST-LIMIT, section 3), `REFUSE_LOWER` and `REFUSE_TARGET_SECRET`.
`test/check/expect.txt` gives the code of each refusal mutant in
`test/check/` (197 rows). The compiler writes the target; a program
cannot.

## 3. Type formers

The type formers are F1 to F15. `formers/tcc-wasm.md` gives the status of
each former on the primary target, Wasm MVP (rows :14 to :28), with the
`src/front/check.c` and `src/front/lower.c` lines. `formers/tcc-js.md` and
`formers/tcc-webgpu.md` give the other 2 targets. Rule R1 admits only cell
monads (CM) and their duals (CC) as type constructors: a CM has `eta` and a
`bind` that is the recursor, and a CC has `eps` and an `extend` that is the
corecursor (`docs/PLAN.md` section 2). Each other parameterized declaration
gets `REFUSE_CELL`. F1, F2, F3, F11, F12 and F13 are DONE with no
deviation. The table gives each other former.

| ID | Status on tcc-wasm | Effect on this language | Open item |
|---|---|---|---|
| F4 | DONE as `FreeMon`. HOST-LIMIT: a tree at run time (`formers/tcc-wasm.md:17`, :30) | `FreeMon` (`domain/domain.lang:113`) is the list of the language. A run-time fold over a linear spine lowers (one side of each `fmMul` is a static leaf). A run-time fold over a tree gives `REFUSE_RUNTIME_TREE`. | O8, O21 |
| F5 | DONE over `Option`, `Sum E` and `List` (:18) | A guest uses `FreeMon`, not `List`: a cons-list name gives `REFUSE_CELL`. The domain gives `fmMap` and `fmBind` on `FreeMon` (section 4). | O2 |
| F6 | DONE over `Nat`, `List` and the domain families. HOST-LIMIT: a tree at run time (:19, :30) | A run-time fold over a tree gives `REFUSE_RUNTIME_TREE`, also at the elim limit (the row `test/ir/tree-elim.lang`, over `Word`). `len` is a domain fold over `fmFold`. 2 lowering paths have no value row: `bin_case` and the `cases_of` site of `family_fold`. | O8, O16, O25, O26 |
| F7 | DONE into `List`, with a `Nat` step limit (:20) | A guest cannot name `List` (`REFUSE_CELL`). The guest corecursion is the `extend` of the cocell comonad `Stream` (`domain/domain.lang:64`). | O2 |
| F8 | DONE over `Option` and `List` (:21) | A guest uses `filter` on `Option`. The `List` form needs a cons-list name (`REFUSE_CELL`). | O2 |
| F9 | DONE. HOST-LIMIT: a function value at run time (:22, :39) | A lambda stays only as the direct function argument of an eliminator. A function value that must exist at run time gives `REFUSE_RUNTIME_CLOSURE`. | O21 |
| F10 | DONE over `Type 0` and over `Nat` (:23) | A dependent function takes a type in `Type 0` or a `Nat`. Pi is not a builtin row, so the row cites no `check.c` line. | O21 |
| F14 | DONE for `Type 0` and `Type 1`, without cumulativity (:27) | A term of `Type 0` is not also a term of `Type 1`. | None |
| F15 | PARTIAL: only the domain file declares an indexed family (:28, :44) | A `family` in a program gives `REFUSE_DATA`. A def with scalar parameters and a family, product or list result has no target form: `langc ir` and `langc build` write a note on stderr. | O21, O24 |

The open item ids are the ids of `docs/PLAN.md` section 10, and section 9
uses the same ids. O25 has no owner slice. O2 has no DONE mark, but the
gate has its `REFUSE_CELL` row (`guest-list`). The other items are DONE.

## 4. Structures

- **Monad**: `pure`, `map` and `bind` on `Option`, `Sum E` and `List` (F5,
  `formers/tcc-wasm.md:18`). A guest uses `FreeMon`, not `List`: a
  cons-list name gives `REFUSE_CELL` (the gate row `guest-list`). The
  domain gives `fmBind` and `fmMap` on `FreeMon`
  (`domain/domain.lang:132`, :135), and the bind is the fold `fmFold`
  (:129). The domain also gives `encBind`, `encMap` and `encOp2` on `Enc`
  (the note at :229): `encBind` is the fold, and `encMap` and `encOp2` are
  binds.
- **Algebra**: `fold` on `Nat`, `List` and each domain family (F6, :19),
  and `fmFold` on `FreeMon`. `unfold` runs into `List` with a `Nat` step
  limit (F7, :20), but a guest cannot name `List`. The guest corecursion is
  the cocell comonad `Stream` (`cofamily Stream`, `domain/domain.lang:64`):
  its `extend` is the corecursor (rule R1).
- **Filterable**: `filter` on `Option` and `List` (F8, :21). A guest uses
  the `Option` form.

The host gives the carriers `Nat`, `Option`, `Sum E` and `List`, and a
guest cannot name `List`. The carriers that the host does not give are the
16 families of `domain/domain.lang`: `S1` (:38), `Trunc1` (:43), `Trunc0`
(:49), `Size` (:57), `Parity` (:85), `Empty` (:92), `Bag` (:101),
`FreeMon` (:113), `Inclusion` (:210), `Quorum` (:249), `Auth` (:320),
`Pos` (:371), `Word` (:378), `ChoiceBag` (:401), `OrbitN` (:406) and
`Charter` (:418), and the cofamily `Stream` (:64). A program cannot add a
family (`REFUSE_DATA`, F15).

## 5. Core types

| Type | Meaning (PLAN section) | Definition |
|---|---|---|
| `Entry` | 1 entry of the record: a digest (3.1) | `Digest` (`domain/domain.lang:163`) |
| `Record` | A free monoid of entries (3.1). `snoc` and the `FreeMon` constructors also produce records; `append` adds the signature check | `FreeMon Entry`, a cell monad (:165) |
| `Auth` | 1 FROST threshold Schnorr signature of the append committee on (`head r`, e) under `keyAppend`. The kernel builtin `schnorrOk` checks it (3.1) | `family Auth := authOf (authR : U256) (authZ : U256)` (:320) |
| `Pos` | The position of an entry: the `Nat` that `len` gives (3.1) | `OpPos` = `Nat` (:140), the result of `verify` (:198). The indexed `family Pos : Size -> Type 0` (:371) is a different family |
| `Inclusion` | The chain suffix from a position to the head (3.1) | `family Inclusion := inclusionOf (inclBefore : Digest) (inclSuffix : Record)` (:210) |
| `Member` | 1 member: a point (3.2) | `Point` = `U256` (:279) |
| Constitution (`Polity`) | The members, the thresholds `tAppend`, `tAmend` and `tOpen`, and the keys `keyOpen` and `keyAppend` (3.2) | `Prod (FreeMon Point) (Prod Nat (Prod Nat (Prod Nat (Prod Point Point))))` (:294) |
| `Proposal` | The full new constitution (3.2) | `Polity` (:328) |
| Ballot (`SealedBallot`) | An ideal encrypted verdict, a truncated bit witness and a caller-supplied link tag (3.2). This type contains no key, Chaum-Pedersen transcript or bLSAG signature | `Sigma (c : Enc Verdict) (Prod (ZKProof (Bit c)) Digest)` (:333), with `Bit` (:330) and `Verdict` = `Flag` (:189) |
| `Box` | The ballots of 1 proposal (3.2) | `FreeMon SealedBallot` (:342) |
| `Enc A` | Encryption: the identity cell monad behind a sealed eliminator. Eta is ideal encryption, and bind is homomorphic evaluation (3.3) | `sealed family Enc (A : Type 0) := encEta (encOf : A)` (:231) |
| `ZKProof W` | The ideal proof truncation `squash W`, a cell monad (3.3). It holds a witness rather than a serialized cryptographic proof | `Trunc1 W`, a def and not a family (:261, the note at :254) |
| `Quorum` | The public sizes t and n of an ideal opening (3.2, 3.3). Its constructor carries no shares or threshold proof | `family Quorum := quorum (quorumT : Size) (quorumN : Size)` (:249) |
| `Share` | 1 host partial decryption with a DLEQ proof (3.2) | Host C `ThrPart` (`src/front/threshold.h`). The domain has no `Share` definition |

The kit base data (O1) are `Scalar`, `Point` and `Digest`, and `Nat` stays
in the kernel. The kit has no chain data: no `U256`, `Addr`, `Env`, `now`,
`block`, `caller` or `Out` (PLAN :124 to :128). But the domain defines
`Digest` and `Point` as `U256` (`domain/domain.lang:161`, :279), and the 2
fields of `Auth` are `U256` (:320). The 2 statements agree: `U256` is only
the domain carrier of these types, and a guest names `Digest` or `Point`,
never `U256` (the note at :159). `Count` is `Nat` (:281).

## 6. Core operations

The operations are defs of `domain/domain.lang`, and PLAN section 3 gives
their meaning. The domain names of PLAN :179 apply: `amend` is `amendAll`,
`cast` is `castBallot`, `tallyEnc` is `tallyBox`, `count` is `countVotes`
and `decide` is `decideYes`. `decrypt`, `observe` and `open` are law defs:
they are law-only (3.3).

The guest model and the host protocols have separate entry points.
`mkBallot` can construct a ballot from any verdict and tag. `castBallot`
checks only tag reuse and does not use its proposal argument; it does not
verify membership, a ring signature or a cryptographic bit proof.
`open` ignores its quorum argument, and `enact` returns the proposal
without resharing. Host callers must run the cryptographic protocols
explicitly: `src/front/elgamal.c`, `sigma.c`, `blsag.c`, `threshold.c` and
`dkg.c` implement them. `examples/daokit.lang` shows explicit kit
verification. Its `enactKit` is a separate operation, not an alias of
`enact`. Record consumers must use `append` to check authorization;
constructing a `Record` with `snoc` does not authenticate it.

| Operation | Type | Meaning (PLAN section) |
|---|---|---|
| `append` | `Polity -> Auth -> Entry -> Record -> Option Record` (`domain/domain.lang:325`) | Add 1 entry when `Auth` holds. It is 1 def over `snoc` (:167), with no delete and no update (3.1) |
| `head` | `Record -> Digest` (:184) | The chain head: `head (fmUnit Entry) = hash tagGenesis 0u` and `head (snoc e r) = H3 tagAppend (head r) e`, where `H3 t d e = hash (hash t d) e` (3.1) |
| `verify` | `Entry -> Record -> Option OpPos` (:198) | The position of an entry in a record, or none (3.1) |
| `verifyAt` | `Entry -> Inclusion -> Digest -> Verdict` (:221) | Check an entry against an inclusion and a head (3.1) |
| `len` | `(A : Type 0) -> FreeMon A -> Nat` (:155) | The length of a free monoid. It gives the position (3.1) |
| `castBallot` | `Proposal -> SealedBallot -> Box -> Option Box` (:347) | Add 1 ballot unless its link tag is already used; the proposal argument is unused (3.2) |
| `tallyBox` | `Box -> Enc Count` (:350) | The homomorphic sum of the ballots (3.2) |
| `countVotes` | `FreeMon Verdict -> Count` (:353) | The count of the yes votes (3.2) |
| `decideYes` | `Polity -> Count -> Flag` (:358) | Yes when the count is at least `tAmend` (3.2) |
| `enact` | `Proposal -> Polity -> Polity` (:360) | Return the proposal, ignoring the old polity. It performs no reshare (3.2) |
| `stepD` | `Polity -> Decided -> Polity` (:362) | 1 step of the amend chain on a decided proposal (3.2) |
| `amendAll` | `FreeMon Decided -> Polity -> Polity` (:365) | The amend chain: a fold over the decided proposals (3.2, PLAN :173) |
| `decrypt` | `(A : Type 0) -> Enc A -> A` (:242) | The counit `Enc -> Id`. It is law-only (3.3, the note at :230) |
| `observe` | `(A : Type 0) -> Enc A -> Trunc1 A` (:245) | `squash` after `decrypt` (3.3) |
| `open` | `(A : Type 0) -> Quorum -> Enc A -> A` (:251) | The law-only ideal opening is `decrypt A c`; it ignores the quorum. Host `thr_open` checks the shares and interpolates (3.2, 3.3) |
| `encMap` | `(A : Type 0) -> (B : Type 0) -> (A -> B) -> Enc A -> Enc B` (:236) | A map under `Enc`. It is a bind (3.3) |
| `encBind` | `(A : Type 0) -> (B : Type 0) -> Enc A -> (A -> Enc B) -> Enc B` (:233) | Homomorphic evaluation. It is the fold of `Enc` (3.3) |
| `encOp2` | `(A : Type 0) -> (B : Type 0) -> (C : Type 0) -> (A -> B -> C) -> Enc A -> Enc B -> Enc C` (:239) | A 2-argument operation under `Enc`. It is a bind (3.3) |
| `schnorrOk` | Kernel builtin, `IR_CALL_SCHNORR_OK` on the JS target (`src/js.c:210`) | Check 1 Schnorr signature: the `Auth` check of `append` (3.1). The JS prelude ports `schnorr_verify` of `src/front/frost.c` (`src/js.c:19`) |

Some parts are host C, not guest code (PLAN 3.2, Q-S6-4 (a)). `Deal` and
`Reshare` (a Pedersen DKG with Feldman commitments and a Schnorr proof of
knowledge) are in `src/front/dkg.c`. The FROST signing (S7) is in
`src/front/frost.c`: the RFC 9591 2-round shape, x-only keys, identifiable
abort and cairn tag chains, with no RFC 9591 vectors.

Laws (PLAN 3.3). The MPC law is `open q c = decrypt c`: the body of `open`
is `decrypt A c` (`domain/domain.lang:252`). The TCM law of the tally is
`open q (tallyEnc bs) = count (map decrypt bs)`; the gate proves it only
for the closed boxes of O22, not for an arbitrary box. Model privacy comes
from `squash`: `observe c = observe c'` for each 2 ciphertexts `c` and `c'`,
because `observe` gives a `Trunc1` value. Guest `encBind` and `encMap`
accept arbitrary well-typed functions in the ideal model. The host
exponential ElGamal scheme supports affine homomorphic evaluation only
(Q-S0-1 (a)); the compiler does not translate every guest bind into that
scheme. IND-CPA is an assumption of `docs/VALIDATION.md`, not a consequence
of the truncation law. `langc eval` can print the plaintext and witness of
an ideal value, so these guest values are not encrypted wire formats.

## 7. Target and instance encoding

The compiler has 1 front end, the ported kernel, and 3 writers that read
its first-order IR (PLAN 6.2). A target entry has scalar parameters and a
scalar result (`Nat`, `Flag` or `U256`, including aliases). A top-level
data definition need not have an exported target form. Family results
are omitted, with the O24 note for functions with scalar parameters.

- **Wasm MVP (primary)**: `src/wasm.c` writes a module from the
  first-order IR (`src/front/lower.c`, `src/ir.h`). The module has a
  prelude of Wasm functions: field and scalar arithmetic on i64 limbs,
  point add and multiply, and the hash. It runs public computations;
  real key shares and secret protocol steps stay in host C (V7 and V8).
- **ES module and JSON**: `src/js.c` writes the ES module from the IR
  (S9), with a BigInt prelude. `langc abi` writes the JSON entry interface;
  record serialization as JSON remains open (O27). JS is not constant
  time. A secret operation that remains at run time gets
  `REFUSE_TARGET_SECRET`, as on the other targets. Closed test-key
  operations can reduce to public constants (V8; `src/front/codes.c:27`).
- **WGSL compute**: `src/wgsl.c` reads the entries, with u32 limbs. This
  target is for batch verification and hash-chain work. The Metal backend
  rejects 20 entries in 4 modules, including `examples/ballot.lang`;
  `test/wgsl/internal.txt` lists them (O31, V15).

The kit HOST-LIMITs stay: no closure at run time
(`REFUSE_RUNTIME_CLOSURE`) and no fold over a tree at run time
(`REFUSE_RUNTIME_TREE`). A `Record` is a `FreeMon`, which is a tree. A
linear fold over `FreeMon Entry` lowers when one side of each binary node
is a static leaf (O8, S8). Internal family values
use tagged heap blocks (`src/ir.h`, `src/front/lower.c:690`), including
the linear spine representation. There is no exported record array ABI.

| Type | Encoding |
|---|---|
| `Point`, `Digest` | 1 `U256` word (section 5) |
| Field and scalar values | i64 limbs (Wasm), BigInt (ES module), u32 limbs (WGSL) |
| `Record` | Internal tagged heap blocks for supported folds. No exported family result (O24), and no JSON record format (O27) |
| A type, a universe, an index, the content of a proof | Erased on Wasm (`formers/tcc-wasm.md:10`, :52) |

A proof erases: `ZKProof W` is `Trunc1 W` (`domain/domain.lang:261`), and
the content of a proof erases on Wasm (`formers/tcc-wasm.md:10`). `verified`
is a law def that truncates the witness of an ideal `Transcript`; it does
not verify a serialized cryptographic transcript at run time. Host
cryptographic verifiers report their own failures.

The differential tests run the same vectors in each target prelude and in
the C reference (`langc eval`). The C tool `test/wasmtool.c` (the wasmtime
C API, Q-S8-7) runs each module.

## 8. Host and target

The host is TinyCC: `tcc -v` gives 0.9.28rc (mob@0fb54300, AArch64
Darwin). The repo has a `PIN` file (7 lines). It records the `tcc` version
and path, `node` v23.10.0, and `wasm-validate` and `wasm-objdump` 1.0.39,
from `/opt/homebrew/bin`. It also records the kit host check `make -C
hosts/tcc-wasm check` GREEN on 2026-10-08. No gate reads `PIN`.

The targets are Wasm MVP (primary), an ES module with JSON, and WGSL
compute (section 7). `probe/CAPABILITY.md` (S16) holds the host probe. The
host facts of the design come from the former rows F1 to F15 of
`formers/tcc-wasm.md` (section 3) and from the kit HOST-LIMITs (section 7).

The checks on the output are in `make check` (the header of
`docs/VALIDATION.md`). The C tool `test/wasmtool.c` runs each Wasm module,
and the differential rows compare each target prelude with the C reference
`langc eval` on the same vectors (PLAN 6.2).

## 9. Open items

The open items are the O rows of `docs/PLAN.md` section 10 (:410 to :637,
O1 to O37). Each row gives its slice and its ruling. PLAN section 11 gives
each ruling with its USER date. 17 rows have a DONE mark: O3, O5 to O8,
O11 to O18, O20, O21, O24 and O26. The 20 rows below have no DONE mark.
O25 has no owner slice.

- O1. The kit base data is `Scalar`, `Point` and `Digest` (:410).
- O2. `REFUSE_CELL` comes with the port (S2). Its gate row is `guest-list`
  (:412).
- O4. The S2 port must not take `.kanon-exec/` or `.kanon-wait/` (:417).
- O9. The pinned MCP spec version and the JSON-RPC subset of A4 (:450).
- O10. The `fix` of each refusal code (A2) is 1 concrete edit (:452).
- O19. (S5) The laws L4 and L5 for a variable entry `e` need `u256Eq e e ≡
  1` (:525).
- O22. (S6, Q-S6-3 (a)) L6 holds only on some closed boxes (:553).
- O23. (S7, Q-S7-9 (a)) The message `(head r, e)` that `langc run` prints
  (:559).
- O25. (S8) 2 lowering paths have no value row, for example `bin_case`
  (:572).
- O27. (S9, Q-S9-5 (a)) The record as JSON (:586).
- O28. (S9, Q-S9-7 (a)) The note at `src/front/sigma.h:19` (:589).
- O29. (S9, Q-S9-7 (a)) The note at `src/front/core.h:382` (:594).
- O30. (S9, Q-S9-9 (a)) A secret operation that is stuck at run time (O6)
  (:598).
- O31. (S10, Q-S10-r13 (b)) The Metal backend of WebGPU refuses 20 entries
  (:606).
- O32. (S10, Q-S10-r14 (a)) The WGSL functions `u_mulmod`, `u_pow` and the
  others of :611.
- O33. (S12, Q-S12-2 (a)) The `span` of `langc check --json` for a check
  refusal (:618).
- O34. (S12, Q-S12-4 (a)) A fix of `langc check --json` is 1 mechanical
  edit (:623).
- O35. (S13, Q-S13-9 (a)) `langc mcp` gives tools only (:629).
- O36. (S13, Q-S13-4 (a)) Each tool of `langc mcp` reads its program
  (:633).
- O37. (S14, Q-S14-5 (a)) 3 mutants of the skill gate are LIVE (:637).

## 10. Milestones

The plan maps its slices to the kit milestones by content (`docs/PLAN.md`
section 9). The agent slices S11 to S15 (A1 to A6) have their own row.

| Milestone | Slices | Content | Commits (date) | Status |
|---|---|---|---|---|
| M0 | S0 to S3 | Host probe (`probe/CAPABILITY.md`); the seed and the gate; the port of the intent-lang kernel at 67445ae; no time (`REFUSE_TIME`, `OpPos`, `len`); the core types construct and check; refusal list; examples and tests | e222eba, e120933 (2026-10-25); 6a0c223, d076765 (2026-10-26) | DONE (S16 fills the probe file) |
| M1 | S4 to S7 | Core operations: the group and the hash (secp256k1, SHA-256), Part R (the record hash chain), Part D (the DAO and the DKG), the threshold Schnorr appends | 06f4b8a, 49d02f3, 6748510 (2026-10-26); 0743108 (2026-10-27) | DONE |
| M2 | S8 to S10 | Reads and targets: Wasm with the crypto prelude, the ES module with the JSON ABI, WGSL compute | 024c4cd (2026-10-28), 6b73467 (2026-10-29), 4569c92 (2026-10-30) | DONE |
| A | S11 to S15 | Agent work: `langc fmt` (A1), `langc check --json` (A2), SKILL.md (A3, A5), `langc mcp` (A4), the token comparison (A6) | 3717b81 (2026-10-30); 98d18ef, 60768e8 (2026-10-31); 2df0354, 0175ef1 (2026-11-01) | DONE |
| M3 | none | Hardening: speed, limits, a checked certificate for the output | none | PARTIAL: the refusal codes and the mutant rows give the limits; no speed work, no checked certificate |
| Close | S16 | SPEC.md, `probe/CAPABILITY.md`, `docs/VALIDATION.md`, the final gate | staged, not committed | DONE |

Status 2026-11-01 (S16 closed, staged): M0 to M2 and the agent work
are DONE, and M3 is PARTIAL. The plan is CLOSED.
