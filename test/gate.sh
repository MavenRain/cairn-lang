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

# The target (S2). The EVM writer is not ported and the cairn target comes in
# S8, so `langc build examples/dao.lang` must exit 1, print `langc:
# REFUSE_LOWER: -: ...target pending S8...` on stderr and write nothing on
# stdout.
builds=0
status=0
build/langc $(kit examples/kit.txt dao.lang) build examples/dao.lang >"$tmp/out" 2>"$tmp/err" || status=$?
builds=$((builds + 1))
case "$status:$(head -n 1 "$tmp/err"):$(wc -c <"$tmp/out" | tr -d ' ')" in
  "1:langc: REFUSE_LOWER: -: "*"target pending S8"*":0") ;;
  *) echo "FAIL build dao: want exit 1, REFUSE_LOWER and target pending S8, got exit $status: $(head -n 1 "$tmp/err")"; fail=$((fail + 1)) ;;
esac
echo "build refusals: $builds checked"

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
