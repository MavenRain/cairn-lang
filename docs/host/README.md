# TinyCC Wasm host kit

This kit compiles a small typed language to a WebAssembly MVP module.
One C99 program, `build/langc`, holds the parser, the checker, the
evaluator, the lowering to a first-order IR and the Wasm writer.
TinyCC builds it.

## Shared files

This kit and `hosts/tcc-evm` share these files. `TCC_SHARED` in the root
`Makefile` lists them:

- the source: the front end (`src/front/`), `src/ir.h`, `src/target.h`
  and `src/main.c`
- the domain and its embed tool: `domain/` and `gen/`
- the examples: `examples/`
- the shared tests: `test/parse/`, `test/check/`, `test/eval/`, `test/ir/`,
  `test/review.sh` and `test/fronttool.c`
- `.gitignore`

`make check` at the template root compares each shared entry in the two
kits with `diff -r`. It fails when a shared file is different in the two
kits or is missing from one kit. When you change a shared file, make the
same change in `hosts/tcc-evm`.

These files are specific to this kit:

- the target file: `src/wasm.c`
- the target test files: `test/wasmtool.c` (the Wasm runner on the
  wasmtime C API) and `test/wasm/` (the prelude vectors)
- the build and gate files: `Makefile`, `test/gate.sh` and `PIN`
- the docs: `README.md`, `FORMERS.md` and `docs/CAPABILITY.md`

## Build

From the template root:

```sh
make -C hosts/tcc-wasm build
make -C hosts/tcc-wasm check
make -C hosts/tcc-wasm check-clang
```

`build` runs `tcc -run gen/embed.c` to copy `domain/domain.lang` into
`build/domain.c`. Then it builds `build/langc` with
`tcc -std=c99 -Wall -Werror`. The domain is part of the executable.
`check-clang` compiles the same sources with
`cc -std=c99 -Wall -Wextra -Wswitch-enum -Werror -fsyntax-only`.
`check` does both steps, then runs `test/gate.sh`.
The gate needs `node`, `wasm-validate` and `wasm-objdump` on the `PATH`.
`PIN` records the tool versions of the last GREEN gate.
The build and the gate need no network access.

## Source language

A program is a sequence of `def NAME : TYPE := TERM`. A line comment
starts with `--`. Lambdas are `fun (x : A) => t`. There is no text type.

Core names: types `Nat Flag Prod Sum Option List Eq`, values
`flagYes flagNo pair first second inl inr either none some option nil cons
pure map bind fold unfold filter pack witness payload refl symm trans
transport cong`, Nat operations `natAdd natSub natMul natEq natLe`, and the
Flag eliminator `flagIf b yes no`. `Type 0 : Type 1` is the universe
chain. `Sigma (x : A) B` and `(x : A) * B` are dependent pairs.
`FORMERS.md` gives the status of each former.

The compiler enforces these rules:

| Rule | Code | The compiler refuses |
|---|---|---|
| R1 | `REFUSE_DATA` | a `family` declaration in a program |
| R2 | `REFUSE_AXIOM` | an `axiom` declaration |
| R3 | `REFUSE_REC` | `def rec`, or a definition that refers to itself or to a later definition |
| R4 | `REFUSE_NAME` | a core name, a domain name, a reserved name or a repeated name as a new definition |
| R4 | `REFUSE_TIME` | a time word (`now`, `block`, `timestamp`, `time`, `clock`, `epoch`) as a name or a use; the message says to count operations (`len r`, an `OpPos`) |

## CLI

```sh
build/langc check [--json] PROG
build/langc eval PROG NAME [ARGS...]
build/langc ir PROG
build/langc abi PROG
build/langc build PROG -o OUT [--runtime]
build/langc fmt PROG [--check]
```

- `check` prints `ok`, or refuses the program. With `--json`, it
  prints one JSON line on stdout in place of `ok` or the diagnostic
  (`docs/CHECK-JSON.md`).
- `eval` prints the normal form of `NAME`. Each argument is a decimal
  number. When an entry traps, `eval` prints `trap` on stdout and an
  `EVAL_OVERFLOW` line on stderr, and the exit status is 0.
- `ir` prints the first-order IR. It has one `func` for each entry.
- `abi` prints the name of each entry, one for each line.
- `build` writes the Wasm module to `OUT`. On this target, `--runtime`
  gives the same bytes.
- `fmt` prints the canonical text of the program (S11, A1). It parses
  `PROG` and does not check it, so it also accepts a program that `check`
  refuses (`examples/algebra.lang`, `domain/domain.lang`). With
  `--check`, `fmt` prints nothing and exits 0 when `PROG` is canonical.
  Else it prints `PROG:LINE: not canonical` on stderr, where `LINE` is
  the first line that is different, and exits 1. On a lex or parse error,
  `fmt` writes the diagnostic on stderr, writes nothing on stdout and
  exits 1. A usage error, an IO error or a full arena gives exit 2.
  Before emitting text, `fmt` checks that it fits the source size limit
  and reparses it. If the canonical spelling exceeds the size or parser
  depth limit, `fmt` exits 1 with a diagnostic and no stdout output.

The exit status is 0 for success, 1 for a refused program and 2 for a
usage or IO error. A diagnostic on stderr has the form
`langc: CODE: DEF: message`.

## Entries

An entry is a top-level program definition whose declared type is `Nat`,
`Flag`, or a first-order function from `Nat` and `Flag` arguments to a
`Nat` or `Flag` result. `build` exports each entry by its name. Each
parameter and the result is an `i64`. Other definitions are not exported.

`Nat` is an unsigned 64-bit integer. `natAdd` and `natMul` trap on an
overflow past 2^64 - 1. `natSub` stops at 0. `Flag` is 0 or 1.

## Compilation

Compilation is partial evaluation. The compiler normalizes the body of
each entry with the parameters as free variables. Types and universes erase.
Proof contents erase, while their trap flags remain. A lambda stays only as the direct function argument of
`fold`, `unfold`, `map`, `bind`, `filter`, `either` or `option`. The
lowering puts its body into a loop or a branch. The lowering refuses two
programs with a HOST-LIMIT code (see `docs/CAPABILITY.md`):

- `REFUSE_RUNTIME_CLOSURE`: a function value must exist at run time.
- `REFUSE_RUNTIME_TREE`: a fold walks a tree that exists only at run time.

## Lazy traps

A trap is lazy. It stops a computation only when the computation needs
the value. `langc eval` and the Wasm module agree on this rule. The
lowering gives each value a word and a trap flag. A heap field keeps its
word and its flag. The entry result is the only strict point: when its
flag is set, the entry executes `unreachable`.

The lowering lowers a value one time in a block. The blocks in that
block use the same result, but the two arms of a branch do not share
results.

Strict traps are not correct here. Normalization at compile time drops
terms that are not read, for example `first (pair a b)` gives `a`. The
evaluator also computes both arms of `flagIf`. A strict trap would fire
in a term that the program does not use.

Decision record: the kit builder (Claude) made this decision. The
repository owner has not ruled on it.

## Wasm layout

- The module has one memory with a maximum of 16384 pages (1 GiB) and
  one mutable `i64` global, the heap pointer.
- Function 0 is the internal allocator `alloc (bytes : i32) -> i64`. It
  increases the heap pointer. When a block does not fit, it calls
  `memory.grow`. When the memory cannot grow past 1 GiB, it traps.
- Function 1 + K is entry K. Each entry first sets the heap pointer to 0,
  so each call starts with an empty heap.
- An entry traps when a `Flag` argument is larger than 1.
- A word is 8 bytes. A heap block holds the tag in word 0, the word of
  field K in word 1 + 2K and the trap flag of field K in word 2 + 2K.
- A tag switch is nested blocks and `br_table`. A tag out of range traps.

## Domain slot

`domain/domain.lang` holds the families and the operations of the domain.
Only this file can declare a `family` (rule R1). `domain/README.md` tells
what the domain holds and how to change it.

## Gate

`make -C hosts/tcc-wasm check` prints these lines when it is GREEN:

| Line | What the gate does |
|---|---|
| `examples: 57 checked, 0 failed` | `langc check` accepts each `examples/*.lang` file |
| `parse refusals: 19 checked` | each `test/parse` file fails with the code in `test/parse/expect.txt`; one file has 1100 nested parentheses |
| `check refusals: 197 checked` | each `test/check` mutant fails with the code in `test/check/expect.txt`; the `TYPE_REFL` message shows both normal forms |
| `evals: 245 checked` | `langc eval` gives each result in `test/eval/expect.txt` |
| `build refusals: 9 checked` | `langc build examples/dao.lang -o OUT` exits 0, writes nothing on stdout and writes a valid module; each refused build exits 1 with its code and writes nothing on stdout; the rows include the transp row (`test/ir/transp.lang`, `REFUSE_LOWER`), three `REFUSE_TARGET_SECRET` rows (`test/ir/secret-key.lang`, `test/ir/secret-sign.lang`, O6, and `test/ir/secret-key.lang` with `--wgsl`), the elim row (`test/ir/tree-elim.lang`, `REFUSE_RUNTIME_TREE`, O26) and a guest-mode `REFUSE_KIT` row |
| `ir: 56 lowered, 1 refused (HOST-LIMIT)` | `langc ir` lowers each example, with one `func` for each `abi --text` line derived independently from checked types; an example that does not lower refuses `REFUSE_LOWER` with a `HOST-LIMIT` detail |
| `wasm: 56 modules valid` | `langc build` writes a module for each lowered example, and `wasm-validate` and `wasm-objdump -x` accept it |
| `wgsl: 56 modules written` | `langc build PROG --wgsl OUT.wgsl` writes a WGSL compute module for each lowered example; each build exits 0 and writes nothing on stdout |
| `wgsl refusals: 2 checked` | each `--wgsl` usage row of `src/main.c` fails with its message: `--wgsl` with `--js`, and `--wgsl` with no path |
| `wasm differential: 81 rows, 0 mismatches` | `build/wasmtool` (`test/wasmtool.c`, on the wasmtime C API) runs each `test/eval/expect.txt` row whose name is an entry of its file; each result is equal to the row result, and a trap is equal to a trap |
| `js differential: 81 rows, 0 mismatches` | `langc build PROG --js OUT.mjs` writes an ES module for each lowered example, and `build/jstool` (`test/jstool.c`, node through `popen`) runs the rows of the `wasm differential` line; each result is equal to the row result, and a trap is equal to a trap |
| `test/ir wasm: 5 modules valid, 32 rows, 0 mismatches` | each `test/ir` module builds in `--kit` mode and is valid, and each row of the gate list gives the same result on `build/wasmtool` and in `langc --kit eval` |
| `test/ir js: 32 rows, 0 mismatches` | each row of the `test/ir` gate list gives the same result on `build/jstool` and in `langc --kit eval` |
| `json abi: 2 checked` | `langc --kit abi test/wasm/secp.lang` prints the JSON abi line (each kind is `nat`, `flag` or `u256`), and `abi` with a flag other than `--text` refuses `USAGE` |
| `fmt canonical: 289 passed` | `langc fmt` gives the same bytes for each accepted `.lang` file of `domain/`, `examples/` and `test/*/` outside `test/fmt/` (the `test/json` rows `LEX_*` and `PARSE_*` are not in this check); each file that `langc fmt` refuses is in `test/fmt/refused.txt` (18 `test/parse` files), and no accepted file is |
| `fmt round trip: 296 passed` | each accepted file (the 289 files and the 7 `test/fmt/*.lang` files) and its canonical text give the same `build/fronttool dump` (`test/fronttool.c`: one line for each node, with no line numbers and no comments) |
| `fmt rows: 7 passed` | `langc fmt test/fmt/X.lang` gives the bytes of `test/fmt/X.canon` for each of the 7 rows |
| `fmt check: 8 passed` | `langc fmt X.canon --check` exits 0 for each of the 7 rows, and `langc fmt test/fmt/check.lang --check` exits 1 with `test/fmt/check.lang:3: not canonical` |
| `json rows: 20 passed` | for each `test/json/X.lang`, `langc check --json` exits 0 (the rows `ok` and `kit`) or 1 (each other row, whose first code is the file name up to the first dot), writes nothing on stderr, and gives the bytes of `test/json/X.json`; each refused row has `test/json/X.fix` (`docs/CHECK-JSON.md`) |
| `json refusal codes: 13 of 13` | each `REFUSE_` code of `src/front/check.c` has a row `test/json/CODE.lang`, and the list of codes is not empty |
| `json fixes: 18 passed` | `build/fronttool fix` applies each `test/json/X.fix` to its row, and `langc check --json` of the result exits 0 or gives a first code other than the code of the row |
| `gate: 0 failures` | the sum of all failures |

`make check` also runs `test/jstest.c` against an ES module with entries
named `default`, `a'` and `then`, loaded from a path containing a space,
`#`, `?` and `%`. It repeats the calls with an apostrophe in `TMPDIR`.

The gate also checks the usage and IO exit codes, five `eval` refusals,
`test/ir/closure.lang` (`REFUSE_RUNTIME_CLOSURE`), `test/ir/tree.lang`
(`REFUSE_RUNTIME_TREE`), the 11 entries of `test/ir/spine.lang`,
`pick 2 4 9` = `trap` on Wasm, and the absence of em-dashes and en-dashes
in the kit. The lowering computes a path of data that is stuck at compile
time before it takes the end point of the path, so a trap in the path
reaches the result: the 4 entries of `test/ir/proof-trap.lang` trap at
18446744073709551615 on Wasm. `perl bin/doc-check.pl hosts/tcc-wasm` must report 0
problems.

`make check` also compares the 25 fixed rows in `test/wasm/traps.txt`
against the evaluator, Wasm and the ES module, and the rows of
`test/wasm/hash.txt` and `test/wasm/secp.txt` against Wasm and the ES
module. They cover neutral path endpoints,
trapped curve coordinates, and unused invalid curve results. It checks
that `abi --text` still reports checked signatures when lowering refuses
`test/ir/transp.lang`.

`make gpu-check` runs `sh test/gate.sh gpu`, the WebGPU rows (S10). It
needs a GPU adapter, so it runs outside the sandbox. `build/wgsltool`
(`test/wgsltool.c`) gives each WGSL module to deno (`--unstable-webgpu`;
`docs/VALIDATION.md` V11). `make check` skips these rows. `make gpu-check`
prints these lines when it is GREEN:

| Line | What the gate does |
|---|---|
| `wgsl valid: 52 modules` | the GPU compiles each entry of the WGSL module of each lowered example; the 4 modules of `test/wgsl/internal.txt` are not counted |
| `wgsl known internal: 4 modules, 20 entries` | each entry of `test/wgsl/internal.txt` gives `GPUInternalError` (the Metal backend limit, `docs/VALIDATION.md` V15), and no other entry does |
| `wgsl differential: 81 rows, 0 mismatches` | `build/wgsltool` runs the rows of the `wasm differential` line; each result is equal to the row result, and a trap is equal to a trap |
| `wgsl regressions: passed` | the rows of `test/wasm/hash.txt`, `test/wasm/secp.txt` and `test/wasm/traps.txt` on the WGSL module, so the gate prints this line three times |
| `test/ir wgsl: 32 rows, 0 mismatches` | each row of the `test/ir` gate list gives the same result on `build/wgsltool` and in `langc --kit eval` |
| `wgsl batch: 192 rows, 0 mismatches` | the batch proof check of `test/wgsl/batch.lang` on the rows of `test/wgsl/batch.txt`: 64 rows each for `sigOk`, `or0Ok` and `or1Ok` |
| `wgsl chain: 1024 links, 1 bad link found` | the chain rehash of `test/wgsl/batch.lang` checks 1024 links on the GPU and finds the one bad link |

## Known limits

- A `fold` over a bare `nil` cannot infer its type (`TYPE_INFER`). Use a
  definition with a declared `List` type.
- With no expected type, `either`, `map` and `cong` take the result type
  of the function at a fresh variable. A dependent result needs a
  declared type.
- A definition with a `List`, `Prod` or other non-scalar result is not an
  entry. `ir` and `abi` skip it.
- `langc eval` stops after 20000000 steps (`EVAL_FUEL`). The Wasm module
  has no step limit, so a large loop count can run on Wasm and stop in
  `eval`. The grid uses large values only for entries with no loop.
- The arena of `langc` and the Wasm memory each stop at 1 GiB.
- Source files are at most 1 MiB. `Nat` literals are at most
  18446744073709551615.
