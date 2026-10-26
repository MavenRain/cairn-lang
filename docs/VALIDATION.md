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
