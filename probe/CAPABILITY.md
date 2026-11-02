# Host capability probe: tcc-wasm

Date: 2026-11-01. Host tree: `/Users/oobi/Documents/cairn-lang` at
0175ef1 (tree 15aaa5f9), read only: `git archive` copies the tree into a
new temporary directory (probe.log:5, :15). Driver: `build/langc`, built on
2026-11-01 by `make build/langc` in that copy (probe.log:16-20), with tcc
0.9.28rc 2026-09-04 mob@0fb54300 (AArch64 Darwin), wasmtime 48.0.1,
wasm-validate 1.0.39 and node v23.10.0 (probe2.log:11-14). Other work
loaded the machine (load averages 7.23 to 7.94, probe.log:4,
probe2.log:4). Thus each wall time is an upper bound. A cite `probe.log:N`
is a line of a run log in `cairn-lang-work/s16r2/probe-logs/`, and a cite
`probe2.log:N` is a line of a run log in `cairn-lang-work/s16r3/probe-logs/`.
Both directories are outside the repo.

The historical probe ran `zsh -f probe.sh > probe-logs/probe.log 2>&1` in
`cairn-lang-work/s16r2/`, then `zsh -f probe2.sh > probe-logs/probe2.log
2>&1` in `cairn-lang-work/s16r3/`. Each step is one `step` line of a
script; to run some steps, keep the setup lines and those `step` lines.
probe.sh runs the base span, the kit steps with `--kit` and the cost rows
on a `Size` chain. probe2.sh runs `test/check/self.lang` and
`test/check/later.lang` again with the flag that `test/check/kit.txt`
gives them, as the gate does. Each step runs below `probe/guard.py`, which
stops a command above 4096 MB resident memory or above its time limit,
120 s (probe.sh:11). The probe writes only below a new temporary
directory. The largest process in the probe used 415.7 MB (probe.log:327,
the build of a chain of 1000 `Size` definitions joined to
`examples/dao.lang`). The scripts, generated inputs and logs are local
artifacts, not files shipped with this repository. They are provenance
for this report, not a repository-local replay command.

To recheck the repository fixtures from the repository root, run:

```sh
make check
python3 probe/guard.py --rss-mb 4096 --timeout 120 -- build/langc check examples/dao.lang
python3 probe/guard.py --rss-mb 4096 --timeout 120 -- build/langc --kit check examples/sigma.lang
python3 probe/guard.py --rss-mb 4096 --timeout 120 -- build/langc --kit check test/check/self.lang
python3 probe/guard.py --rss-mb 4096 --timeout 120 -- build/langc --kit check test/check/later.lang
```

The last two commands intentionally exit 1 with `REFUSE_REC`; the first
three exit 0. The guard uses macOS libproc. These commands cover the
checked-in fixtures, not the custom probe inputs or the historical cost
measurements in section 6.

The base span: `langc check` refuses a program with a `Nat` or another kit
item. The line is REFUSE_KIT `NAME: Nat is a kit item outside the base
span (PLAN section 4); only the kit mode langc --kit admits it`
(probe.log:147, :154, :168; `langc explain REFUSE_KIT`, probe2.log:47).
The gate runs the kit-type programs with `--kit` when `kit.txt` lists them
(`test/gate.sh:15`, `:37`), and so do the kit steps of probe.sh and
probe2.sh. `examples/dao.lang` checks `ok` in the base span (probe.log:42),
and so does a chain of `Size` definitions (probe.log:307, :319).

## Questions

Each host must answer these questions. Each answer is one numbered section
below. An answer sets the status of a former in `formers/tcc-wasm.md`.

| Question | Section | Formers |
|---|---|---|
| Which commands check a file, print a normal form, build and run? | 1 | all |
| Does the host take several files, or must the driver join them? | 2 | all |
| Which recursion does the host accept, and how does it refuse the rest? | 3 | F6, F7 |
| Which `Nat` primitives exist? Does `Nat` have an eliminator? | 4 | F6, F7 |
| How does a result leave the host? | 5 | target |
| What do check and build cost (time and resident memory) at N = 100 and N = 1000? | 6 | all |
| Can a family take a parameter? Can a constructor of such a family appear in a term? | 7 | F4, F12, F15 |
| Can a program project a Sigma? Does the kernel accept Sigma eta? | 8 | F11 |
| How does a program use equality: transport, cong, an index type? | 9 | F12, F13 |
| How does the host report an axiom? | 10 | R2 |

## 1. Commands

| Job | Command | Result |
| --- | --- | --- |
| Check | `build/langc [--kit] check FILE` | `ok` and exit 0 for `examples/dao.lang` (probe.log:42-43), or one line `langc: CODE: NAME: TEXT` and exit 1 (probe.log:142-143). `check --json` prints one JSON object (section 10) |
| Print normal form | `build/langc [--kit] eval FILE NAME [ARGS...]` | The normal form of NAME: `0` for `natSub 4 9` (probe.log:176). Exit 0, also after a trap (section 4) |
| Build | `build/langc [--kit] build FILE -o OUT`, or `--js OUT` or `--wgsl OUT` in place of `-o OUT` | One file. For `examples/dao.lang`: a wasm module of 105 bytes that `wasm-validate` accepts, a JavaScript module of 1863 bytes, or a WGSL file of 3903 bytes (probe.log:67-81) |
| Run | `build/langc [--kit] run FILE SCRIPT` | Each call of SCRIPT and its result: `examples/lists.lang` with `test/run/lists.script` gives the same text as `test/run/lists.out` (probe.log:100-102) |

- `langc` with no command prints the usage of 10 commands and exits 2:
  `check [--json]`, `eval`, `run`, `ir`, `abi [--text]`, `build` (with
  `--runtime`), `prove PROG NAME [POLL]`, `fmt [--check]`, `explain CODE`
  and `mcp` (probe2.log:23-35).
- One file for each command: `check` with two files gives USAGE `too many
  arguments` and exit 2 (probe.log:125-138, section 2).
- One output for each build: `-o` with `--js` gives USAGE `build takes one
  of -o OUT, --js OUT and --wgsl OUT` and exit 2 (probe.log:83, :95).
- `abi examples/dao.lang --text` exits 0 in 1.104 s at 414.8 MB
  (probe.log:97-98). `explain CODE` prints the text of a code
  (probe.log:296, :300; probe2.log:47). `fmt examples/sigma.lang --check`
  prints nothing and exits 0 (probe.log:304).
- `make build/wasmtool` builds the host tool `build/wasmtool`. With no
  argument it prints its usage and exits 2 (probe.log:104-111).
- `PIN` gives the tool versions of the last green host gate (tcc
  0.9.28rc, node v23.10.0, wasm-validate 1.0.39), and the probe tools are
  the same (probe2.log:11-14). `test/gate.sh` and the Makefile do not read
  `PIN` or the probe.
- Not probed: `ir`, `prove`, `mcp`, `build --runtime`, and a run of a built
  module in wasmtime or node.

## 2. Several files

No. A program is one file, with no import form. A file that starts with
`import domain` gives PARSE_EXPECT and exit 1 (probe.log:121-123). `langc
check` with two files gives USAGE `too many arguments` and exit 2
(probe.log:125-138). A program uses the domain names with no import: a
chain of `Size` definitions (`def d1 : Size := szS d0`, probe.sh:93)
checks `ok` (probe.log:307). Thus a driver must join the parts of a
program into one file. The cost rows do this: the probe joins the `Size`
chain and `examples/dao.lang` into one file, and the build of that file
exits 0 (probe.log:315, :327; section 6).

## 3. Recursion and totality

No recursive definition checks. `def rec` (`test/check/rec.lang`) gives
REFUSE_REC `loop: def rec is refused (rule R3); use fold or unfold` and
exit 1 (probe.log:142-143). With `--kit`, a call of the definition itself
(`test/check/self.lang`) or of a later name (`test/check/later.lang`)
gives REFUSE_REC and exit 1: `spin: spin is this definition or a later
one; a definition can use only earlier ones (rule R3)` (probe2.log:39-40),
and the same line for `late` in `early` (probe2.log:43-44). In the base
span these two files stop first at REFUSE_KIT (probe.log:147, :154; the
base-span paragraph above). Recursion is `fold` and `unfold`, as the
REFUSE_REC line says. F6 (algebra fold) is DONE over Nat, List and the
domain families, and F7 (algebra unfold) is DONE into List with a Nat
step limit (`formers/tcc-wasm.md:19-20`). The probe ran no `fold` or
`unfold` of its own.

## 4. Nat

The primitives are `natAdd`, `natSub`, `natDiv` and `natMod`, with the
comparisons `natEq` and `natLe` (`src/front/check.c:196-199`, :211-212),
and `natIsSet` (:229). `natMul` gives NAME_UNKNOWN and exit 1
(probe.log:199-200). `natRec` gives NAME_UNKNOWN too (probe.log:207):
`Nat` has no eliminator primitive, and the eliminator is the algebra
`fold` (F6, section 3). The base span refuses `Nat`:
`test/check/kit-type-nat.lang` gives REFUSE_KIT and exit 1
(probe.log:168-169). Thus the steps below run with `--kit`. `natSub 4 9`
is 0 (probe.log:176). The largest value is 18446744073709551615, that is
2^64 - 1 (probe.log:180). The sum of that value and 1 (`over`), a division
by 0 (`divz`) and a modulo by 0 (`modz`) give EVAL_OVERFLOW `an operation
trapped: a Nat or U256 overflow, or a U256 division by 0`, then print
`trap` and exit 0 (probe.log:184-186, :189-191, :194-196). A literal above
2^64 - 1 gives LEX_NAT_RANGE (probe.log:203). A definition cannot recurse
on a `Nat` (section 3). The base span counts with the domain family
`Size` (`domain/domain.lang:57`, probe.log:266-286).

## 5. How a result leaves the host

As printed text or as one file of a target. `eval` prints the normal form
(probe.log:176), and `run` prints each call of a script
(probe.log:100-102). `build` writes one file: a wasm module with `-o`, a
JavaScript module with `--js`, or a WGSL file with `--wgsl`
(probe.log:67-81). One build gives one target (probe.log:83).
`wasm-validate` accepts the wasm module of `examples/dao.lang`
(probe.log:71-73). `check --json` prints the diagnostics as one JSON
object (probe.log:288-289), and `abi --text` prints the interface of a
program (probe.log:97-99). The probe did not run a built module (section
1).

## 6. Cost

Each file has N chained `Size` definitions (`def dI : Size := szS dJ`,
probe.sh:93). All three rows run in the base span. Check runs `langc check`
on the chain. Build joins the chain and `examples/dao.lang` into one file
and runs `langc build FILE -o OUT`. Run is `langc eval` of the last
definition `dN`, because the probe did not run a built module (section 1).
Each cell gives the wall time and the peak resident memory, the larger of
the two `probe/guard.py` peaks (probe.log:307-329).

| Step | N = 100 | N = 1000 |
| --- | --- | --- |
| Check (`langc check FILE`) | 0.026 s, 12.6 MB | 0.049 s, 17.5 MB |
| Build (the chain joined to `examples/dao.lang`, `langc build FILE -o OUT`) | 1.17 s, 412.0 MB | 1.216 s, 415.7 MB |
| Run (`langc eval FILE dN`) | 0.027 s, 12.7 MB | 0.054 s, 20.9 MB |

- The eval output is 602 and 6002 bytes by `wc -c` (probe.log:311, :323).
  The Run row runs in `zsh -c "... | wc -c"` (probe.sh:95), so its peak
  includes the shell and `wc`.
- `dao100.wasm` and `dao1000.wasm` are 105 bytes, the same as the module
  of `examples/dao.lang` alone (probe.log:317, :329). The build does not
  lower the unused chain definitions.
- `examples/dao.lang` alone builds in 1.115 s at 411.1 MB (probe.log:68).
  Thus almost all of the build cost is fixed. At N = 1000 the chain adds
  about 0.1 s and 4.6 MB.

## 7. Parametric families

A program cannot declare a family: `test/check/data.lang` gives REFUSE_DATA
and exit 1 (probe.log:159-160), and a program with a family `Box` gives
REFUSE_DATA `Box: a program cannot declare a family (rule R1); only
domain/domain.lang can` (probe.log:235). In `domain/domain.lang`, `Trunc1`
(:43) and `Trunc0` (:49) are families, `Stream` (:64) is a cofamily, and
`Pos`, `Word`, `ChoiceBag` and `OrbitN` take a `Size` index, with the type
`Size -> Type 0` (:371-406; probe.log:266-286). The constructor `trunc1`
takes its type as an explicit argument: with the type left out it gives
TYPE_ARITY `trunc1 needs 2 arguments, found 1` (probe.log:223), and with
the type it checks `ok` (probe.log:227). A constructor of an index family
appears in a term: `def p : Pos (szS sz0) := posFirst sz0` (probe.sh:76)
checks `ok` (probe.log:231). `examples/trunc.lang` and `examples/quot.lang`
check `ok` (probe.log:247, :251). `FreeMon` (`domain/domain.lang:113`) is
F4, DONE with the HOST-LIMIT a run-time tree (`formers/tcc-wasm.md:17`).
Thus F15 stays PARTIAL: only the domain file declares an indexed family
(`formers/tcc-wasm.md:28`). Probe files: the step files of probe.sh:73-79
and `examples/trunc.lang`.

## 8. Sigma projection and eta

The projections check: the projection step (probe.sh:71) checks `ok` in
the base span (probe.log:219), and `examples/sigma.lang` checks `ok` with
`--kit` (probe.log:37-40). A Sigma pattern does not parse: a `fun` with a
Sigma pattern (probe.sh:69) gives PARSE_EXPECT `expected ':', found ')'`
(probe.log:215). The kernel accepts Sigma eta: the eta step (probe.sh:67)
checks `ok` with `--kit` (probe.log:211). F11 (Sigma) is DONE
(`formers/tcc-wasm.md:24`).

## 9. Equality

A program writes equality as `Path A x y`, with `refl` and `cong` (F12 and
F13, both DONE, `formers/tcc-wasm.md:25-26`). `transport`, `J` and `trans`
are definitions in `domain/domain.lang:7`, :13 and :21 (probe.log:266-286).
A program that uses `transport` checks `ok` (probe.log:239), and so does a
program that uses `trans` (probe.log:243). The families with path
constructors are `S1` (`domain/domain.lang:38`, a higher inductive family
with `loop (i : I)`), `Trunc1` and `Trunc0` (probe.log:266-286).
`examples/paths.lang`, `examples/equality.lang` and `examples/circle.lang`
check `ok` (probe.log:255, :259, :263); the circle check takes 0.09 s at
188.3 MB (probe.log:264). `trunc1` takes its type as an argument (section
7), so one family serves each index type.

## 10. Axiom detection

No command lists the axioms of a file (the usage, probe2.log:23-34).
`langc check FILE` prints only `ok` for a file with no axiom
(probe.log:42). `langc check --json FILE` prints one JSON object. For a
one-definition axiom-free probe input, it prints
`{"diagnostics":[],"progress":{"parsed":1,"typed":1,"laws_discharged":0,"laws_total":0}}`
and exits 0 (probe.log:288-289). For `test/check/axiom.lang`, it prints one
diagnostic REFUSE_AXIOM `magic: an axiom has no proof (rule R2)` with the
span 1:1 to 1:18, and exits 1 (probe.log:292-293). A plain `check` of that
file gives REFUSE_AXIOM and exit 1 (probe.log:164-165). `langc explain
REFUSE_AXIOM` prints the text of the code (probe.log:296). Thus a program
with an axiom does not check, and R2 holds at the check.

## Consequences for M0

1. A program is one file. The domain names need no import (section 2).
2. No `def rec`, and no call of the definition itself or of a later one:
   recursion is `fold` and `unfold` (section 3, R3).
3. A program with a kit type (`Nat`) runs with `langc --kit`, as the gate
   does when `test/check/kit.txt` or `examples/kit.txt` lists it. The base
   span refuses it with REFUSE_KIT (the base-span paragraph, section 4).
4. A `Nat` is at most 2^64 - 1. An overflow and a division by 0 trap with
   EVAL_OVERFLOW, print `trap` and exit 0, and `natSub` stops at 0. Thus a
   driver must read the `trap` line, not the exit status (section 4).
5. One build writes one target: `-o` (wasm), `--js` or `--wgsl` (sections
   1 and 5).
6. A program cannot declare a family, so F15 stays PARTIAL (section 7, R1).
7. A build costs about 1.2 s and 412 MB, and almost all of it is fixed. The
   unused definitions add no byte to the module (section 6).
8. An axiom stops the check with REFUSE_AXIOM and exit 1 (section 10, R2).
