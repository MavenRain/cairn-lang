# Validation

Date: YYYY-MM-DD. <Tool>: <version>. Host executable: `<path>` at
<commit>.

`<gate command>` passes: <what the gate checks and builds>, and it runs <N>
tests with zero failures. The test runner reported <S> seconds.

> Write here: one paragraph for each slice, in this form.

The <slice> review adds <N> cases in `<test file>`. They check <the forms
and the cases that work>. Refusals cover <the cases that the compiler must
refuse>.

## Trusted constants

Each row is a kernel constant with no computation rule. The checker
accepts its type as given, so each row is a trust assumption.

| Constant | Type | Since | Reason that the type holds |
|---|---|---|---|
| `natIsSet` | `isSet Nat` | S5, Q-S5-6 (b) | `Nat` has a decidable equality, so the Hedberg theorem gives `isSet Nat`. |
| `u256IsSet` | `isSet U256` | S5, Q-S5-6 (b) | `U256` has a decidable equality (Hedberg). |
| `flagIsSet` | `isSet Flag` | S5, Q-S5-6 (b) | `Flag` has a decidable equality (Hedberg). |

Each constant is a `VIS_INTERNAL` opaque op (`OP_NAT_IS_SET`,
`OP_U256_IS_SET`, `OP_FLAG_IS_SET` in `src/front/core.h`). The evaluator
keeps each one stuck. `rule_is_set` in `src/front/check.c` gives each one
its fixed kernel type, independently of the domain definition named
`isSet`. The domain folds `len`, `chainFrom` (thus `head`), `verify` and
`verifyAt` use them (`domain/domain.lang`), directly or through
`isSetFun`.

## Signature verification

`schnorrOk` is a computed `VIS_INTERNAL` kernel builtin
(`OP_SCHNORR_OK`). `rule_fixed` in `src/front/check.c` checks its argument
types, and `schnorr_ok` in `src/front/eval.c` computes the result when all
five arguments are closed `U256` words. Open arguments keep it stuck.

| Builtin | Type | Since | Check |
|---|---|---|---|
| `schnorrOk` | `U256 -> U256 -> U256 -> U256 -> U256 -> Flag` | S7, Q-S7-6 (a), Q-S7-8 (a) | The arguments are the key, R.x, z, head r and e. The C check is the secp256k1 x-only Schnorr check of Q-S7-2 (a): it gives 1 when z G = R + c P, else 0. The unforgeability of the threshold Schnorr signature is V1 to V4 (PLAN section 5). |

## Assumptions

Each row is a computational assumption that no path can prove. V1 to V4
stay in PLAN section 5.

| Item | Since | Assumption |
|---|---|---|
| V5 | S6, Q-S0-7 (a), Q-S6-8 (a) | The Pedersen DKG lets a member bias the distribution of the public key (Gennaro, Jarecki, Krawczyk, Rabin). The security of ElGamal and of the threshold Schnorr signature under that bias is an assumption (PLAN section 5, row V5). |
| V6 | S8, Q-S8-9 (a) | The Wasm runner (wasmtime 48.0.1, the C API that `test/wasmtool.c` links) runs a module as the Wasm MVP specification says, and `wasm-validate` (wabt 1.0.40) accepts only a valid module. The `wasm differential` and `wasm regressions` rows hold only under this assumption. |
| V7 | S8, Q-S8-9 (a) | The Wasm prelude uses the algorithms of `secp256k1.c`. Nobody checks the prelude for constant time. The JS prelude of `src/js.c` uses BigInt and is not constant time either. No secret value reaches a target: O6 (S9) refuses a secret operation that is stuck at run time, and V8 holds. |
| V8 | S9, Q-S9-9 (a) | The guest secret operations (`zkKey`, `zkSign` and the other three of O6) give test keys only. A test key is hash(tag, i) mod n, so it is public. A real key never enters the guest: the DKG shares, the nonces and the decryption key stay in the host C code. The build refuses a secret operation that is stuck at run time (O6). A closed secret operation lowers to constants (PLAN O30). |
| V9 | S9, Q-S9-2 (a) | The JS runner (node v23.10.0, which `test/jstool.c` runs through `popen` with a heap cap of 2048 MiB) runs the ES module of `langc build PROG --js OUT.mjs` as the ECMAScript specification says, with exact BigInt arithmetic. The `js differential`, `test/ir js` and `js regressions` rows hold only under this assumption. |
| V10 | S9, Q-S9-13 (a) | The ES module and the Wasm module give the same result only below the heap limit. The Wasm `fe_mul` takes a 64-byte scratch block for each call and the JS `feMul` does not, so a long curve run can trap on Wasm at `HEAP_WORDS` where JS gives a value, and node can stop first at its heap cap (`build/jstool` exit 2). No gate row reaches the limit. |
| V11 | S10, Q-S10-7 (a) | The GPU runner (deno 2.9.7 with `--unstable-webgpu`: WebGPU on wgpu and Metal, on the adapter "Apple M2 Pro") runs the WGSL module of `langc build PROG --wgsl OUT.wgsl` as the WGSL specification says, with exact u32 arithmetic. `build/wgsltool` (`test/wgsltool.c`) gives the module and its calls to deno outside the sandbox (`make gpu-check`). The `wgsl valid`, `wgsl differential`, `wgsl regressions`, `test/ir wgsl`, `wgsl batch` and `wgsl chain` rows hold only under this assumption. |
| V12 | S10, Q-S10-7 (a) | The WGSL prelude of `src/wgsl.c` (`RUNTIME`, `LIMB_PART`, `SHA_PART` and `EC_PART`) works on u32 limbs. Nobody checks it for constant time. No secret value reaches it: the build refuses a secret operation that is stuck at run time with `REFUSE_TARGET_SECRET` (O6; the `--wgsl` row of `build refusals: 9 checked`), and V8 holds. |
| V13 | S10, Q-S10-7 (a) | The WGSL module keeps each value in u32 limbs, low first (`src/wgsl.c:19`), and WGSL code makes each carry. The `wgsl regressions` rows of `test/wasm/secp.txt` and the `wgsl batch` sigOk rows kill a mutant that drops the `w_add` carry (S10 M1) and a mutant that drops one cross term of `w_mul32` (S10 M3). The `wgsl differential` rows do not kill them. Only the `wgsl differential` rows kill a mutant of the `w_divmod` subtract (S10 M4b). |
| V14 | S10, Q-S10-7 (a) | Each GPU invocation has a heap slice of `HEAP_WORDS` words (4096, `src/wgsl.c:83`). A row that needs more words sets the trap flag and traps (`src/wgsl.c:186`); it does not give a wrong value. The Wasm module and the ES module have larger heaps, so a long run can trap on WGSL where they give a value. The 81 differential rows give the row result with `HEAP_WORDS` 4096 or 64. With 16 or 8, the `words` rows `addTrap 0` and `eqTrap 0` trap (want 1), and each other row gives the row result (`cairn-lang-work/s10r14/run3.sh`). No gate row reaches the limit. |
| V15 | S10, Q-S10-r13 (b) | The Metal backend does not compile 20 entries in 4 modules (`examples/ballot.lang`, `examples/daokit.lang`, `examples/onehot.lang`, `examples/threshold.lang`): their pipelines give `error E pipeline GPUInternalError`. `test/wgsl/internal.txt` names the 20 entries, and the `wgsl known internal: 4 modules, 20 entries` row checks that the list is exact. A scope of each entry found them; the earlier `wgsl valid: 56` count was a false pass. None of the 20 entries is in the 81 differential rows. PLAN O31 keeps this open. |
| V16 | S10, Q-S10-r14 (a) | The WGSL `LIMB_PART` (`src/wgsl.c:205`) holds `u_mulmod` (`src/wgsl.c:296`) and `u_pow` (`src/wgsl.c:314`), and the call table holds `feAdd`, `feSub`, `feMul` and `feInv` (`src/wgsl.c:604`, `src/wgsl.c:615`, `src/wgsl.c:623`, `src/wgsl.c:631`). No module that `langc` writes calls them: the front end lowers no `fe_*` IR call from guest source (`src/front/lower.c:1741` to `src/front/lower.c:1770`), and the need mask comes from the IR call nodes only (`src/wgsl.c:816`). `wgsl_write` writes `LIMB_PART` whole when a module needs limbs (`src/wgsl.c:1094`). Thus no gate row tests them on the GPU. In Wasm, `fe_mul` is live: `ec_add` and `ec_mul` need it (`src/wasm.c:611`). PLAN O32 keeps this open. |
| V17 | S11, Q-S11-1 to Q-S11-8 (a) | `langc fmt` keeps the meaning of a program. No proof shows this. The gate checks it on the files of the repo: each of the 296 accepted `.lang` files and its canonical text give the same `build/fronttool dump` (`fmt round trip: 296 passed`), the 289 files outside `test/fmt/` are canonical (`fmt canonical: 289 passed`), each `test/fmt/X.lang` formats to `test/fmt/X.canon` (`fmt rows: 7 passed`), and `--check` exits 0 on each `X.canon` and 1 on `test/fmt/check.lang` (`fmt check: 8 passed`). The dump has no comments and no line numbers: only the byte compare of the canonical files and the row `test/fmt/comment.lang` check that `langc fmt` keeps each comment. The mutants M1 to M6 of `src/front/fmt.c` and `src/main.c` are KILLED, each by its own row. |
| V18 | S12, Q-S12-1 to Q-S12-12 | `langc check --json` gives the first failure of `langc check`, a fix that removes it, and correct progress counts. No proof shows this. The gate checks it on the rows of `test/json/` (`json rows: 20 passed`, `json refusal codes: 13 of 13`, `json fixes: 18 passed`; `docs/CHECK-JSON.md`). The 14 S12 mutants are KILLED. The first json rows kill M2, M3, M4, M5, M6, M8, M9b, M10 and M11 (`src/front/check.c`, `src/front/parser.c`, `src/front/base.c`, `src/main.c`, `test/fronttool.c`). The `json quote` regression of `test/fronttool.c` kills M1 (`json_string` does not escape `"`): no program text can put a `"` in the line, only the file path can. The row `test/json/TYPE_MISMATCH.after.lang` kills M3b (the law scan drops the law decls after the failing decl). The row `test/json/TYPE_MISMATCH.splice.lang`, where a spliced law def follows the failing decl, kills M4b (the law scan drops the program origin test). The gate check that `src/front/check.c` has a `REFUSE_` code kills M7 (the gate code loop runs over no code). The `diag fix reset` regression of `test/fronttool.c` kills M9 (`diag_vfail` does not clear `fix_next`). Through `langc check`, M9 is equivalent: the only resets of `diag->set` are in `branch_index` of `src/front/check.c`, and they occur only after a first `TYPE_MISMATCH` failure. `src/front/check.c` sets a fix only through `FAIL_FIX` (`REFUSE_CELL`, `REFUSE_SIG`, `REFUSE_STATE`), and each lexer or parser fix comes with its own failure. Thus a pending fix never meets a first `TYPE_MISMATCH` failure. |
| V19 | S13, Q-S13-1 to Q-S13-13 | `langc mcp` gives the replies of the MCP revision `2025-11-25` for the methods `initialize`, `ping`, `tools/list` and `tools/call`, and each tool result has the stdout, the stderr and the exit code of its CLI verb. No proof shows this, and no MCP client runs in the gate. The gate checks it on the 9 rows of `test/mcp/` (`mcp rows: 9 passed`: the replies are byte equal), checks that `build/mcptool` exits 1 on 3 bad inputs (`mcp negatives: 3 of 3`), checks that the rows call each of the 5 tools (`mcp tools: 5 of 5`), and checks that `langc explain` knows each `REFUSE_` code of `src` (`explain codes: 17 of 17`; `docs/MCP.md`). The -32600 and -32603 errors and the 1 MiB line limit have no row. Of the S13 mutants, 8 ran: 7 are KILLED and 1 is LIVE. The lifecycle row, where a `ping` has the id `"a\nb"`, kills M1 (`\n` decodes to `\v`). The errors row kills M2 (an unknown method gives -32600). Each of the 9 rows kills M3 (a notification gets a reply). The tool rows kill M4 (`isError` inverted, 7 rows) and M5 (the stderr item dropped, 6 rows). The explain row kills M6 (the `REFUSE_DATA` and `REFUSE_AXIOM` texts swapped). The gate line `FAIL mcp negative changed .out: exit 0` kills M9 (`same_bytes` of `test/mcptool.c` always gives equal). M8 (`build/mcptool` skips the stderr check) is LIVE: no `langc mcp` path writes stderr, thus no row can kill it, and the check guards a later path that does (Q-S13-12 (a)). M7 (the arena is not released) did not run, because no row checks memory. |
| V20 | S14, Q-S14-1 to Q-S14-8 | The worked examples G1 to G5 of `skills/cairn-lang/SKILL.md` are true: each program block is the bytes of its file in `examples/skill/`, each file checks to ok and is canonical, and each `langc` row gives the bytes of its output block and the exit code of its sentence. No proof shows this, and no agent runs in the gate. The gate checks it with `langc check` (`langc --kit check` for G2, the line of `examples/skill/kit.txt`) and `langc fmt --check` on each example (`skill checks: 5 of 5`) and with `build/skilltool` on SKILL.md (`skill quotes: 5 of 5`, `skill outputs: 14 of 14`), and checks that `build/skilltool` exits 1 with the FAIL line of the change on 3 fixtures of `test/skill/`: a changed program byte, no text block and a wrong exit code (`skill negatives: 3 of 3`). AGENTS.md and the README table have review only, and no row compares the usage block of SKILL.md `## Verbs` with the usage text of `langc`. Of the S14 mutants, 6 ran: 2 are KILLED, 3 are LIVE and 1 is equivalent. The bad-exit fixture kills M2 (the exit code is not compared). The G2 check row kills M4 (the block drops `--kit` for G2, thus `REFUSE_KIT`). M1 (only the G1 program block is compared), M3 (the block compare stops one byte short) and M5 (the block skips `fmt --check`) are LIVE: no fixture changes a block of G2 to G5, no fixture differs from its file only at the end, and the 5 examples are canonical (PLAN O37). M6 (a negative fixture that passes adds no failure) is equivalent, because the 3 fixtures fail as given. |

The MCP `fmt` row covers a UTF-8 diagnostic truncated within a character: the reply replaces the invalid byte with `\ufffd`. The `kit-eval` row rejects embedded NUL bytes in both argument positions and then accepts a valid call. It also builds a program with a family result, captures the O24 note in a successful tool result and keeps process stderr empty.

Before emitting canonical text, `fmt` checks the source size limit and reparses the result. The C regressions in `test/fronttool.c` cover nested Sigma types, expanded Pi and lambda binder groups, and a canonical result larger than the source size limit. These cases refuse with a diagnostic and no canonical output; the parser and source size limits stay in force.

`src/front/dkg.c` deals and reshares the keys `keyOpen` and `keyAppend`:
a Pedersen DKG with Feldman commitments and a Schnorr proof for each dealt
secret. The reshare keeps the public key. `thr_deal` in
`src/front/threshold.c` is a trusted dealer. It is for test vectors only.

Initial DKG combination requires the agreed threshold and member count
from trusted caller state and one verified contribution from every member.
The caller must authenticate dealer identities, reliably broadcast the same
commitments to all members, and deliver the private shares over authenticated
confidential channels, as in the [FROST DKG protocol](https://github.com/ZcashFoundation/frost/blob/main/book/src/tutorial/dkg.md).
Quorum resharing instead uses the old threshold and verification keys.

The FROST signing of `src/front/frost.c` (S7) needs the same caller
guarantees. The caller must authenticate the signers and send the same
round 1 list of commitments to every signer (`src/front/dkg.h`).
