# TinyCC JS realization

Paths below are relative to this host kit, or to the root of a generated
language. The template's `formers/FORMERS.md` gives the host-neutral laws.
The JS target uses the rows of `formers/tcc-wasm.md`: `src/front/check.c`
lines are rows of the builtin table, and `src/front/lower.c` lowers a
program to the IR of `src/ir.h`. `src/js.c` writes that IR as an ES module
(`langc build PROG --js OUT.mjs`, S9, Q-S9-1 (a)).

DONE means: `langc check` accepts the former, `langc eval` computes it,
and `langc build --js` writes it to an ES module when its value is
first-order at run time. Types and universes erase in the module. Proof
contents erase, while their trap flags remain.

`test/gate.sh` runs the rows of each lowered example (`js differential`)
on the ES module in `build/jstool` (`test/jstool.c`, Node). Each row must
print the same output as its Wasm row. It also runs the `test/ir` rows of
`comb`, `lazy`, `spine`, `proof-trap` and `memname` (`test/ir js`) against
`langc --kit eval`. The Makefile runs the rows of `test/wasm/hash.txt`,
`test/wasm/secp.txt` and `test/wasm/traps.txt` on the ES module
(`js regressions`).

| ID | Former | Status | Where | Evidence |
|---|---|---|---|---|
| F1 | Product | DONE | `src/front/check.c:159`, `src/front/check.c:168`, `src/front/lower.c:777`, `src/front/lower.c:1698` | `examples/formers.lang:3`, `examples/laws.lang:12` |
| F2 | Coproduct | DONE | `src/front/check.c:160`, `src/front/check.c:172`, `src/front/lower.c:780`, `src/front/lower.c:1706` | `examples/formers.lang:8`, `examples/laws.lang:13` |
| F3 | Option | DONE | `src/front/check.c:164`, `src/front/check.c:173`, `src/front/lower.c:783`, `src/front/lower.c:1708` | `examples/formers.lang:11`, `examples/formers.lang:12` |
| F4 | FreeMon | DONE. HOST-LIMIT: a run-time tree | `domain/domain.lang:104`, `src/front/lower.c:702`, `src/front/lower.c:704`, `src/front/lower.c:1291`, `src/front/lower.c:1402` | `examples/formers.lang:13`, `test/ir/comb.lang:9` |
| F5 | Monad pure/map/bind | DONE over Option, Sum E and List | `src/front/check.c:175`, `src/front/check.c:176`, `src/front/check.c:177`, `src/front/lower.c:1480`, `src/front/lower.c:1495`, `src/front/lower.c:1074`, `src/front/lower.c:1089` | `examples/monad.lang:2`, `examples/monad.lang:6`, `examples/monad.lang:8`, `examples/laws.lang:8`, `test/ir/spine.lang:10` |
| F6 | Algebra fold | DONE over Nat, List and domain families. HOST-LIMIT: a run-time tree | `src/front/check.c:179`, `src/front/lower.c:1530`, `src/front/lower.c:1059`, `src/front/lower.c:1291`, `src/front/lower.c:1314` | `examples/algebra.lang:4`, `examples/algebra.lang:3`, `examples/algebra.lang:6`, `test/ir/comb.lang:9` |
| F7 | Algebra unfold | DONE into List with a Nat step limit | `src/front/check.c:180`, `src/front/lower.c:1161` | `examples/algebra.lang:7`, `examples/algebra.lang:8`, `test/ir/spine.lang:5` |
| F8 | Filter | DONE over Option and List | `src/front/check.c:178`, `src/front/lower.c:1506`, `src/front/lower.c:1111` | `examples/monad.lang:9`, `test/ir/spine.lang:11` |
| F9 | Non-dependent Pi | DONE. HOST-LIMIT: a function value at run time | `src/front/lower.c:641`, `src/front/lower.c:1830`, `src/front/lower.c:625` | `examples/functions.lang:2`, `examples/functions.lang:4`, `examples/laws.lang:5` |
| F10 | Dependent Pi | DONE over `Type 0` and over Nat | `src/front/lower.c:1827` | `examples/functions.lang:8`, `examples/equality.lang:7` |
| F11 | Sigma | DONE | `src/front/check.c:166`, `src/front/check.c:170`, `src/front/lower.c:778`, `src/front/lower.c:1699` | `examples/sigma.lang:3`, `examples/sigma.lang:5`, `examples/sigma.lang:6` |
| F12 | Path refl/sym/trans | DONE | `src/front/check.c:155`, `src/front/check.c:167`, `src/front/check.c:181`, `src/front/lower.c:768`, `src/front/lower.c:1816` | `examples/equality.lang:4`, `examples/equality.lang:5`, `examples/equality.lang:6`, `test/check/bad-refl.lang:1` |
| F13 | Path transp/cong | DONE | `src/front/check.c:184`, `src/front/check.c:182`, `src/front/lower.c:1771`, `src/front/lower.c:768` | `examples/equality.lang:8`, `examples/equality.lang:9` |
| F14 | Universes | DONE for Type 0 and Type 1, without cumulativity | `src/front/lower.c:1826` | `examples/universes.lang:4`, `test/check/universe.lang:1`, `test/check/universe-range.lang:1` |
| F15 | Generic indexed family | PARTIAL: only the domain file declares an indexed family | `domain/domain.lang:327`, `domain/domain.lang:104` | `examples/bagorbit.lang:7`, `examples/formers.lang:13` |

The HOST-LIMIT refusals (`REFUSE_RUNTIME_TREE`, `REFUSE_RUNTIME_CLOSURE`)
come from `src/front/lower.c`, before a target writes the IR. Thus
`build --js` refuses the same programs as `build`. The F4, F6, F9 and F15
notes of `formers/tcc-wasm.md` apply to the ES module too.
