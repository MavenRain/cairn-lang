# AGENTS.md

## Routing rule

Use cairn-lang when the task is in a "yes" row of the README table
(`README.md`, "When to use cairn-lang"): an append-only record of content
hashes with inclusion proofs, a threshold committee that signs each
append, an encrypted vote with a threshold tally, a rule change, or a
change of the rule that changes the rules. Do not use it for a token or a
general contract, a wall-clock deadline (there is no clock: time is the
number of operations applied), state with deletes or edits, general FHE
or general ZK circuits. The skill `skills/cairn-lang/SKILL.md` gives the
same rule, the loop and 5 graded examples (G1 to G5, the files of
`examples/skill/`).

## The loop

1. Write FILE.lang: one `def NAME : TYPE := TERM` for each value. A guest
   file names the domain defs only (`design/DESIGN.md`). A word literal
   (for example a key or a signature word) is a kit item: a kit list
   names the file (`examples/skill/kit.txt`), and `build/langc --kit`
   checks it.
2. Run `build/langc check --json FILE.lang` (`build/langc --kit check
   --json FILE.lang` for a kit file). It writes one JSON line on stdout,
   with the `diagnostics` array and the `progress` object
   (`docs/CHECK-JSON.md`). Exit 0: the checker accepts the file. Exit 1:
   the checker refuses the file. Exit 2: a usage error.
3. For each diagnostic, `build/langc explain CODE` gives the row of its
   `code`. Change the file, then do step 2 again.
4. Run `build/langc fmt FILE.lang --check`. No output and exit 0: the
   text is canonical. Else write the output of `build/langc fmt FILE.lang`
   to a new file, and replace FILE.lang with it.
5. `build/langc eval FILE.lang NAME` gives the value of the def NAME.

For a kit file, use `build/langc --kit eval FILE.lang NAME` in step 5.

`build/langc mcp` gives the tools `check`, `fmt`, `eval`, `build` and
`explain` as an MCP server on stdin and stdout (`docs/MCP.md`). With
`--kit`, each tool runs in the kit mode.

## Status (S14)

The slices S0 to S13 give the kernel, the domain, the targets (Wasm, ES
module + JSON, WGSL), `langc fmt` (S11), `langc check --json` (S12), and
`langc mcp` and `langc explain` (S13). S14 adds
`skills/cairn-lang/SKILL.md`, the graded examples of `examples/skill/`,
`test/skilltool.c`, this file and the README table. `docs/PLAN.md` gives
each slice and the next slice.

## Commands that work now

- `make`: build `build/langc` and the 17 test tools (`build/*tool`).
- `make check`: build, then run the gate `test/gate.sh`. Run it serial
  (no `-j`). It takes about 325 s. It prints 60 count lines (`NAME: N of
  N`) and `gate: 0 failures` at the end. Each failure prints a `FAIL:`
  line.
- `build/langc [--kit] check [--json] PROG`: check PROG. Text: `ok` on
  stdout, or one line on stderr. `--json`: one JSON line
  (`docs/CHECK-JSON.md`).
- `build/langc [--kit] eval PROG NAME [ARGS...]`: the value of the def
  NAME.
- `build/langc fmt PROG [--check]`: print the canonical text of PROG.
  With `--check`: no output and exit 0 when PROG is canonical.
- `build/langc explain CODE`: the row of the diagnostic code CODE.
- `build/langc [--kit] mcp`: the MCP server (`docs/MCP.md`).
- `build/langc [--kit] run PROG SCRIPT`, `ir PROG`, `abi PROG [--text]`,
  `build PROG (-o OUT | --js OUT.mjs | --wgsl OUT.wgsl) [--runtime]` and
  `prove PROG NAME [POLL]`: the back end and the targets (`README.md`).

`build/langc` with no argument prints the usage text and exits 2.
`build/langc check --json examples/skill/g1-record.lang` exits 0 and
prints this line:

```text
{"diagnostics":[],"progress":{"parsed":17,"typed":17,"laws_discharged":0,"laws_total":0}}
```

## Rules for a change

- `design/DESIGN.md` is the only source of the core types and the core
  operations.
- Keep each `.lang` file canonical: `build/langc fmt PROG --check` gives
  no output and exit 0.
- Put a test in a C tool in `test/` or in a `.lang` file, with a row in
  `test/gate.sh`. Do not add a new node, Python or shell test harness.
- Each example block of SKILL.md is byte equal to its file in
  `examples/skill/`, and each output block is the output of its command.
  The gate checks both (`skill quotes`, `skill outputs`). Change the file
  and the block together.
- No em-dash or en-dash anywhere. The gate checks it.
- The gate does not compare the verbs in this file with the usage text.
  After a change to the usage text, compare them by hand.
