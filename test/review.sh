#!/bin/sh
# Regression checks for the staged compiler review.
set -eu
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lang-review.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat >"$tmp/capture.lang" <<'EOF'
def keep : Nat -> Nat -> Nat := fun x y => x
def value : Unit -> Nat -> Nat -> Nat := fun u y => keep y
EOF
normal=$(build/langc --kit eval "$tmp/capture.lang" value)
printf 'def value : Unit -> Nat -> Nat -> Nat := %s\ndef answer : Nat := value unit 4 9\n' "$normal" >"$tmp/roundtrip.lang"
test "$(build/langc --kit eval "$tmp/roundtrip.lang" answer)" = 4

# Reducing a closure can expose a constructor with the binder's old name.
cat >"$tmp/constructor.lang" <<'EOF'
def constant : Unit -> Flag := fun u => flagYes
def value : Unit -> Flag := fun flagYes => constant flagYes
EOF
normal=$(build/langc --kit eval "$tmp/constructor.lang" value)
printf 'def value : Unit -> Flag := %s\ndef answer : Nat := flagIf (value unit) 1 2\n' "$normal" >"$tmp/roundtrip.lang"
test "$(build/langc --kit eval "$tmp/roundtrip.lang" answer)" = 1

# A generated eta binder must also print as a source identifier.
printf 'def value : Unit -> Nat -> Nat -> Nat := fun u => natAdd\n' >"$tmp/eta.lang"
normal=$(build/langc --kit eval "$tmp/eta.lang" value)
printf 'def value : Unit -> Nat -> Nat -> Nat := %s\ndef answer : Nat := value unit 4 9\n' "$normal" >"$tmp/roundtrip.lang"
test "$(build/langc --kit eval "$tmp/roundtrip.lang" answer)" = 13

# More than 64 live binders used to read past the printer's name table.
awk 'BEGIN {
  printf "def value : "; for (i = 0; i < 70; i++) printf "Unit -> ";
  printf "Unit := fun "; for (i = 0; i < 70; i++) printf "u%d ", i;
  print "=> u69"
}' >"$tmp/binders.lang"
awk 'BEGIN {
  for (i = 0; i < 70; i++) printf "fun u%d => ", i;
  print "u69"
}' >"$tmp/want"
build/langc --kit eval "$tmp/binders.lang" value >"$tmp/got"
cmp "$tmp/want" "$tmp/got"

# bind preserves a Sum error and can change the success type.
cat >"$tmp/sum.lang" <<'EOF'
def input : Sum Nat Nat := inl 7
def output : Sum Nat Flag := bind input (fun x => inr flagYes)
def right : Sum Nat Nat := inr 7
def mapped : Sum Nat Flag := bind right (fun x => inr flagYes)
EOF
test "$(build/langc --kit eval "$tmp/sum.lang" output)" = 'inl 7'
test "$(build/langc --kit eval "$tmp/sum.lang" mapped)" = 'inr flagYes'
# A truncated normal form must not be reported as successful evaluation.
printf 'def value : List Nat := fold (fun xs => cons 0 xs) nil 10000\n' >"$tmp/large.lang"
status=0
build/langc --kit eval "$tmp/large.lang" value >"$tmp/got" 2>"$tmp/err" || status=$?
test "$status" = 1
test ! -s "$tmp/got"
case "$(cat "$tmp/err")" in
  'langc: EVAL_PRINT:'*) ;;
  *) exit 1 ;;
esac
# Calls accept each scalar type, and invalid literals keep the CLI exit codes.
cat >"$tmp/args.lang" <<'EOF'
state State := makeState (count : Nat)
def init : State := makeState 0
def natView : Env -> State -> Nat -> Nat := fun e s n => n
def flagView : Env -> State -> Flag -> Flag := fun e s b => b
def wordView : Env -> State -> U256 -> U256 := fun e s w => w
def addrView : Env -> State -> Addr -> Addr := fun e s a => a
EOF
caller=0x00000000000000000000000000000000000000bb
cat >"$tmp/args.script" <<EOF
-- A comment does not increment the call number.

$caller natView 18446744073709551615
$caller flagView 1
$caller wordView 0x10u
$caller addrView $caller
EOF
cat >"$tmp/want" <<EOF
1 natView = 18446744073709551615
2 flagView = flagYes
3 wordView = 16u
4 addrView = $caller
state makeState 0
EOF
build/langc --kit run "$tmp/args.lang" "$tmp/args.script" >"$tmp/got"
cmp "$tmp/want" "$tmp/got"
for call in 'natView 18446744073709551616' 'flagView 2' 'wordView 5' 'addrView 0xbb'; do
  printf '%s %s\n' "$caller" "$call" >"$tmp/args.script"
  status=0
  build/langc --kit run "$tmp/args.lang" "$tmp/args.script" >"$tmp/got" 2>"$tmp/err" || status=$?
  test "$status" = 2
  case "$(cat "$tmp/err")" in
    'langc: EVAL_ARGS:'*) ;;
    *) exit 1 ;;
  esac
done
# The sample domain is gone (S2, R21): its names are free for a program.
printf 'def red : Nat := 1\ndef Color : Nat := 2\ndef lineTotal : Nat := natAdd red Color\n' >"$tmp/free.lang"
test "$(build/langc --kit eval "$tmp/free.lang" lineTotal)" = 3

# Exercise the token output recipes without Python, Rust or the network.
# A producer that prints the right bytes and then fails must be refused.
token_root="$tmp/tokens"
mkdir -p "$token_root/build/tokens" "$token_root/bin" \
  "$token_root/test/baseline/rust/target/debug"
make -n tokens TOKEN_PROGRAMS=g2-committee \
  'TOKEN_EVALS=g2-committee:appGood g2-committee:appBad' >"$tmp/token-make"
awk '/^for row in / || /^for g in /' "$tmp/token-make" >"$tmp/token-recipes"
test "$(wc -l <"$tmp/token-recipes")" -eq 2
cat >"$token_root/bin/producer" <<'EOF'
#!/bin/sh
printf 'some 1\n'
case "$0" in
  */langc)
    case "$*" in *appGood) exit "${TOKEN_LANGC_EXIT:-0}" ;; esac
    ;;
  */python3) printf 'some 1\n'; exit "${TOKEN_PYTHON_EXIT:-0}" ;;
  */g2) printf 'some 1\n'; exit "${TOKEN_RUST_EXIT:-0}" ;;
esac
EOF
chmod +x "$token_root/bin/producer"
cp "$token_root/bin/producer" "$token_root/build/langc"
cp "$token_root/bin/producer" "$token_root/bin/python3"
cp "$token_root/bin/producer" "$token_root/test/baseline/rust/target/debug/g2"
run_token_recipes() {
  rm -f "$token_root/build/tokens/"*
  while IFS= read -r recipe; do
    (cd "$token_root" && PATH="$token_root/bin:$PATH" sh -c "$recipe") || return 1
  done <"$tmp/token-recipes"
}
TOKEN_LANGC_EXIT=0 TOKEN_PYTHON_EXIT=0 TOKEN_RUST_EXIT=0
export TOKEN_LANGC_EXIT TOKEN_PYTHON_EXIT TOKEN_RUST_EXIT
run_token_recipes
for producer in langc python rust; do
  TOKEN_LANGC_EXIT=0 TOKEN_PYTHON_EXIT=0 TOKEN_RUST_EXIT=0
  case "$producer" in
    langc) TOKEN_LANGC_EXIT=7 ;;
    python) TOKEN_PYTHON_EXIT=7 ;;
    rust) TOKEN_RUST_EXIT=7 ;;
  esac
  if run_token_recipes >"$tmp/token-got" 2>"$tmp/token-err"; then
    echo "FAIL tokens: accepted failed $producer producer"
    exit 1
  fi
done
echo 'review regressions: passed'
