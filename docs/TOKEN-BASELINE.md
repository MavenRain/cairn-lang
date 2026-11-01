# Token baseline (S15)

This file compares the tokens of the worked examples G1 to G5 in
cairn-lang, Python and Rust (PLAN 7, row A6). The counts in this file are
an offline estimate: ceil(bytes / 4) of each file (Q-S15-10 (c)). They are
not tokenizer counts. The API route (`make tokens`: the Anthropic
`count_tokens` endpoint with the newest Claude model, Q-S0-8 (a), Q-S15-1
(a)) is not run, because no API Console key is available. `make check`
checks the file hashes and the estimate block in this file, with no key
and no network (Q-S15-7 (a)).

Status: the estimate is from 2026-11-01 (`build/tokcount --estimate`). The
API route is not run.

## The task

Each G is one worked example of `skills/cairn-lang/SKILL.md` (S14). The 3
programs of a G do the same task and print the same lines:

| G | task | eval rows |
| :--- | :--- | :--- |
| G1 | an append-only record of content hashes | `verifyHit` |
| G2 | the check of a threshold Schnorr signature (kit mode) | `appGood`, `appBad` |
| G3 | the sum of sealed votes (the sum stays sealed) | `tallyThree` |
| G4 | the amendment of `tAppend` from 2 to 3 | `g4Append`, `g4Refused` |
| G5 | a self-amendment: the amendment of `tAmend` | `g5Open`, `g5Refused` |

## The files

- `examples/skill/g1-record.lang` to `g5-self-amend.lang`: the cairn-lang
  programs (S14). G2 runs in kit mode. The count is of the `.lang` file
  only: `kit.txt` is a list of 1 file name, not program text (Q-S15-3
  (a)).
- `test/baseline/g1-record.py` to `g5-self-amend.py`: the same tasks in
  Python, with the standard library only (Q-S15-4 (a)).
- `test/baseline/rust/src/bin/g1.rs` to `g5.rs`: the same tasks in Rust,
  1 crate with 5 bins (Q-S15-5 (a)). All bins use `sha2` 0.10. Only G2
  uses `k256` 0.13 (the secp256k1 group). `Cargo.lock` pins the versions.

Each Python and each Rust program prints the eval lines of its G in the
SKILL.md order. The lines must be equal, byte for byte, to the output of
`langc eval` (`langc --kit eval` for G2). `make tokens` checks this before
it counts.

## The method

- Estimate: `build/tokcount --estimate` reads the full text of each file,
  comments included. The estimate of a file is ceil(bytes / 4), that is
  (bytes + 3) / 4 in integers (Q-S15-10 (c)). It needs no key and no
  network. An estimate is not a tokenizer count: it has no model and no
  message frame.
- Request (the API route): `POST /v1/messages/count_tokens` with 1 user
  message. The message is the full text of 1 file, comments included. The
  reply is `{"input_tokens": N}`.
- Model (the API route): the model with the newest `created_at` in the
  reply of `GET /v1/models` on the run date (Q-S15-2 (a)).
- Frame (the API route): N includes the tokens of the message frame. Thus
  `make tokens` also counts a fixed 1-byte message, `x`. The net count of
  a file is N minus the count of that message (Q-S15-9 (a)).
- Ratios: each ratio divides 2 estimates now (2 net counts on the API
  route), half up, with 2 decimals. One line gives each Python file and
  each Rust file over the cairn-lang file of the same G. The last 2 lines
  give the sum of each language over the sum of the 5 cairn-lang files.
  Each file is a full alternative, thus the table has no total row.
- Hashes: the SHA-256 of each counted file. `make check` runs
  `build/tokcount --check docs/TOKEN-BASELINE.md`. Each hash in the table
  must be the hash of the file now. `make check` also compares the lines
  between the markers with the output of `build/tokcount --estimate`.
  Thus a changed program fails the gate until a new estimate block
  replaces the old one.

## The run

The estimate: from the repo root, offline, with no key:

```
build/tokcount --estimate
```

The output is the block between the markers below, byte for byte.

The API route (not run; it needs an API Console key): from the repo root,
outside the sandbox, with `ANTHROPIC_API_KEY` set:

```
make tokens
```

The rule does these steps:

1. It builds the Rust crate (`RUST_BUILD`, by default `cargo build -j 2`).
2. It runs each program and compares its output with `langc eval`.
3. It gets the model list and selects the model (`build/tokcount --model`).
4. It sends 1 request for the frame message and 1 request for each of the
   15 files (`build/tokcount --request` makes each body). It keeps each
   reply in `build/tokens/`.
5. It writes the table with `build/tokcount --table build/tokens`.

Now the lines between the markers below come from `build/tokcount
--estimate`. On the API route, step 5 replaces them.

## The table

Model: none (an offline estimate). Date: 2026-11-01.

<!-- tokcount begin -->
| file | bytes | estimate | sha256 |
| :--- | ---: | ---: | :--- |
| examples/skill/g1-record.lang | 1340 | 335 | ba40215c8ac7f37c92696b63016a4eeb92e9bbd729d7be033cf04797e21b10f1 |
| test/baseline/g1-record.py | 1100 | 275 | 1b52f19bf8f9d1f1a78af585808dd93b273992bfec39905ec7c160079baa5252 |
| test/baseline/rust/src/bin/g1.rs | 1432 | 358 | d42c4a8b4a9ba92b6faa326410bfa33a5b4c2e39c39b0ac499b3e0b35667cde8 |
| examples/skill/g2-committee.lang | 1372 | 343 | 8479f4b93373001d9524d58cf9128b6adebc29f10cc6ded2be1dd49998c914d5 |
| test/baseline/g2-committee.py | 3017 | 755 | 17c7565f337e16d2d5462c7a62db8ec92dc823a08146eac0206b37474ce91c1d |
| test/baseline/rust/src/bin/g2.rs | 3469 | 868 | cf58163755dd58ce6681af02b538b8cca227d7d634c7d5a8681b18f8eaa98a6c |
| examples/skill/g3-vote.lang | 2179 | 545 | 5108720a865a726880d8134d916aa41f696feb2e62df39f54e7898d116f5a6a0 |
| test/baseline/g3-vote.py | 1426 | 357 | 3e64d7cc1d0affb942891c720b0ed6271015922d727ff622c1ee2c125644f197 |
| test/baseline/rust/src/bin/g3.rs | 1854 | 464 | b63125be78fd0ed0fdb62e4aa5f89207dca5473e28d8b77813930950a363e718 |
| examples/skill/g4-amend.lang | 1357 | 340 | adc40736b77d4f66d871050523422c3782585267b6e1eb4eb7e86e1de9d12d8c |
| test/baseline/g4-amend.py | 1278 | 320 | cc46e5c907c7865725c591e5f8eccf752f356bdb0ca2b9954dde1a96be7bb3c1 |
| test/baseline/rust/src/bin/g4.rs | 1785 | 447 | 6bd8c0af0cac498c641cadd2feb0b632a6d83e5b7d3874d78ca3a7f4af7b0a46 |
| examples/skill/g5-self-amend.lang | 1504 | 376 | d8bd0e5756f6d71dd4f6de46dc1ad9b71742ddd22f88d528535ce2157a9aa2ed |
| test/baseline/g5-self-amend.py | 1454 | 364 | 73782953291e7afea53d5a490ef44301814659fb9eaa90f9fcf763f6ef577e00 |
| test/baseline/rust/src/bin/g5.rs | 2077 | 520 | 861cd3f7475e628be915c41853d0f64f272766ba8c2f705d6eac580e564447f8 |

estimate: ceil(bytes / 4) of each file; this is not a tokenizer count.

ratio of the estimates (test/baseline/g1-record.py over examples/skill/g1-record.lang): 0.82
ratio of the estimates (test/baseline/rust/src/bin/g1.rs over examples/skill/g1-record.lang): 1.07
ratio of the estimates (test/baseline/g2-committee.py over examples/skill/g2-committee.lang): 2.20
ratio of the estimates (test/baseline/rust/src/bin/g2.rs over examples/skill/g2-committee.lang): 2.53
ratio of the estimates (test/baseline/g3-vote.py over examples/skill/g3-vote.lang): 0.66
ratio of the estimates (test/baseline/rust/src/bin/g3.rs over examples/skill/g3-vote.lang): 0.85
ratio of the estimates (test/baseline/g4-amend.py over examples/skill/g4-amend.lang): 0.94
ratio of the estimates (test/baseline/rust/src/bin/g4.rs over examples/skill/g4-amend.lang): 1.31
ratio of the estimates (test/baseline/g5-self-amend.py over examples/skill/g5-self-amend.lang): 0.97
ratio of the estimates (test/baseline/rust/src/bin/g5.rs over examples/skill/g5-self-amend.lang): 1.38
ratio of the estimates (the 5 Python files over the 5 cairn-lang files): 1.07
ratio of the estimates (the 5 Rust files over the 5 cairn-lang files): 1.37
<!-- tokcount end -->

Result: for the 5 G together, the estimate of the Python files is 1.07
times the estimate of the cairn-lang files, and the estimate of the Rust
files is 1.37 times the estimate of the cairn-lang files.

## Notes

- Versions: the estimate uses no tool version; it reads only the bytes of
  the files. On the API route, the `python3`, `rustc` and `cargo`
  versions come from `build/tokens/versions.txt`.
- A `make tokens` run that replaces the estimate block must also remove
  the estimate compare of `make check` (the 2 Makefile lines after
  `build/tokcount --check`).
- The G2 record in Rust holds `u64` entries. The standard library has no
  256-bit integer with a decimal form; the rows use 42.
- The 256-bit constants in Rust use `U256::from_be_hex`, which `k256`
  exports again.
- Cargo dependencies apply to the full crate. Thus `k256` builds with each
  bin, but only `g2.rs` uses it.
- The Rust build gives 3 `dead_code` warnings: the fields that no code
  reads (the G3 ballot, the G4 and G5 polity). The Python programs keep
  the same fields.
