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

Status: S0 seed. The kernel, the domain and the targets come in the
slices of `docs/PLAN.md`. A table that tells when to use cairn-lang comes
in slice S14, with `AGENTS.md` and `SKILL.md`.

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
