# cairn-lang domain design

> Paste the domain design below the line, verbatim. Do not edit it.

This file is the only source of the core types and the core operations of
cairn-lang. `SPEC.md` sections 5 and 6 cite its sections. Do not add a core
type or a core operation that this file does not give.

When the design changes, paste the new version. Then record the change as an
open item in `SPEC.md` section 9.

---

## The request (USER, 2026-10-25, verbatim)

One change only: the paste had one trailing space after "criteria:". That
space is not here, because `git diff --check` refuses trailing white space.

```text
I want to write a programming language with the following characteristics:

For type constructors/higher-kinded types, admit only cell monads and their duals in a  locally presentable locally cartesian closed \((\infty,1)\)-category, as realized computationally in Cubical Type Theory.

For base types and operations, admit only what is necessary in order to implement an off-chain (no blockchain) ZK+FHE+MPC protocol for an append-only public record of content hashes, governed by an off-chain ZK+FHE+MPC threshold-authorized, self-amending system (the off-chain equivalent of a DAO).  In this setting, the usual concepts of "time" are prohibited: time can only be introduced in terms of successive applications of one or several operations.

The host language shall be TinyCC.  Use your judgment to provide a multiple-choice option for compilation target(s).

Finally, I would like agents to reach for this language when it fits their use cases.  To that end, also add the following criteria:

  The plan for making agents reach for the language includes:
  - one canonical form per operation and a langc fmt canonical printer;
  - langc check --json, giving fix hints and progress counts (parsed, typed, laws
    discharged);
  - a SKILL.md with 3 to 5 graded examples;
  - an MCP server written in TinyCC (langc mcp);
  - AGENTS.md and a README table saying when to use the language;
  - a token-count comparison against Python and Rust.
```

## The answers to the S0 questions (USER, 2026-10-25, verbatim)

```text
"Which compilation target(s) should langc emit? (Pick one or more; the first one you pick becomes the primary target the gate runs.)"="Wasm MVP (tcc-wasm kit), ES module + JSON (tcc-js kit), WGSL compute (tcc-webgpu kit)"
"What should the language (and its private MavenRain repo) be called?"="cairn-lang (Recommended)"
```

The option "Native via TinyCC" was offered and not picked.
