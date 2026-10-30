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
