# cairn-lang

cairn-lang is a domain-specific language. It comes from lang-template with
the tcc-wasm host.

cairn-lang is for one job: an append-only public record of content hashes,
governed off chain by a threshold DAO. The DAO encrypts its ballots (FHE),
proves each step (ZK), opens only by threshold (MPC), and can amend its own
rules. There is no clock: time is the number of operations applied.

- Type constructors: only cell monads and their duals, in Cubical TT.
- Host: TinyCC. `build/langc` is C99.
- Targets: Wasm MVP (primary), ES module + JSON, WGSL compute.
- `langc build PROG -o OUT.wasm` writes Wasm, `langc build PROG --js
  OUT.mjs` writes an ES module, and `langc abi PROG` prints the JSON abi
  (`--text` prints the Wasm layout). `build/jstool` runs an ES module on
  node.

Status: `docs/PLAN.md` gives the slices and the state of each slice.

## When to use cairn-lang

| Task | cairn-lang? | Use instead |
| --- | --- | --- |
| An append-only record of content hashes with inclusion proofs (G1) | yes | |
| A threshold committee that signs each append (G2) | yes | |
| An encrypted vote with a threshold tally (G3) | yes | |
| A rule change (G4) | yes | |
| A change of the rule that changes the rules (G5) | yes | |
| A token or a general contract | no | a smart contract language, for example Solidity |
| A wall-clock deadline (there is no clock) | no | a contract or a service that reads a clock |
| State with deletes or edits | no | a database |
| General FHE | no | an FHE library, for example OpenFHE or TFHE-rs |
| General ZK circuits | no | a circuit language, for example Circom or Noir |

`skills/cairn-lang/SKILL.md` gives the examples G1 to G5 (the files of
`examples/skill/`). `AGENTS.md` gives the loop and the commands for an
agent.

## Files

- `docs/PLAN.md`: the readings of the design, the route, the slices and
  the open questions.

- `SPEC.md`: the specification. Fill each section.
- `design/DESIGN.md`: the domain design. It is the only source of the core
  types and the core operations.
- `formers/FORMERS.md`: the type formers. `formers/tcc-wasm.md` gives their
  status on the tcc-wasm host.
- `probe/CAPABILITY.md`: the host probe.
- `docs/STATUS.md` and `docs/VALIDATION.md`: the status and the gate results.
- `docs/host/README.md`: how to build and test the host kit.
- `domain/README.md`: what the domain holds and how to change it.
