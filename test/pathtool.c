/* Path regressions. Each row checks a definition, prints its type and its
 * value, checks the printed text again and compares the two results with
 * conv_values. This is the port of coin-lang test/path-regressions.c
 * (582ecef) onto this kit's harness shape (test/fronttool.c): the source is
 * parsed as a domain, so a row that needs a family or a domain definition
 * carries it in a prelude that both programs share. */
#include <stdio.h>
#include <string.h>

#include "front/check.h"
#include "front/lexer.h"
#include "front/parser.h"

static const char LOT[] = "family Lot (n : Nat) := makeLot (amount : Nat)\n";
static const char TRANSPORT[] =
  "def transport : (X : Type 0) -> (Y : Type 0) -> Path (Type 0) X Y -> X -> Y :=\n"
  "  fun (X : Type 0) => fun (Y : Type 0) => fun (p : Path (Type 0) X Y) => fun (a : X) => transp (fun (i : I) => p @ i) i0 a\n";
static const char S1[] = "family S1 := base | loop (i : I) [ ~i -> base, i -> base ]\n";
static const char TRUNC0[] =
  "family Trunc0 (A : Type 0) := trunc0 (trunc0Of : A) | squash0 (squash0From : Trunc0 A) (squash0To : Trunc0 A) (squash0Left : Path (Trunc0 A) squash0From squash0To) (squash0Right : Path (Trunc0 A) squash0From squash0To) (i : I) (j : I) [ ~i -> squash0Left @ j, i -> squash0Right @ j, ~j -> squash0From, j -> squash0To ]\n";
static const char TRUNC1[] =
  "family Trunc1 (A : Type 0) := trunc1 (truncOf : A) | squash1 (squashFrom : Trunc1 A) (squashTo : Trunc1 A) (i : I) [ ~i -> squashFrom, i -> squashTo ]\n";
static const char PARAM_HIT[] =
  "family Trunc1 (A : Type 0) := trunc1 (truncOf : A) | squash1 (squashFrom : Trunc1 A) (squashTo : Trunc1 A) (i : I) [ ~i -> squashFrom, i -> squashTo ]\n"
  "family ParamHit (a : Trunc1 Nat) := paramPoint (paramValue : Trunc1 Nat) |\n"
  "  paramLoop (i : I) [ ~i -> paramPoint a a, i -> paramPoint a a ]\n";

/* The domain defs that the Glue rows use (S5, coin-lang 91d7091): the
   tool embeds no domain, so the rows take them as a prelude. */
static const char EQUIV[] =
  "def transport : (X : Type 0) -> (Y : Type 0) -> Path (Type 0) X Y -> X -> Y :=\n"
  "  fun (X : Type 0) => fun (Y : Type 0) => fun (p : Path (Type 0) X Y) => fun (a : X) => transp (fun (i : I) => p @ i) i0 a\n"
  "def isContr : (A : Type 0) -> Type 0 :=\n"
  "  fun (A : Type 0) => Sigma (c : A) ((x : A) -> Path A c x)\n"
  "def fiber : (A : Type 0) -> (B : Type 0) -> (f : A -> B) -> (y : B) -> Type 0 :=\n"
  "  fun (A : Type 0) => fun (B : Type 0) => fun (f : A -> B) => fun (y : B) => Sigma (x : A) (Path B (f x) y)\n"
  "def isEquiv : (A : Type 0) -> (B : Type 0) -> (f : A -> B) -> Type 0 :=\n"
  "  fun (A : Type 0) => fun (B : Type 0) => fun (f : A -> B) => (y : B) -> isContr (fiber A B f y)\n"
  "def Equiv : (A : Type 0) -> (B : Type 0) -> Type 0 :=\n"
  "  fun (A : Type 0) => fun (B : Type 0) => Sigma (f : A -> B) (isEquiv A B f)\n"
  "def idEquiv : (A : Type 0) -> Equiv A A :=\n"
  "  fun (A : Type 0) => pack (fun (x : A) => x) (fun (y : A) =>\n"
  "    pack (pack y refl) (fun (z : fiber A A (fun (x : A) => x) y) =>\n"
  "      fun (i : I) => pack (payload z @ ~i) (fun (j : I) => payload z @ (~i \\/ j))))\n"
  "def ua : (A : Type 0) -> (B : Type 0) -> (e : Equiv A B) -> Path (Type 0) A B :=\n"
  "  fun (A : Type 0) => fun (B : Type 0) => fun (e : Equiv A B) =>\n"
  "    fun (i : I) => Glue B (~i \\/ i) (fun (_ : I) => [ ~i -> pack A e, i -> pack B (idEquiv B) ])\n";

static int checked(Arena *arena, Machine *m, Diag *diag, const char *source) {
  TokenList tokens = {0};
  DeclList decls = {0};
  diag_init(diag);
  return lex_source(arena, "path-regression", source, strlen(source), &tokens, diag)
      && parse_source(arena, "path-regression", ORIGIN_DOMAIN, &tokens, &decls, diag)
      && check_program(arena, &decls, m, diag, CHECK_KIT);
}

static int roundtrip(const char *label, const char *prelude, const char *source) {
  Arena arena;
  Machine m, replay;
  Diag diag;
  char type[8192], value[8192], program[32768];
  int ok;
  uint32_t index;
  arena_init(&arena, (size_t)128 << 20);
  snprintf(program, sizeof program, "%s%s", prelude, source);
  ok = checked(&arena, &m, &diag, program);
  index = ok ? m.def_count - 1u : 0u;
  if (ok)
    ok = value_print(&m, NULL, 0, m.defs[index].type, type, sizeof type)
      && value_print(&m, NULL, 0, def_value(&m, index), value, sizeof value);
  if (ok) {
    snprintf(program, sizeof program, "%sdef replay : %s := %s\n", prelude, type, value);
    ok = checked(&arena, &replay, &diag, program);
    if (!ok) fprintf(stderr, "%s\n", program);
  }
  if (ok)
    ok = conv_values(&m, 0, m.defs[index].type, replay.defs[replay.def_count - 1u].type)
      && conv_values(&m, 0, def_value(&m, index), def_value(&replay, replay.def_count - 1u));
  if (!ok) {
    fprintf(stderr, "FAIL %s\n", label);
    diag_print(&diag, stderr);
  }
  arena_release(&arena);
  return ok;
}

/* A stuck internal finite case has no surface syntax: the printer refuses
   it (S5, ported from coin-lang 44c917f). */
static int print_refused(const char *label, const char *prelude, const char *source) {
  Arena arena;
  Machine m;
  Diag diag;
  char value[8192], program[32768];
  int ok;
  arena_init(&arena, (size_t)128 << 20);
  snprintf(program, sizeof program, "%s%s", prelude, source);
  ok = checked(&arena, &m, &diag, program);
  if (ok)
    ok = !value_print(&m, NULL, 0, def_value(&m, m.def_count - 1u), value, sizeof value)
      && !diag.set;
  if (!ok) {
    fprintf(stderr, "FAIL %s: an internal finite case has no surface syntax\n", label);
    if (diag.set) diag_print(&diag, stderr);
  }
  arena_release(&arena);
  return ok;
}

/* Glue composition and system conversion (S5, ported from coin-lang
   34acff5): ACCEPTS checks a program, SYSTEM_CONVERSION compares two
   systems by branch values and face coverage. */
static int accepts(const char *label, const char *prelude, const char *source) {
  Arena arena;
  Machine m;
  Diag diag;
  char program[32768];
  int ok;
  arena_init(&arena, (size_t)128 << 20);
  snprintf(program, sizeof program, "%s%s", prelude, source);
  ok = checked(&arena, &m, &diag, program);
  if (!ok) {
    fprintf(stderr, "FAIL %s\n", label);
    diag_print(&diag, stderr);
  }
  arena_release(&arena);
  return ok;
}

static int system_conversion(void) {
  Arena arena;
  Machine m;
  Diag diag;
  Value lines[2] = {{0}, {0}};
  Value negative;
  IConj conjunction;
  uint32_t negative_lit = 1;
  const Value *points[2];
  const Value *args[6];
  const Value *i;
  const Value *ni;
  const Value *original;
  const Value *other;
  int ok;
  uint32_t k;
  arena_init(&arena, (size_t)128 << 20);
  ok = checked(&arena, &m, &diag, "def seed : Nat := 0\n");
  if (ok) {
    for (k = 0; k < 2u; k++) {
      points[k] = val_nat(&m, k);
      lines[k].kind = VAL_PLAM;
      lines[k].tag = CLOS_CONST;
      lines[k].args = &points[k];
      lines[k].argc = 1;
    }
    i = val_ivar(&m, 0);
    negative = *i;
    conjunction.lits = &negative_lit;
    conjunction.count = 1;
    negative.conj = &conjunction;
    ni = &negative;
    args[0] = i; args[1] = &lines[0];
    args[2] = ni; args[3] = &lines[1];
    original = val_op(&m, VAL_OP, OP_SYSTEM, 0, 0, args, 4);
    args[0] = ni; args[1] = &lines[1];
    args[2] = i; args[3] = &lines[0];
    other = val_op(&m, VAL_OP, OP_SYSTEM, 0, 0, args, 4);
    ok = conv_values(&m, 2, original, other);
    args[1] = &lines[0]; args[3] = &lines[1];
    other = val_op(&m, VAL_OP, OP_SYSTEM, 0, 0, args, 4);
    ok = ok && !conv_values(&m, 2, original, other);
    args[0] = i; args[1] = &lines[0];
    other = val_op(&m, VAL_OP, OP_SYSTEM, 0, 0, args, 2);
    ok = ok && !conv_values(&m, 2, original, other);
    args[2] = ni; args[3] = &lines[1];
    args[4] = face_meet(&m, i, val_ivar(&m, 1)); args[5] = &lines[0];
    other = val_op(&m, VAL_OP, OP_SYSTEM, 0, 0, args, 6);
    ok = ok && conv_values(&m, 2, original, other) && !diag.set;
  }
  if (!ok) {
    fprintf(stderr, "FAIL system conversion: order, values, coverage or refinement\n");
    if (diag.set) diag_print(&diag, stderr);
  }
  arena_release(&arena);
  return ok;
}

int main(void) {
  int failed = 0;
  failed += !roundtrip("PathP binder capture", "",
    "def original : Sigma (PathP : Nat) (Path Nat PathP PathP) := pack 0 refl\n");
  failed += !roundtrip("anonymous funExt argument", "",
    "def original : (h : (_ : Nat) -> Path Nat 0 0) -> Path (Nat -> Nat) (fun n => 0) (fun n => 0) := fun h => funExt h\n");
  failed += !roundtrip("application of a path of functions", "",
    "def original : (p : Path (Nat -> Nat) (fun n => n) (fun n => n)) -> Path Nat 0 0 := fun p => fun (i : I) => (p @ i) 0\n");
  failed += !roundtrip("dependent congruence", LOT,
    "def original : (p : Path Nat 0 0) -> PathP (fun (i : I) => Lot (p @ i)) (makeLot 0 7) (makeLot 0 7) := fun p => cong (fun (n : Nat) => makeLot n 7) p\n");
  failed += !roundtrip("dependent symmetry", "",
    "def original : (A : Path (Type 0) Nat Nat) -> (p : PathP (fun (j : I) => A @ j) 0 1) -> PathP (fun (i : I) => A @ ~i) 1 0 := fun A p => sym p\n");
  failed += !roundtrip("dependent function extensionality", "",
    "def original : (A : Path (Type 0) Nat Nat) -> (h : (n : Nat) -> PathP (fun (j : I) => A @ j) n n) -> PathP (fun (i : I) => Nat -> A @ i) (fun n => n) (fun n => n) := fun A h => funExt h\n");
  failed += !roundtrip("stuck transport fill", TRANSPORT,
    "def original : PathP (fun (i : I) => (p : Path (Type 0) Nat Nat) -> p @ i) (fun (p : Path (Type 0) Nat Nat) => 0) (fun (p : Path (Type 0) Nat Nat) => transport Nat Nat p 0) := fun (i : I) => fun (p : Path (Type 0) Nat Nat) => transp (fun (j : I) => p @ (i /\\ j)) (~i) 0\n");
  failed += !roundtrip("stuck hcomp with two keys", "",
    "def original : (n : Nat) -> (p : Path Nat n n) -> Path Nat n n := fun n p => fun (j : I) => hcomp Nat (~j \\/ j) (fun (i : I) => [ ~j -> n, j -> p @ i ]) (p @ j)\n");
  failed += !roundtrip("stuck transport in a Path line", "",
    "def original : (A : Type 0) -> (x : A) -> (y : A) -> (r : Path A x y) -> (p : Path A x x) -> Path A y y := fun A x y r p => transp (fun (i : I) => Path A (r @ i) (r @ i)) i0 p\n");
  failed += !roundtrip("dead constructor branch", "",
    "def original : (x : Prod Nat Nat) -> (n : Nat) -> (p : Path Nat n n) -> Path (Prod Nat Nat) (pair n 0) (pair n 0) := fun x n p => fun (j : I) => hcomp (Prod Nat Nat) (~j \\/ j) (fun (i : I) => [ ~j -> pair n 0, j -> pair (p @ i) 0, i0 -> x ]) (pair n 0)\n");
  failed += !roundtrip("dependent Sigma composition", "",
    "def original : (n : Nat) -> (p : Path Nat n n) -> Path (Sigma (x : Nat) (Path Nat x x)) (pack n refl) (pack n refl) := fun n p => fun (j : I) => hcomp (Sigma (x : Nat) (Path Nat x x)) (~j \\/ j) (fun (i : I) => [ ~j -> pack n refl, j -> pack (p @ i) refl ]) (pack n refl)\n");
  failed += !roundtrip("universe function composition", "",
    "def original : (p : Path (Nat -> Type 0) (fun n => Nat) (fun n => Nat)) -> Path (Nat -> Type 0) (fun n => Nat) (fun n => Nat) := fun p => fun (j : I) => hcomp (Nat -> Type 0) (~j \\/ j) (fun (i : I) => [ ~j -> (fun n => Nat), j -> p @ i ]) (fun n => Nat)\n");
  failed += !roundtrip("universe Sigma composition", "",
    "def original : (p : Path (Sigma (x : Nat) (Type 0)) (pack 0 Nat) (pack 0 Nat)) -> Path (Sigma (x : Nat) (Type 0)) (pack 0 Nat) (pack 0 Nat) := fun p => fun (j : I) => hcomp (Sigma (x : Nat) (Type 0)) (~j \\/ j) (fun (i : I) => [ ~j -> pack 0 Nat, j -> p @ i ]) (pack 0 Nat)\n");
  failed += !roundtrip("universe product composition", "",
    "def original : (p : Path (Prod Nat (Type 0)) (pair 0 Nat) (pair 0 Nat)) -> Path (Prod Nat (Type 0)) (pair 0 Nat) (pair 0 Nat) := fun p => fun (j : I) => hcomp (Prod Nat (Type 0)) (~j \\/ j) (fun (i : I) => [ ~j -> pair 0 Nat, j -> pair 0 Nat ]) (pair 0 Nat)\n");
  failed += !roundtrip("universe path composition", "",
    "def original : (p : Path (Path (Type 0) Nat Nat) refl refl) -> Path (Path (Type 0) Nat Nat) refl refl := fun p => fun (j : I) => hcomp (Path (Type 0) Nat Nat) (~j \\/ j) (fun (i : I) => [ ~j -> refl, j -> p @ i ]) refl\n");
  failed += !roundtrip("transport of a universe path", "",
    "def original : (p : Path (Type 0) Nat Nat) -> Path (Type 0) Nat Nat := fun p => transp (fun (_ : I) => Path (Type 0) Nat Nat) i0 p\n");
  failed += !roundtrip("face action on a local type", "",
    "def original : (P : Path (Type 0) Nat Nat) -> PathP (fun (j : I) => (n : P @ j) -> P @ j) (fun (n : Nat) => n) (fun (n : Nat) => n) := fun P => fun (j : I) => fun (n : P @ j) => hcomp (P @ j) (~j \\/ j) (fun (i : I) => [ ~j -> n, j -> n ]) n\n");
  failed += !roundtrip("a branch on a disjunctive face", "",
    "def original : (p : Path Nat 0 1) -> PathP (fun (j : I) => Path Nat (p @ j) (p @ j)) refl refl := fun p => fun (j : I) => hcomp (Path Nat (p @ j) (p @ j)) (~j \\/ j) (fun (i : I) => [ ~j \\/ j -> refl ]) refl\n");
  /* Kit-only row (sealed-lang S3, R27): a word base with no live branch is the base. */
  failed += !roundtrip("hcomp over a literal word base", "",
    "def original : U256 := hcomp U256 i0 (fun (i : I) => [ ]) 7u\n");
  failed += !roundtrip("formal hcomp at a HIT", S1,
    "def original : Path S1 base base := fun (j : I) => hcomp S1 (~j \\/ j) (fun (i : I) => [ ~j -> base, j -> loop i ]) (loop j)\n");
  failed += !roundtrip("path constructor at a neutral interval", S1,
    "def original : PathP (fun (i : I) => Path S1 base (loop i)) refl (fun (j : I) => loop j) := fun (i : I) => fun (j : I) => loop (i /\\ j)\n");
  failed += !roundtrip("transport of a formal hcomp at Trunc1 along a parameter line", TRUNC1,
    "def original : (A : Type 0) -> (B : Type 0) -> (p : Path (Type 0) A B) -> (a : A) -> (b : A) ->\n"
    "  Path (Trunc1 B) (transp (fun (i : I) => Trunc1 (p @ i)) i0 (trunc1 A a)) (transp (fun (i : I) => Trunc1 (p @ i)) i0 (trunc1 A a)) :=\n"
    "  fun (A : Type 0) => fun (B : Type 0) => fun (p : Path (Type 0) A B) => fun (a : A) => fun (b : A) => fun (j : I) =>\n"
    "  transp (fun (i : I) => Trunc1 (p @ i)) i0 (hcomp (Trunc1 A) (~j \\/ j)\n"
    "    (fun (i : I) => [ ~j -> trunc1 A a, j -> squash1 A (trunc1 A b) (trunc1 A a) i ]) (squash1 A (trunc1 A a) (trunc1 A b) j))\n");
  failed += !roundtrip("transport preserves a parameter-dependent HIT boundary", PARAM_HIT,
    "def endpoint : ParamHit (trunc1 Nat 1) :=\n"
    "  transp (fun (i : I) => ParamHit (squash1 Nat (trunc1 Nat 0) (trunc1 Nat 1) i)) i0\n"
    "    (paramPoint (trunc1 Nat 0) (trunc1 Nat 0))\n"
    "def original : Path (ParamHit (trunc1 Nat 1)) endpoint endpoint :=\n"
    "  fun (j : I) => transp (fun (i : I) => ParamHit (squash1 Nat (trunc1 Nat 0) (trunc1 Nat 1) i)) i0\n"
    "    (paramLoop (trunc1 Nat 0) j)\n");
  /* The transport of a dimension-2 constructor along a parameter line stays
     stuck. Its boundary branches are path applications, not recursive
     fields, so a field rebuild needs a boundary correction. */
  failed += !roundtrip("transport of squash0 along a parameter line stays stuck", TRUNC0,
    "def original : (A : Type 0) -> (B : Type 0) -> (p : Path (Type 0) A B) -> (x : Trunc0 A) -> (y : Trunc0 A) ->\n"
    "  (u : Path (Trunc0 A) x y) -> (v : Path (Trunc0 A) x y) ->\n"
    "  Path (Path (Trunc0 B) (transp (fun (k : I) => Trunc0 (p @ k)) i0 x) (transp (fun (k : I) => Trunc0 (p @ k)) i0 y))\n"
    "    (fun (j : I) => transp (fun (k : I) => Trunc0 (p @ k)) i0 (u @ j)) (fun (j : I) => transp (fun (k : I) => Trunc0 (p @ k)) i0 (v @ j)) :=\n"
    "  fun (A : Type 0) => fun (B : Type 0) => fun (p : Path (Type 0) A B) => fun (x : Trunc0 A) => fun (y : Trunc0 A) =>\n"
    "  fun (u : Path (Trunc0 A) x y) => fun (v : Path (Trunc0 A) x y) =>\n"
    "  fun (i : I) => fun (j : I) => transp (fun (k : I) => Trunc0 (p @ k)) i0 (squash0 A x y u v i j)\n");
  /* Boundary images must fold path fields just as evaluation does. */
  failed += !roundtrip("fold boundary maps a point constructor path field",
    "family HH := hhBase | hhNode (hhX : HH) (hhY : HH) (hhP : Path HH hhX hhY) | hhLoop (i : I) [ ~i -> hhNode hhBase hhBase refl, i -> hhNode hhBase hhBase refl ]\n",
    "def original : Path Nat 0 0 := fun (t : I) => fold 0 (fun (x : Nat) (y : Nat) (p : Path Nat x y) => p @ t) refl (hhLoop t)\n");
  failed += !roundtrip("fold square boundary preserves captured intervals in path fields",
    "family HH := hhBase | hhNode (hhX : HH) (hhY : HH) (hhP : Path HH hhX hhY)\n"
    "  | hhSquare (hhU : HH) (hhV : Path HH hhU hhU) (i : I) (j : I) [ ~i -> hhNode hhU hhU (fun (k : I) => hhV @ (j /\\ ~j /\\ k /\\ ~k)),\n"
    "      i -> hhNode hhU hhU (fun (k : I) => hhV @ (j /\\ ~j /\\ k /\\ ~k)), ~j -> hhNode hhU hhU refl, j -> hhNode hhU hhU refl ]\n",
    "def original : HH -> HH := fun (z : HH) => fold hhBase (fun (x : HH) (y : HH) (p : Path HH x y) => hhNode x y p) (fun (x : HH) (p : Path HH x x) => fun (a : I) => fun (b : I) => hhSquare x p a b) z\n");
  failed += !roundtrip("stuck Fin addition", "",
    "def original : (r : Fin 11) -> Fin 11 := fun (r : Fin 11) => finAdd 11 r 3\n");
  failed += !print_refused("finite case function printing", "",
    "def original : (k : Fin 2) -> Path (Fin 2) (finAdd 2 k 0) k := fun k => refl\n");
  failed += !print_refused("finite case path printing", "",
    "def p : (k : Fin 2) -> Path (Fin 2) (finAdd 2 k 0) k := fun k => refl\n"
    "def original : Path (Fin 2 -> Fin 2) (fun k => finAdd 2 k 0) (fun k => k) := funExt p\n");
  failed += !roundtrip("Glue type at a neutral face", EQUIV,
    "def original : (A : Type 0) -> (e : Equiv A A) -> (f : Equiv A A) -> Path (Type 0) A A := fun A e f => fun (i : I) => Glue A (~i \\/ i) (fun (_ : I) => [ ~i -> pack A e, i -> pack A f ])\n");
  failed += !roundtrip("stuck unglue", EQUIV,
    "def original : (A : Type 0) -> (e : Equiv A A) -> (x : A) -> (y : A) -> (g : PathP (fun (i : I) => Glue A (~i \\/ i) (fun (_ : I) => [ ~i -> pack A e, i -> pack A e ])) x y) -> Path A (witness e x) (witness e y) := fun A e x y g => fun (i : I) => unglue (g @ i)\n");
  failed += !roundtrip("univalence at a Glue endpoint", EQUIV,
    "def G : Type 0 := Glue (Fin 3) i0 (fun (_ : I) => [ ])\n"
    "def u : G := glue (fun (_ : I) => [ ]) 0\n"
    "def original : G := transport G G (ua G G (idEquiv G)) u\n");
  failed += !roundtrip("unglue at a nested Glue face", EQUIV,
    "def G : Type 0 := Glue (Fin 3) i0 (fun (_ : I) => [ ])\n"
    "def u : G := glue (fun (_ : I) => [ ]) 0\n"
    "def p : PathP (fun (i : I) => Glue G (~i \\/ i) (fun (_ : I) => [ ~i -> pack G (idEquiv G), i -> pack G (idEquiv G) ])) u u := fun (i : I) => glue (fun (_ : I) => [ ~i -> u, i -> u ]) u\n"
    "def original : PathP (fun (i : I) => Path G (unglue (p @ i)) u) refl refl := fun (i : I) => refl\n");
  failed += !system_conversion();
  failed += !accepts("transport at a Glue with a stable face", EQUIV,
    "def G : Path (Type 0) (Fin 3) (Fin 3) := fun (r : I) => Glue (Fin 3) (~r \\/ r) (fun (_ : I) => [ ~r -> pack (Fin 3) (idEquiv (Fin 3)), r -> pack (Fin 3) (idEquiv (Fin 3)) ])\n"
    "def g : PathP (fun (r : I) => G @ r) 0 0 := fun (r : I) => glue (fun (_ : I) => [ ~r -> 0, r -> 0 ]) 0\n"
    "def same : PathP (fun (r : I) => Path (Fin 3) (unglue (transp (fun (_ : I) => G @ r) i0 (g @ r))) 0) refl refl := fun (r : I) => refl\n");
  failed += !accepts("transport at a Glue with a nonempty constancy face", EQUIV,
    "def G : Path (Type 0) (Fin 3) (Fin 3) := fun (r : I) => Glue (Fin 3) (~r \\/ r) (fun (_ : I) => [ ~r -> pack (Fin 3) (idEquiv (Fin 3)), r -> pack (Fin 3) (idEquiv (Fin 3)) ])\n"
    "def g : PathP (fun (r : I) => G @ r) 0 0 := fun (r : I) => glue (fun (_ : I) => [ ~r -> 0, r -> 0 ]) 0\n"
    "def same : PathP (fun (r : I) => Path (Fin 3) (unglue (transp (fun (_ : I) => G @ r) r (g @ r))) 0) refl refl := fun (r : I) => refl\n");
  failed += !accepts("Glue composition with reordered faces", EQUIV,
    "def G : Path (Type 0) (Fin 3) (Fin 3) := fun (j : I) => Glue (Fin 3) (~j \\/ j) (fun (_ : I) => [ ~j -> pack (Fin 3) (idEquiv (Fin 3)), j -> pack (Fin 3) (idEquiv (Fin 3)) ])\n"
    "def g : PathP (fun (j : I) => G @ j) 0 0 := fun (j : I) => glue (fun (_ : I) => [ ~j \\/ j -> 0 ]) 0\n"
    "def same : PathP (fun (j : I) => Path (G @ j) (hcomp (G @ j) (~j \\/ j) (fun (_ : I) => [ ~j -> g @ j, j -> g @ j ]) (g @ j)) (g @ j)) refl refl := fun (j : I) => refl\n");
  failed += !accepts("Glue composition under a stronger face", EQUIV,
    "def G0 : Path (Type 0) (Fin 3) (Fin 3) := fun (j : I) => Glue (Fin 3) (~j \\/ j) (fun (_ : I) => [ ~j -> pack (Fin 3) (idEquiv (Fin 3)), j -> pack (Fin 3) (idEquiv (Fin 3)) ])\n"
    "def g0 : PathP (fun (j : I) => G0 @ j) 0 0 := fun (j : I) => glue (fun (_ : I) => [ ~j \\/ j -> 0 ]) 0\n"
    "def G1 : Path (Type 0) (Fin 3) (Fin 3) := fun (j : I) => Glue (Fin 3) (~j \\/ j) (fun (_ : I) => [ ~j -> pack (Fin 3) (idEquiv (Fin 3)), j -> pack (Fin 3) (idEquiv (Fin 3)), ~j -> pack (Fin 3) (idEquiv (Fin 3)) ])\n"
    "def g1 : PathP (fun (j : I) => G1 @ j) 0 0 := fun (j : I) => glue (fun (_ : I) => [ ~j \\/ j -> 0 ]) 0\n"
    "def G : Path (Path (Type 0) (Fin 3) (Fin 3)) G0 G1 := fun (i : I) => fun (j : I) => Glue (Fin 3) (~j \\/ j) (fun (_ : I) => [ ~j -> pack (Fin 3) (idEquiv (Fin 3)), j -> pack (Fin 3) (idEquiv (Fin 3)), ~j /\\ i -> pack (Fin 3) (idEquiv (Fin 3)) ])\n"
    "def g : PathP (fun (i : I) => PathP (fun (j : I) => G @ i @ j) 0 0) g0 g1 := fun (i : I) => fun (j : I) => glue (fun (_ : I) => [ ~j \\/ j -> 0 ]) 0\n"
    "def same0 : PathP (fun (j : I) => Path (G0 @ j) (hcomp (G0 @ j) (~j \\/ j) (fun (_ : I) => [ ~j -> g0 @ j, j -> g0 @ j ]) (g0 @ j)) (g0 @ j)) refl refl := fun (j : I) => refl\n"
    "def same1 : PathP (fun (j : I) => Path (G1 @ j) (hcomp (G1 @ j) (~j \\/ j) (fun (_ : I) => [ ~j -> g1 @ j, j -> g1 @ j ]) (g1 @ j)) (g1 @ j)) refl refl := fun (j : I) => refl\n"
    "def same : PathP (fun (i : I) => PathP (fun (j : I) => Path (G @ i @ j) (hcomp (G @ i @ j) (~j \\/ j) (fun (_ : I) => [ ~j -> g @ i @ j, j -> g @ i @ j ]) (g @ i @ j)) (g @ i @ j)) refl refl) same0 same1 := fun (i : I) => fun (j : I) => refl\n");
  if (failed == 0)
    puts("path regressions: passed");
  else
    printf("FAIL path regressions: %d failed\n", failed);
  return failed != 0;
}
