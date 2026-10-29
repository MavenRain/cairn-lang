#!/bin/sh
# The kit gate. `make check` runs it from the kit directory after the build.
set -eu
fail=0
count=0
# kit LIST FILE prints --kit when FILE is a line of LIST. A row whose text uses a
# kit name or a Nat or word literal is a kit row (orbit-lang S3); the others run
# in guest mode.
kit() {
  if [ -f "$1" ] && rg -q -x -F "$2" "$1"; then echo --kit; fi
}
for prog in examples/*.lang; do
  out=$(build/langc $(kit examples/kit.txt "${prog#examples/}") check "$prog" 2>&1) || true
  count=$((count + 1))
  if [ "$out" != "ok" ]; then
    echo "FAIL $prog: $out"
    fail=$((fail + 1))
  fi
done
echo "examples: $count checked, $fail failed"

tmp=${TMPDIR:-/tmp}/langc-gate.$$
mkdir -p "$tmp"
awk 'BEGIN { printf "def deep : Nat := " } { } END {
  for (i = 0; i < 1100; i++) printf "("; printf "1"; for (i = 0; i < 1100; i++) printf ")"; print "" }' </dev/null >"$tmp/deep.lang"

# refusals DIR LABEL: each line of DIR/expect.txt is `FILE CODE`. `langc check`
# must exit 1 and print `langc: CODE: ...` on stderr.
refusals() {
  checked=0
  while read -r file code; do
    path="$1/$file"
    [ "$file" = "deep.lang" ] && path="$tmp/deep.lang"
    status=0
    build/langc $(kit "$1/kit.txt" "$file") check "$path" 2>"$tmp/err" >/dev/null || status=$?
    first=$(head -n 1 "$tmp/err")
    checked=$((checked + 1))
    case "$status:$first" in
      "1:langc: $code: "*) ;;
      *) echo "FAIL $file: want exit 1 and $code, got exit $status: $first"; fail=$((fail + 1)) ;;
    esac
  done <"$1/expect.txt"
  echo "$2 refusals: $checked checked"
}
refusals test/parse parse
refusals test/check check

# A REFUSE_KIT row refuses a kit item in guest mode only: `langc --kit check`
# must print ok.
kit_only=0
while read -r file code; do
  [ "$code" = "REFUSE_KIT" ] || continue
  status=0
  out=$(build/langc --kit check "test/check/$file" 2>&1) || status=$?
  kit_only=$((kit_only + 1))
  if [ "$status:$out" != "0:ok" ]; then
    echo "FAIL $file: want ok under --kit, got exit $status: $out"
    fail=$((fail + 1))
  fi
done <test/check/expect.txt
echo "kit-only refusals: $kit_only checked"

# A refl refusal shows both normal forms.
build/langc $(kit test/check/kit.txt bad-refl.lang) check test/check/bad-refl.lang 2>"$tmp/err" >/dev/null || true
if ! rg -q -F 'the path at i1 is 1201, but the type needs 1200' "$tmp/err"; then
  echo "FAIL bad-refl.lang: the message does not show both normal forms"
  fail=$((fail + 1))
fi

# A REFUSE_TIME refusal names the op-count form (S3, O3).
time_hint=0
build/langc $(kit test/check/kit.txt time-block.lang) check test/check/time-block.lang 2>"$tmp/err" >/dev/null || true
time_hint=$((time_hint + 1))
if ! rg -q -F 'count operations: a position is `len r` (an OpPos) and a deadline is a count of later operations' "$tmp/err"; then
  echo "FAIL time-block.lang: the message does not name the op-count form"
  fail=$((fail + 1))
fi
echo "time hint: $time_hint checked"

# Each line of test/eval/expect.txt is `FILE NAME [ARGS] => OUTPUT`. `langc eval`
# must exit 0 and print OUTPUT. A trap also prints `langc: EVAL_OVERFLOW: NAME: ...`
# on stderr.
evals=0
while IFS= read -r line; do
  want=${line#* => }
  set -- ${line%% => *}
  file=$1
  name=$2
  shift 2
  status=0
  got=$(build/langc $(kit examples/kit.txt "$file.lang") eval "examples/$file.lang" "$name" "$@" 2>"$tmp/err") || status=$?
  first=$(head -n 1 "$tmp/err")
  want_err=""
  [ "$want" = "trap" ] && want_err="langc: EVAL_OVERFLOW: $name: "
  evals=$((evals + 1))
  case "$status|$got|$first" in
    "0|$want|$want_err"*) ;;
    *) echo "FAIL eval $file $name: want $want, got exit $status: $got $first"; fail=$((fail + 1)) ;;
  esac
done <test/eval/expect.txt
echo "evals: $evals checked"

# eval_refused STATUS CODE NAME [ARGS]: `langc --kit eval examples/entries.lang
# NAME ARGS` must exit STATUS and print `langc: CODE: NAME: ...` on stderr.
eval_refused() {
  want_status=$1
  code=$2
  shift 2
  status=0
  build/langc --kit eval examples/entries.lang "$@" 2>"$tmp/err" >/dev/null || status=$?
  case "$status:$(head -n 1 "$tmp/err")" in
    "$want_status:langc: $code: $1: "*) ;;
    *) echo "FAIL eval $*: want exit $want_status and $code, got exit $status"; fail=$((fail + 1)) ;;
  esac
}
eval_refused 2 EVAL_ARGS addPrice 1
eval_refused 2 EVAL_ARGS addPrice x 1
eval_refused 2 EVAL_ARGS addPrice 18446744073709551616 1
eval_refused 2 EVAL_ARGS pick 2 4 9
eval_refused 1 EVAL_ENTRY nope

# Each line of test/prove/expect.txt is `FILE NAME [POLL] => WORDS`. `langc prove`
# must exit 0 and print the words of WORDS, one per line (S8 B2c).
proves=0
while IFS= read -r line; do
  want=${line#* => }
  set -- ${line%% => *}
  file=$1
  name=$2
  shift 2
  status=0
  build/langc $(kit examples/kit.txt "$file.lang") prove "examples/$file.lang" "$name" "$@" >"$tmp/out" 2>"$tmp/err" || status=$?
  got=$(tr '\n' ' ' <"$tmp/out")
  got=${got% }
  proves=$((proves + 1))
  case "$status|$got" in
    "0|$want") ;;
    *) echo "FAIL prove $file $name: want $want, got exit $status: $got"; fail=$((fail + 1)) ;;
  esac
done <test/prove/expect.txt
echo "proves: $proves checked"

# prove_refused STATUS CODE FILE NAME [POLL]: `langc prove examples/FILE.lang
# NAME POLL` must exit STATUS and print `langc: CODE: NAME: ...` on stderr.
prove_refused() {
  want_status=$1
  code=$2
  file=$3
  shift 3
  status=0
  build/langc prove "examples/$file.lang" "$@" 2>"$tmp/err" >/dev/null || status=$?
  case "$status:$(head -n 1 "$tmp/err")" in
    "$want_status:langc: $code: $1: "*) ;;
    *) echo "FAIL prove $file $*: want exit $want_status and $code, got exit $status"; fail=$((fail + 1)) ;;
  esac
}
prove_refused 1 PROVE_BALLOT dao w0
prove_refused 2 EVAL_ARGS dao b0 x

# The target (S8, Q-S8-8). `langc build examples/dao.lang -o OUT` must exit 0,
# write nothing on stdout and write a valid module. build_refused FLAGS CODE
# FILE: `langc FLAGS build FILE -o OUT` must exit 1, print `langc: CODE: ...` on
# stderr and write nothing on stdout.
status=0
build/langc $(kit examples/kit.txt dao.lang) build examples/dao.lang -o "$tmp/dao.wasm" >"$tmp/out" 2>"$tmp/err" || status=$?
case "$status:$(wc -c <"$tmp/out" | tr -d ' ')" in
  0:0) wasm-validate "$tmp/dao.wasm" 2>"$tmp/err" || { echo "FAIL build dao: the module is not valid: $(head -n 1 "$tmp/err")"; fail=$((fail + 1)); } ;;
  *) echo "FAIL build dao: want exit 0 and no stdout, got exit $status: $(head -n 1 "$tmp/err")"; fail=$((fail + 1)) ;;
esac
builds=0
build_refused() {
  flags=$1
  code=$2
  file=$3
  status=0
  build/langc $flags build "$file" -o "$tmp/refused.wasm" >"$tmp/out" 2>"$tmp/err" || status=$?
  builds=$((builds + 1))
  case "$status:$(head -n 1 "$tmp/err"):$(wc -c <"$tmp/out" | tr -d ' ')" in
    "1:langc: $code: "*":0") ;;
    *) echo "FAIL build $file: want exit 1 and $code, got exit $status: $(head -n 1 "$tmp/err")"; fail=$((fail + 1)) ;;
  esac
}
build_refused --kit REFUSE_RUNTIME_CLOSURE test/ir/closure.lang
build_refused --kit REFUSE_RUNTIME_TREE test/ir/tree.lang
build_refused --kit REFUSE_LOWER examples/zk.lang
build_refused --kit REFUSE_LOWER test/ir/transp.lang
build_refused --kit REFUSE_TARGET_SECRET test/ir/secret-key.lang
build_refused --kit REFUSE_TARGET_SECRET test/ir/secret-sign.lang
build_refused "" REFUSE_KIT test/ir/comb.lang
build_refused --kit REFUSE_RUNTIME_TREE test/ir/tree-elim.lang
# O26 (Q-S9-11): the elim limit of family_fold refuses, not the closure
# check. build_refused checks the code only, so check the message too.
case "$(head -n 1 "$tmp/err")" in
  "langc: REFUSE_RUNTIME_TREE: t: a fold over a tree of Word must exist at run time (HOST-LIMIT)") ;;
  *) echo "FAIL build test/ir/tree-elim.lang: want the Word tree message, got: $(head -n 1 "$tmp/err")"; fail=$((fail + 1)) ;;
esac
# O24 (Q-S9-8): a def with scalar params and a family result has no target
# form. The build skips it and writes one note for it on stderr.
status=0
build/langc $(kit examples/kit.txt state-no-history.lang) build examples/state-no-history.lang -o "$tmp/note.wasm" >"$tmp/out" 2>"$tmp/err" || status=$?
case "$status:$(head -n 1 "$tmp/err")" in
  "0:langc: note: mk: a family result has no target form (O24)") ;;
  *) echo "FAIL build examples/state-no-history.lang: want exit 0 and the mk note, got exit $status: $(head -n 1 "$tmp/err")"; fail=$((fail + 1)) ;;
esac
# O24 (Q-S9-10): a Prod or a List result is a family result too. The
# pairOf def of examples/functions.lang gets the note.
status=0
build/langc $(kit examples/kit.txt functions.lang) build examples/functions.lang -o "$tmp/note.wasm" >"$tmp/out" 2>"$tmp/err" || status=$?
case "$status:$(head -n 1 "$tmp/err")" in
  "0:langc: note: pairOf: a family result has no target form (O24)") ;;
  *) echo "FAIL build examples/functions.lang: want exit 0 and the pairOf note, got exit $status: $(head -n 1 "$tmp/err")"; fail=$((fail + 1)) ;;
esac
echo "build refusals: $builds checked"

# The IR (Q-S8-7): each example lowers, with one `func` line for each `abi
# --text` line, and its module is valid (`wasm-validate`, `wasm-objdump -x`).
# An example that does not lower must refuse REFUSE_LOWER with a HOST-LIMIT
# detail (a host proof check, as examples/zk.lang; the secp256k1 ops of part B
# lower). An example with a family-result def exits 0 with an O24 note.
irs=0
host_limits=0
wasms=0
for f in examples/*.lang; do
  k=$(kit examples/kit.txt "${f#examples/}")
  status=0
  build/langc $k ir "$f" >"$tmp/ir.txt" 2>"$tmp/ir.err" || status=$?
  case "$status:$(head -n 1 "$tmp/ir.err")" in
    "1:langc: REFUSE_LOWER: "*"(HOST-LIMIT)"*) host_limits=$((host_limits + 1)); continue ;;
    0: | "0:langc: note: "*"(O24)") ;;
    *) echo "FAIL ir $f: exit $status: $(head -n 1 "$tmp/ir.err")"; fail=$((fail + 1)); continue ;;
  esac
  funcs=$(rg -c '^func ' "$tmp/ir.txt" || echo 0)
  entries=$(build/langc $k abi "$f" --text | rg -c '.' || echo 0)
  [ "$funcs" -eq "$entries" ] || { echo "FAIL ir $f: $funcs funcs for $entries entries"; fail=$((fail + 1)); continue; }
  irs=$((irs + 1))
  w="$tmp/$(basename "$f" .lang).wasm"
  if ! build/langc $k build "$f" -o "$w" 2>"$tmp/w.err" || ! wasm-validate "$w" 2>>"$tmp/w.err" ||
    ! wasm-objdump -x "$w" >/dev/null 2>>"$tmp/w.err"; then
    echo "FAIL wasm $f: $(head -n 1 "$tmp/w.err")"
    fail=$((fail + 1))
    continue
  fi
  wasms=$((wasms + 1))
done
echo "ir: $irs lowered, $host_limits refused (HOST-LIMIT)"
echo "wasm: $wasms modules valid"

# The Wasm differential (Q-S8-7): each test/eval/expect.txt row whose NAME is an
# entry of FILE (a line of its `abi --text`) runs on the module in
# build/wasmtool and must print the row OUTPUT (a trap is a trap). Each FILE
# builds once and runs once, with its rows on stdin.
mkdir -p "$tmp/wd"
while IFS= read -r line; do
  want=${line#* => }
  set -- ${line%% => *}
  file=$1
  shift
  if [ ! -f "$tmp/wd/$file.abi" ]; then
    k=$(kit examples/kit.txt "$file.lang")
    : >"$tmp/wd/$file.abi"
    build/langc $k build "examples/$file.lang" -o "$tmp/wd/$file.wasm" 2>/dev/null &&
      build/langc $k abi "examples/$file.lang" --text >"$tmp/wd/$file.abi" 2>/dev/null || :
  fi
  rg -q "^$1 \(" "$tmp/wd/$file.abi" || continue
  echo "$*" >>"$tmp/wd/$file.in"
  echo "$want" >>"$tmp/wd/$file.want"
done <test/eval/expect.txt
rows=0
mismatches=0
for in_file in "$tmp"/wd/*.in; do
  file=$(basename "$in_file" .in)
  if ! build/wasmtool "$tmp/wd/$file.wasm" "$tmp/wd/$file.abi" <"$in_file" >"$tmp/wd/$file.got" 2>"$tmp/w.err"; then
    echo "FAIL wasmtool $file: $(head -n 1 "$tmp/w.err")"
    fail=$((fail + 1))
    continue
  fi
  rows=$((rows + $(rg -c '.' "$in_file")))
  paste -d '|' "$in_file" "$tmp/wd/$file.want" "$tmp/wd/$file.got" | awk -F '|' '$2 != $3' >"$tmp/bad.txt"
  bad=$(rg -c '.' "$tmp/bad.txt" || echo 0)
  mismatches=$((mismatches + bad))
  [ "$bad" -eq 0 ] || { echo "FAIL wasm differential $file: $bad mismatches, first $(head -n 1 "$tmp/bad.txt")"; fail=$((fail + 1)); }
done
echo "wasm differential: $rows rows, $mismatches mismatches"
# A Flag argument larger than 1 traps (langc eval refuses it with EVAL_ARGS).
got=$(echo 'pick 2 4 9' | build/wasmtool "$tmp/wd/entries.wasm" "$tmp/wd/entries.abi" 2>&1) || true
[ "$got" = "trap" ] || { echo "FAIL wasm pick 2 4 9: want trap, got $got"; fail=$((fail + 1)); }

# The JS differential (S9 B4, Q-S9-6 (a)): the same rows run on the ES module
# of `langc build FILE --js` in build/jstool (under node) and must print the
# same OUTPUT.
js_rows=0
js_bad=0
for in_file in "$tmp"/wd/*.in; do
  file=$(basename "$in_file" .in)
  k=$(kit examples/kit.txt "$file.lang")
  if ! build/langc $k build "examples/$file.lang" --js "$tmp/wd/$file.mjs" 2>"$tmp/w.err" ||
    ! build/jstool "$tmp/wd/$file.mjs" "$tmp/wd/$file.abi" <"$in_file" >"$tmp/wd/$file.js" 2>>"$tmp/w.err"; then
    echo "FAIL jstool $file: $(head -n 1 "$tmp/w.err")"
    fail=$((fail + 1))
    continue
  fi
  js_rows=$((js_rows + $(rg -c '.' "$in_file")))
  paste -d '|' "$in_file" "$tmp/wd/$file.want" "$tmp/wd/$file.js" | awk -F '|' '$2 != $3' >"$tmp/bad.txt"
  bad=$(rg -c '.' "$tmp/bad.txt" || echo 0)
  js_bad=$((js_bad + bad))
  [ "$bad" -eq 0 ] || { echo "FAIL js differential $file: $bad mismatches, first $(head -n 1 "$tmp/bad.txt")"; fail=$((fail + 1)); }
done
echo "js differential: $js_rows rows, $js_bad mismatches"
got=$(echo 'pick 2 4 9' | build/jstool "$tmp/wd/entries.mjs" "$tmp/wd/entries.abi" 2>&1) || true
[ "$got" = "trap" ] || { echo "FAIL js pick 2 4 9: want trap, got $got"; fail=$((fail + 1)); }

# The test/ir modules (A7 piece (c)): each builds in --kit mode to a valid
# module. Each row `FILE NAME ARGS` below runs on the module of test/ir/FILE.lang
# in build/wasmtool and must print the `langc --kit eval` output (a trap is
# `trap`).
mkdir -p "$tmp/ir"
ir_mods=0
for m in comb lazy spine proof-trap memname; do
  if ! build/langc --kit build "test/ir/$m.lang" -o "$tmp/ir/$m.wasm" 2>"$tmp/w.err" ||
    ! wasm-validate "$tmp/ir/$m.wasm" 2>>"$tmp/w.err" ||
    ! build/langc --kit abi "test/ir/$m.lang" --text >"$tmp/ir/$m.abi" 2>>"$tmp/w.err"; then
    echo "FAIL test/ir wasm $m: $(head -n 1 "$tmp/w.err")"
    fail=$((fail + 1))
    continue
  fi
  ir_mods=$((ir_mods + 1))
  : >"$tmp/ir/$m.in"
  : >"$tmp/ir/$m.want"
done
while IFS= read -r line; do
  set -- $line
  m=$1
  shift
  [ -f "$tmp/ir/$m.in" ] || continue
  want=$(build/langc --kit eval "test/ir/$m.lang" "$@" 2>/dev/null) || :
  [ -n "$want" ] || { echo "FAIL test/ir eval $line: no output"; fail=$((fail + 1)); continue; }
  echo "$*" >>"$tmp/ir/$m.in"
  echo "$want" >>"$tmp/ir/$m.want"
done <<'EOF'
comb snocChain 0
comb snocChain 1
comb snocChain 3
comb snocRun 0
comb snocRun 1
comb snocRun 3
comb consChain 0
comb consChain 1
comb consChain 3
comb consRun 0
comb consRun 1
comb consRun 3
comb mixChain 0
comb mixChain 1
comb mixChain 3
comb mixRun 0
comb mixRun 1
comb mixRun 3
comb snocLen 0
comb snocLen 1
comb snocLen 3
lazy lazy 3
spine trapHead 3
spine trapSum 3
spine filterTrap 3
spine bindTrap 3
proof-trap proofMove 0
proof-trap proofMove 18446744073709551615
proof-trap proofSymm 18446744073709551615
proof-trap proofTrans 18446744073709551615
proof-trap proofCong 18446744073709551615
memname memory
EOF
ir_rows=0
ir_bad=0
for m in comb lazy spine proof-trap memname; do
  [ -f "$tmp/ir/$m.in" ] || continue
  if ! build/wasmtool "$tmp/ir/$m.wasm" "$tmp/ir/$m.abi" <"$tmp/ir/$m.in" >"$tmp/ir/$m.got" 2>"$tmp/w.err"; then
    echo "FAIL wasmtool test/ir/$m.lang: $(head -n 1 "$tmp/w.err")"
    fail=$((fail + 1))
    continue
  fi
  ir_rows=$((ir_rows + $(rg -c '.' "$tmp/ir/$m.in" || echo 0)))
  paste -d '|' "$tmp/ir/$m.in" "$tmp/ir/$m.want" "$tmp/ir/$m.got" | awk -F '|' '$2 != $3' >"$tmp/bad.txt"
  bad=$(rg -c '.' "$tmp/bad.txt" || echo 0)
  ir_bad=$((ir_bad + bad))
  [ "$bad" -eq 0 ] || { echo "FAIL test/ir wasm $m: $bad mismatches, first $(head -n 1 "$tmp/bad.txt")"; fail=$((fail + 1)); }
done
echo "test/ir wasm: $ir_mods modules valid, $ir_rows rows, $ir_bad mismatches"

# The test/ir rows on the JS module (S9 B4): build/jstool must print the same
# `langc --kit eval` output.
ir_js_rows=0
ir_js_bad=0
for m in comb lazy spine proof-trap memname; do
  [ -f "$tmp/ir/$m.in" ] || continue
  if ! build/langc --kit build "test/ir/$m.lang" --js "$tmp/ir/$m.mjs" 2>"$tmp/w.err" ||
    ! build/jstool "$tmp/ir/$m.mjs" "$tmp/ir/$m.abi" <"$tmp/ir/$m.in" >"$tmp/ir/$m.js" 2>>"$tmp/w.err"; then
    echo "FAIL jstool test/ir/$m.lang: $(head -n 1 "$tmp/w.err")"
    fail=$((fail + 1))
    continue
  fi
  ir_js_rows=$((ir_js_rows + $(rg -c '.' "$tmp/ir/$m.in" || echo 0)))
  paste -d '|' "$tmp/ir/$m.in" "$tmp/ir/$m.want" "$tmp/ir/$m.js" | awk -F '|' '$2 != $3' >"$tmp/bad.txt"
  bad=$(rg -c '.' "$tmp/bad.txt" || echo 0)
  ir_js_bad=$((ir_js_bad + bad))
  [ "$bad" -eq 0 ] || { echo "FAIL test/ir js $m: $bad mismatches, first $(head -n 1 "$tmp/bad.txt")"; fail=$((fail + 1)); }
done
echo "test/ir js: $ir_js_rows rows, $ir_js_bad mismatches"
json_want='{"entries":[{"name":"addX","params":["u256","u256","u256","u256"],"result":"u256"},{"name":"addY","params":["u256","u256","u256","u256"],"result":"u256"},{"name":"mulX","params":["u256","u256","u256"],"result":"u256"},{"name":"mulY","params":["u256","u256","u256"],"result":"u256"},{"name":"sigOk","params":["u256","u256","u256","u256","u256"],"result":"flag"}]}'
json_got=$(build/langc --kit abi test/wasm/secp.lang 2>&1) || json_got="exit $?"
[ "$json_got" = "$json_want" ] || { echo "FAIL json abi: build/langc --kit abi test/wasm/secp.lang is not the JSON abi line"; fail=$((fail + 1)); }
json_rc=0
build/langc --kit abi test/wasm/secp.lang --bad > /dev/null 2> "$tmp/json-abi.err" || json_rc=$?
json_msg=$(head -n 1 "$tmp/json-abi.err")
case "$json_rc $json_msg" in
  "2 "*USAGE*"abi takes only --text"*) ;;
  *) echo "FAIL json abi: --bad gave exit $json_rc, line 1: $json_msg"; fail=$((fail + 1)) ;;
esac
echo "json abi: 2 checked"

# The differential (S9 B2) in the guest. A row of test/contract/diff.txt is
# `PROG START GUEST PERMUTED BALLOTS NAME... => OUTCOME SELF`: `langc eval
# examples/PROG.lang` of GUEST (the canonical config) and of PERMUTED (a config
# outside the act orbit) must exit 0 and print the Outcome constructor of index
# OUTCOME. The rows are read on fd 3, so langc cannot read them.
# ctor_index TEXT prints the constructor index of TEXT: each leading `inr (` adds
# 1, then `inl unit` adds 0 and `inr unit` adds 1. Other text prints -.
ctor_index() {
  _text=$1
  _index=0
  while :; do
    case $_text in
      "inr ("*) _text=${_text#"inr ("}; _index=$((_index + 1)) ;;
      *) break ;;
    esac
  done
  case ${_text%%")"*} in
    "inl unit") echo "$_index" ;;
    "inr unit") echo $((_index + 1)) ;;
    *) echo - ;;
  esac
}
diffs=0
while read -r line <&3; do
  case $line in --*|'') continue ;; esac
  set -- ${line%% => *}
  want=${line#* => }
  for name in "$3" "$4"; do
    status=0
    got=$(build/langc $(kit examples/kit.txt "$1.lang") eval "examples/$1.lang" "$name" 2>"$tmp/err") || status=$?
    diffs=$((diffs + 1))
    if [ "$status" -ne 0 ] || [ "$(ctor_index "$got")" != "${want%% *}" ]; then
      echo "FAIL differential $1 $name: want outcome ${want%% *}, got exit $status: $got $(head -n 1 "$tmp/err")"
      fail=$((fail + 1))
    fi
  done
done 3<test/contract/diff.txt
echo "differential: $diffs guest evals checked"

status=0
build/langc 2>/dev/null || status=$?
[ "$status" -eq 2 ] || { echo "FAIL usage: want exit 2, got $status"; fail=$((fail + 1)); }
status=0
build/langc --kit 2>/dev/null || status=$?
[ "$status" -eq 2 ] || { echo "FAIL usage --kit: want exit 2, got $status"; fail=$((fail + 1)); }
status=0
build/langc check "$tmp/missing.lang" 2>/dev/null || status=$?
[ "$status" -eq 2 ] || { echo "FAIL missing file: want exit 2, got $status"; fail=$((fail + 1)); }
rm -rf "$tmp"

if rg -n '\x{2013}|\x{2014}' . >/dev/null; then
  echo "FAIL: an em-dash or en-dash is in the kit"
  fail=$((fail + 1))
fi
echo "gate: $fail failures"
[ "$fail" -eq 0 ]
