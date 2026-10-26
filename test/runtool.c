/* Interpreter behavior and independent run budgets. */
#include <stdio.h>
#include <string.h>

#include "front/check.h"
#include "front/front.h"

static int run_fuel(int cached_init) {
  const char *program = "state State := makeState (counter : Nat)\n"
    "def init : State := makeState 0\n"
    "def readCounter : Env -> State -> Nat := fun env s => counter s\n";
  const char *script = "1 1u 0x00000000000000000000000000000000000000aa readCounter\n";
  const char *want = "1 readCounter = 0\nstate makeState 0\n";
  Arena arena;
  Diag diag;
  DeclList decls;
  Machine machine;
  char got[128] = {0};
  FILE *out = tmpfile();
  int ok;
  uint32_t i;
  if (out == NULL) return 0;
  arena_init(&arena, (size_t)1 << 25);
  diag_init(&diag);
  ok = front_load(&arena, "run-fuel", program, strlen(program), &decls, &diag)
    && check_program(&arena, &decls, &machine, &diag, CHECK_KIT);
  if (ok && cached_init) {
    for (i = 0; i < machine.def_count; i++) {
      if (strcmp(machine.defs[i].name, "init") == 0) {
        ok = def_value(&machine, i) != NULL;
        break;
      }
    }
  }
  if (ok) {
    machine.fuel = 1;
    ok = run_command(&machine, "fuel.script", script, strlen(script), out) == 0;
  }
  rewind(out);
  size_t count = fread(got, 1, sizeof got - 1u, out);
  ok = ok && !ferror(out) && count == strlen(want) && strcmp(got, want) == 0;
  if (!ok) {
    fprintf(stderr, "FAIL run fuel (%s init)\n", cached_init ? "cached" : "uncached");
    diag_print(&diag, stderr);
  }
  fclose(out);
  arena_release(&arena);
  return ok;
}

/* Preserve the interpreter cases formerly coupled to the Wasm gate. */
static int eval_spines(void) {
  const char *program =
    "def lazy : Nat -> Nat := fun n => first (fold (fun (p : Prod Nat Nat) => pair (natAdd (first p) 1) (natAdd (second p) 18446744073709551615)) (pair 0 2) n)\n"
    "def noNats : List Nat := nil\n"
    "def upFrom : Nat -> List Nat := fun n => unfold (fun k => some (pair k (natAdd k 1))) n 1\n"
    "def upTrap : Nat -> List Nat := fun n => unfold (fun k => flagIf (natEq k 3) (some (fold (fun (p : Prod Nat Nat) => p) (pair 0 0) (natAdd 18446744073709551615 1))) (some (pair k (natAdd k 1)))) n 1\n"
    "def foldOnes : Nat -> Nat := fun n => fold (fun x acc => natAdd x acc) 0 (fold (fun (acc : List Nat) => cons 2 acc) noNats n)\n"
    "def mapUp : Nat -> Nat := fun n => fold (fun x acc => natAdd x acc) 0 (map (fun (x : Nat) => natAdd x (natAdd x x)) (upFrom n))\n"
    "def bindUp : Nat -> Nat := fun n => fold (fun x acc => natAdd x acc) 0 (bind (upFrom n) (fun (x : Nat) => cons x (cons 10 noNats)))\n"
    "def filterUp : Nat -> Nat := fun n => fold (fun x acc => natAdd x acc) 0 (filter (fun x => natLe x 2) (upFrom n))\n"
    "def trapHead : Nat -> Nat := fun n => fold (fun x acc => x) 0 (upTrap n)\n"
    "def trapSum : Nat -> Nat := fun n => fold (fun x acc => natAdd x acc) 0 (upTrap n)\n"
    "def filterTrap : Nat -> Nat := fun n => fold (fun x acc => natAdd x acc) 0 (filter (fun x => flagIf (natEq x 3) (natEq (natAdd 18446744073709551615 1) 0) (natLe x 5)) (upFrom n))\n"
    "def bindTrap : Nat -> Nat := fun n => fold (fun x acc => x) 0 (bind (upFrom n) (fun (x : Nat) => flagIf (natEq x 2) (fold (fun (xs : List Nat) => xs) noNats (natAdd 18446744073709551615 1)) (cons x noNats)))\n";
  static const struct { const char *name; const char *arg; const char *want; } rows[] = {
    {"lazy", "3", "3\n"},
    {"foldOnes", "3", "6\n"},
    {"mapUp", "3", "18\n"},
    {"bindUp", "3", "36\n"},
    {"filterUp", "5", "3\n"},
    {"trapHead", "5", "1\n"},
    {"trapSum", "5", "trap\n"},
    {"trapSum", "2", "3\n"},
    {"filterTrap", "2", "3\n"},
    {"filterTrap", "5", "trap\n"},
    {"bindTrap", "5", "1\n"}
  };
  Arena arena;
  Diag diag;
  DeclList decls;
  Machine machine;
  int ok;
  arena_init(&arena, (size_t)1 << 25);
  diag_init(&diag);
  ok = front_load(&arena, "eval-spines", program, strlen(program), &decls, &diag)
    && check_program(&arena, &decls, &machine, &diag, CHECK_KIT);
  for (size_t i = 0; ok && i < sizeof rows / sizeof rows[0]; i++) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    char got[64] = {0};
    char error[256] = {0};
    char *args[] = {(char *)rows[i].arg};
    if (out == NULL || err == NULL) {
      if (out != NULL) fclose(out);
      if (err != NULL) fclose(err);
      ok = 0;
      break;
    }
    machine.fuel = EVAL_FUEL_STEPS;
    ok = eval_command(&machine, rows[i].name, args, 1, out, err) == 0;
    rewind(out);
    rewind(err);
    fread(got, 1, sizeof got - 1u, out);
    fread(error, 1, sizeof error - 1u, err);
    ok = ok && !ferror(out) && !ferror(err) && strcmp(got, rows[i].want) == 0
      && (strcmp(got, "trap\n") == 0 ? strncmp(error, "langc: EVAL_OVERFLOW:", 21u) == 0 : error[0] == '\0');
    if (!ok) fprintf(stderr, "FAIL eval %s %s: wanted %s, got %s", rows[i].name, rows[i].arg, rows[i].want, got);
    fclose(out);
    fclose(err);
  }
  if (!ok) diag_print(&diag, stderr);
  else printf("interpreter regressions: %zu checked\n", sizeof rows / sizeof rows[0]);
  arena_release(&arena);
  return ok;
}

int main(void) {
  int ok = run_fuel(0);
  ok &= run_fuel(1);
  ok &= eval_spines();
  if (ok) puts("run fuel regressions: passed");
  return ok ? 0 : 1;
}
