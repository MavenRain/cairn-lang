/* The type checker, the refusals R1-R4 and the eval command. SHARED by the
   tcc kits. */
#ifndef LANG_FRONT_CHECK_H
#define LANG_FRONT_CHECK_H

#include "front/core.h"

#define ENTRY_PARAMS_MAX 32u /* 16 before sealed-lang S2 (R23): slashUsr takes 19 words */

/* An entry: a program definition of type Nat, Flag, U256, or a function of
   Nat, Flag and U256 arguments to a Nat, Flag or U256 result (Q-S8-3). */
typedef struct {
  uint32_t param_count;
  int param_flag[ENTRY_PARAMS_MAX];
  int param_u256[ENTRY_PARAMS_MAX];
  int result_flag;
  int result_u256;
} Entry;

/* Checks the domain declarations, then the program, into M. Returns 1, or 0
   after a diagnostic. */
/* The check mode (orbit-lang S3). In guest mode a program declaration
   cannot use a kit item (REFUSE_KIT). Kit mode is the trusted test scope of
   the kit gate (`langc --kit`). */
typedef enum {
  CHECK_GUEST,
  CHECK_KIT
} CheckMode;

/* The progress counts of `langc check --json` (Q-S12-6). */
typedef struct {
  int parsed;
  int typed;
  int laws_discharged;
  int laws_total;
} CheckProgress;

int check_program(Arena *arena, const DeclList *decls, Machine *m, Diag *diag, CheckMode mode);
/* check_program, then the progress counts of `langc check --json`
   (Q-S12-6, Q-S12-9 (b)): parsed and typed count the program decls (a
   block counts each sort), laws_discharged the typed program defs with
   law_only, and laws_total adds the untyped program defs that a syntactic
   test of rule O7 marks (check.c LawScan). */
int check_program_progress(Arena *arena, const DeclList *decls, Machine *m, Diag *diag, CheckMode mode, CheckProgress *progress);
/* Returns 1 when TYPE is the type of an entry. */
int entry_of(Machine *m, const Value *type, Entry *out);
/* Returns 1 when TYPE has one or more scalar params and a family result: the
   definition has no target form (O24). */
int family_result_of(Machine *m, const Value *type);
/* Evaluates the definition NAME on ARGS and prints the result. Returns 0, 1
   after a diagnostic, or 2 after an EVAL_ARGS diagnostic. */
int eval_command(Machine *m, const char *name, char *const *args, int arg_count, FILE *out, FILE *err);
/* Prints the ZK calldata words of the private ballot NAME (castPrivate m c)
   under the poll id POLL (NULL means 0): the choice index, then c_1, s_1 to
   s_n, I.x and I.y, one word per line (S8 B2c). Returns 0, 1 after a
   diagnostic, or 2 after an EVAL_ARGS or IO_WRITE diagnostic. */
int prove_command(Machine *m, const char *name, const char *poll, FILE *out, FILE *err);
/* Runs the call script TEXT (from PATH) on the state from `init` (slice K3b)
   and prints each call and the final state. Returns 0, 1 after a diagnostic,
   or 2 after an EVAL_ARGS diagnostic. */
int run_command(Machine *m, const char *path, const char *text, size_t len, FILE *out);

#endif
