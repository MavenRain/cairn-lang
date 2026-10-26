/* The langc command line. SHARED by the tcc kits.
   Exit 0: ok. Exit 1: the program is refused. Exit 2: usage or IO. */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "front/check.h"
#include "front/front.h"

#define ARENA_LIMIT_BYTES ((size_t)1 << 30)

typedef enum {
  CMD_CHECK,
  CMD_EVAL,
  CMD_RUN,
  CMD_IR,
  CMD_ABI,
  CMD_BUILD,
  CMD_PROVE
} Command;

typedef struct {
  Command command;
  const char *prog_path;
  const char *entry;
  char **args;
  int arg_count;
  const char *script_path;
  const char *out_path;
  int runtime;
  int json;
  CheckMode mode; /* orbit-lang S3: --kit selects the kit mode */
} Options;

static const struct {
  const char *word;
  Command command;
} COMMANDS[] = {
  {"check", CMD_CHECK},
  {"eval", CMD_EVAL},
  {"run", CMD_RUN},
  {"ir", CMD_IR},
  {"abi", CMD_ABI},
  {"build", CMD_BUILD},
  {"prove", CMD_PROVE},
};

static int usage(FILE *err) {
  fputs("usage: langc [--kit] check PROG\n"
        "       langc [--kit] eval PROG NAME [ARGS...]\n"
        "       langc [--kit] run PROG SCRIPT\n"
        "       langc [--kit] ir PROG\n"
        "       langc [--kit] abi PROG [--text]\n"
        "       langc [--kit] build PROG [-o OUT] [--runtime]\n"
        "       langc [--kit] prove PROG NAME [POLL]\n"
        "--kit: the kit mode, the trusted test scope of the kit gate\n", err);
  return 2;
}

static int find_command(const char *word, Command *out) {
  for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
    if (strcmp(COMMANDS[i].word, word) == 0) {
      *out = COMMANDS[i].command;
      return 1;
    }
  }
  return 0;
}

static int parse_build_options(int argc, char **argv, Options *opt, Diag *diag) {
  for (int i = 3; i < argc; i++) {
    int is_out = strcmp(argv[i], "-o") == 0 && i + 1 < argc;
    int is_runtime = strcmp(argv[i], "--runtime") == 0;
    if (!is_out && !is_runtime) return diag_fail(diag, "USAGE", NULL, "unknown build argument '%s'", argv[i]);
    opt->out_path = is_out ? argv[i + 1] : opt->out_path;
    opt->runtime |= is_runtime;
    i += is_out;
  }
  return 1;
}

static int parse_options(int argc, char **argv, Options *opt, Diag *diag) {
  memset(opt, 0, sizeof *opt);
  if (argc < 3 || !find_command(argv[1], &opt->command)) {
    return diag_fail(diag, "USAGE", NULL, "expected a command and a program path");
  }
  opt->prog_path = argv[2];
  switch (opt->command) {
    case CMD_CHECK:
    case CMD_IR:
      return argc == 3 ? 1 : diag_fail(diag, "USAGE", NULL, "too many arguments");
    case CMD_ABI:
      /* The JSON ABI is the default; --text gives the C-K4-16 lines (C-c7-1). */
      opt->json = argc == 3;
      return argc == 3 || (argc == 4 && strcmp(argv[3], "--text") == 0) ? 1
        : diag_fail(diag, "USAGE", NULL, "abi takes only --text after the program");
    case CMD_EVAL:
      if (argc < 4) return diag_fail(diag, "USAGE", NULL, "eval needs a definition name");
      opt->entry = argv[3];
      opt->args = argv + 4;
      opt->arg_count = argc - 4;
      return 1;
    case CMD_RUN:
      if (argc != 4) return diag_fail(diag, "USAGE", NULL, "run needs a program and a script");
      opt->script_path = argv[3];
      return 1;
    case CMD_BUILD:
      return parse_build_options(argc, argv, opt, diag);
    case CMD_PROVE:
      if (argc < 4) return diag_fail(diag, "USAGE", NULL, "prove needs a definition name");
      if (argc > 5) return diag_fail(diag, "USAGE", NULL, "prove takes only a poll id after the name");
      opt->entry = argv[3];
      opt->args = argv + 4;
      opt->arg_count = argc - 4;
      return 1;
  }
  return 0;
}

static int read_source(Arena *arena, const char *path, char **out, size_t *len, Diag *diag) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return diag_fail(diag, "IO", NULL, "cannot open %s", path);
  char *buf = arena_alloc(arena, SOURCE_MAX_BYTES + 1);
  size_t count = buf == NULL ? 0 : fread(buf, 1, SOURCE_MAX_BYTES + 1, file);
  int read_error = ferror(file);
  fclose(file);
  if (buf == NULL) return diag_fail(diag, "OOM", NULL, "no memory for %s", path);
  if (read_error) return diag_fail(diag, "IO", NULL, "cannot read %s", path);
  if (count > SOURCE_MAX_BYTES) return diag_fail(diag, "IO_SIZE", NULL, "%s is larger than %zu bytes", path, SOURCE_MAX_BYTES);
  *out = buf;
  *len = count;
  return 1;
}

static const char *command_word(Command command) {
  switch (command) {
    case CMD_CHECK: return "check";
    case CMD_EVAL: return "eval";
    case CMD_RUN: return "run";
    case CMD_IR: return "ir";
    case CMD_ABI: return "abi";
    case CMD_BUILD: return "build";
    case CMD_PROVE: return "prove";
  }
  return "?";
}

static int run(const Options *opt, Arena *arena, Diag *diag) {
  char *text = NULL;
  size_t len = 0;
  if (!read_source(arena, opt->prog_path, &text, &len, diag)) return 2;
  DeclList decls;
  if (!front_load(arena, opt->prog_path, text, len, &decls, diag)) return 1;
  Machine machine;
  if (!check_program(arena, &decls, &machine, diag, opt->mode)) return 1;
  switch (opt->command) {
    case CMD_CHECK:
      puts("ok");
      return 0;
    case CMD_EVAL:
      return eval_command(&machine, opt->entry, opt->args, opt->arg_count, stdout, stderr);
    case CMD_RUN: {
      char *script = NULL;
      size_t script_len = 0;
      if (!read_source(arena, opt->script_path, &script, &script_len, diag)) return 2;
      return run_command(&machine, opt->script_path, script, script_len, stdout);
    }
    case CMD_PROVE:
      signal(SIGPIPE, SIG_IGN);
      return prove_command(&machine, opt->entry, opt->arg_count == 1 ? opt->args[0] : NULL, stdout, stderr);
    case CMD_BUILD:
    case CMD_ABI:
      /* The EVM writer is not ported: the cairn target comes in S8. */
      return diag_fail(diag, "REFUSE_LOWER", NULL, "%s: target pending S8", command_word(opt->command)) + 1;
    case CMD_IR:
      break;
  }
  diag_fail(diag, "PLANNED", NULL, "the %s command is not built yet (target pending S8)", command_word(opt->command));
  return 1;
}

int main(int argc, char **argv) {
  Diag diag;
  diag_init(&diag);
  Options opt;
  int kit = argc > 1 && strcmp(argv[1], "--kit") == 0;
  if (!parse_options(argc - kit, argv + kit, &opt, &diag)) {
    diag_print(&diag, stderr);
    return usage(stderr);
  }
  opt.mode = kit ? CHECK_KIT : CHECK_GUEST;
  Arena arena;
  arena_init(&arena, ARENA_LIMIT_BYTES);
  int status = run(&opt, &arena, &diag);
  arena_release(&arena);
  if (status != 0) diag_print(&diag, stderr);
  return status;
}
