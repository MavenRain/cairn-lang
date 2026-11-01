/* The langc command line. SHARED by the tcc kits.
   Exit 0: ok. Exit 1: the program is refused. Exit 2: usage or IO. */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "front/check.h"
#include "front/fmt.h"
#include "front/json.h"
#include "front/lexer.h"
#include "front/parser.h"
#include "js.h"
#include "wgsl.h"
#include "front/front.h"
#include "front/lower.h"
#include "target.h"
#include "front/codes.h"
#include "mcp.h"

#define ARENA_LIMIT_BYTES ((size_t)1 << 30)

typedef enum {
  CMD_CHECK,
  CMD_EVAL,
  CMD_RUN,
  CMD_IR,
  CMD_ABI,
  CMD_BUILD,
  CMD_PROVE,
  CMD_FMT,
  CMD_EXPLAIN,
  CMD_MCP
} Command;

typedef struct {
  Command command;
  const char *prog_path;
  const char *entry;
  char *const *args; /* argv, or the arguments of the MCP eval tool */
  int arg_count;
  const char *script_path;
  const char *out_path;
  int js; /* --js OUT: the ES module in place of the target (S9 B2) */
  int wgsl; /* --wgsl OUT: the WGSL compute module in place of the target (S10 B2) */
  int runtime;
  int json;
  int fmt_check; /* fmt --check (S11 A1): compare PROG with its canonical text */
  CheckMode mode; /* orbit-lang S3: --kit selects the kit mode */
  const char *code; /* explain CODE (S13 Q-S13-7 (a)) */
  FILE *out; /* stdout, or the output of an MCP tool (S13 Q-S13-3 (a)) */
  FILE *err; /* stderr, or the error output of an MCP tool */
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
  {"fmt", CMD_FMT},
  {"explain", CMD_EXPLAIN},
  {"mcp", CMD_MCP},
};

static int usage(FILE *err) {
  fputs("usage: langc [--kit] check [--json] PROG\n"
        "       langc [--kit] eval PROG NAME [ARGS...]\n"
        "       langc [--kit] run PROG SCRIPT\n"
        "       langc [--kit] ir PROG\n"
        "       langc [--kit] abi PROG [--text]\n"
        "       langc [--kit] build PROG (-o OUT | --js OUT.mjs | --wgsl OUT.wgsl) [--runtime]\n"
        "       langc [--kit] prove PROG NAME [POLL]\n"
        "       langc fmt PROG [--check]\n"
        "       langc explain CODE\n"
        "       langc [--kit] mcp\n"
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
  int outs = 0;
  opt->js = 0;
  opt->wgsl = 0;
  for (int i = 3; i < argc; i++) {
    int is_js = strcmp(argv[i], "--js") == 0;
    int is_wgsl = strcmp(argv[i], "--wgsl") == 0;
    int is_out = (strcmp(argv[i], "-o") == 0 || is_js || is_wgsl) && i + 1 < argc;
    int is_runtime = strcmp(argv[i], "--runtime") == 0;
    if (!is_out && !is_runtime) return diag_fail(diag, "USAGE", NULL, "unknown build argument '%s'", argv[i]);
    opt->out_path = is_out ? argv[i + 1] : opt->out_path;
    opt->js = is_out ? is_js : opt->js;
    opt->wgsl = is_out ? is_wgsl : opt->wgsl;
    outs += is_out;
    opt->runtime |= is_runtime;
    i += is_out;
  }
  /* Q-S8-8: build writes a file, so -o OUT is required. S10 B2: build writes
     one file, so it takes one output flag only. */
  if (outs > 1) return diag_fail(diag, "USAGE", NULL, "build takes one of -o OUT, --js OUT and --wgsl OUT");
  return opt->out_path != NULL ? 1 : diag_fail(diag, "USAGE", NULL, "build needs -o OUT, --js OUT or --wgsl OUT");
}

static int parse_options(int argc, char **argv, Options *opt, Diag *diag) {
  memset(opt, 0, sizeof *opt);
  if (argc < 2 || !find_command(argv[1], &opt->command) || (argc < 3 && opt->command != CMD_MCP)) {
    return diag_fail(diag, "USAGE", NULL, "expected a command and a program path");
  }
  opt->prog_path = argv[2];
  switch (opt->command) {
    case CMD_CHECK:
      /* check --json PROG (S12 A2, the PLAN A2 row): the json_check line. */
      opt->json = strcmp(argv[2], "--json") == 0;
      if (opt->json && argc < 4) return diag_fail(diag, "USAGE", NULL, "check --json needs a program path");
      opt->prog_path = argv[2 + opt->json];
      return argc == 3 + opt->json ? 1 : diag_fail(diag, "USAGE", NULL, "too many arguments");
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
    case CMD_FMT:
      opt->fmt_check = argc == 4;
      return argc == 3 || (argc == 4 && strcmp(argv[3], "--check") == 0) ? 1
        : diag_fail(diag, "USAGE", NULL, "fmt takes only --check after the program");
    case CMD_EXPLAIN:
      opt->code = argv[2];
      return argc == 3 ? 1 : diag_fail(diag, "USAGE", NULL, "explain takes only a code");
    case CMD_MCP:
      return argc == 2 ? 1 : diag_fail(diag, "USAGE", NULL, "mcp takes no arguments");
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
    case CMD_FMT: return "fmt";
    case CMD_EXPLAIN: return "explain";
    case CMD_MCP: return "mcp";
  }
  return "?";
}

/* One character of a JSON string. */
static void json_char(unsigned char c, FILE *out) {
  switch (c) {
    case '"':
    case '\\':
      fprintf(out, "\\%c", c);
      return;
    default:
      if (c < 0x20)
        fprintf(out, "\\u%04x", (unsigned)c);
      else
        fputc(c, out);
      return;
  }
}

/* The JSON abi (Q-S9-5): the kind of a scalar, without the Wasm layout. */
static const char *json_shape(IrScalar sc) {
  switch (sc) {
    case IR_SCALAR_NAT:
      return "nat";
    case IR_SCALAR_FLAG:
      return "flag";
    case IR_SCALAR_U256:
      return "u256";
  }
  return "?";
}

/* Entry FN as a JSON object, after a comma unless FIRST. Returns 1. */
static int json_abi_entry(const IrFunc *fn, int first, FILE *out) {
  const unsigned char *c;
  size_t k;
  fputs(first ? "{\"name\":\"" : ",{\"name\":\"", out);
  for (c = (const unsigned char *)fn->name; *c != '\0'; c++) json_char(*c, out);
  fputs("\",\"params\":[", out);
  for (k = 0; k < fn->param_count; k++) fprintf(out, "%s\"%s\"", k == 0 ? "" : ",", json_shape(fn->params[k]));
  fprintf(out, "],\"result\":\"%s\"}", json_shape(fn->result));
  return 1;
}

/* The ABI comes from checked types, independently of lowering. An omitted
   IR function must not disappear from both sides of the gate's count check. */
static int print_abi(Machine *machine, int json, FILE *out, FILE *err) {
  uint32_t i;
  uint32_t k;
  uint32_t count = 0;
  Entry entry;
  IrFunc fn;
  IrScalar params[ENTRY_PARAMS_MAX];
  memset(&fn, 0, sizeof fn);
  fn.params = params;
  if (json) fputs("{\"entries\":[", out);
  for (i = 0; i < machine->def_count; i++) {
    machine->fuel = EVAL_FUEL_STEPS;
    if (machine->defs[i].origin != ORIGIN_PROGRAM || !entry_of(machine, machine->defs[i].type, &entry))
      continue;
    fn.name = machine->defs[i].name;
    fn.param_count = entry.param_count;
    for (k = 0; k < entry.param_count; k++)
      params[k] = entry.param_flag[k] ? IR_SCALAR_FLAG : entry.param_u256[k] ? IR_SCALAR_U256 : IR_SCALAR_NAT;
    fn.result = entry.result_flag ? IR_SCALAR_FLAG : entry.result_u256 ? IR_SCALAR_U256 : IR_SCALAR_NAT;
    if (!(json ? json_abi_entry(&fn, count++ == 0, out) : target_abi_line(&fn, out, err)))
      return 1;
  }
  if (json) fputs("]}\n", out);
  return machine->diag->set ? 1 : 0;
}

/* Lowers the entries, then runs the ir or build command (the kit target). */
static int run_target(const Options *opt, Machine *machine, Diag *diag) {
  IrProgram prog;
  FILE *out;
  int ok = 1;
  if (opt->command == CMD_ABI) return print_abi(machine, opt->json, opt->out, opt->err);
  if (!lower_program(machine, &prog, opt->err)) return 1;
  switch (opt->command) {
    case CMD_IR:
      ir_print(&prog, opt->out);
      return 0;
    case CMD_ABI:
    case CMD_CHECK:
    case CMD_EVAL:
    case CMD_RUN:
    case CMD_PROVE:
    case CMD_BUILD:
    case CMD_FMT:
    case CMD_EXPLAIN:
    case CMD_MCP:
      break;
  }
  out = fopen(opt->out_path, "wb");
  if (out == NULL) return diag_fail(diag, "IO", NULL, "cannot write '%s'", opt->out_path) + 2;
  ok = opt->wgsl ? wgsl_write(&prog, out, opt->err)
       : opt->js ? js_write(&prog, out, opt->err)
                 : target_write(&prog, opt->runtime ? TARGET_PART_RUNTIME : TARGET_PART_MAIN, out, opt->err);
  ok = fclose(out) == 0 && ok;
  if (ok) return 0;
  remove(opt->out_path);
  diag_fail(diag, "BUILD", NULL, "no module written; '%s' removed", opt->out_path);
  return 1;
}

/* langc [--kit] check --json PROG (S12 A2): exit 0 or 1 with the json_check
   line on stdout, and main prints no diag (Q-S12-7). After a lex or parse
   error the progress stays all zero (Q-S12-6). OOM is exit 2 with no JSON. */
static int check_json(const Options *opt, Arena *arena, const char *text, size_t len, Diag *diag) {
  CheckProgress progress;
  DeclList decls;
  Machine machine;
  memset(&progress, 0, sizeof progress);
  int ok = front_load(arena, opt->prog_path, text, len, &decls, diag)
      && check_program_progress(arena, &decls, &machine, diag, opt->mode, &progress);
  if (!ok && diag->code != NULL && strcmp(diag->code, "OOM") == 0) return 2;
  json_check(opt->out, opt->prog_path, diag, &progress);
  return ok ? 0 : 1;
}

static int run(const Options *opt, Arena *arena, Diag *diag) {
  char *text = NULL;
  size_t len = 0;
  if (!read_source(arena, opt->prog_path, &text, &len, diag)) return 2;
  if (opt->command == CMD_CHECK && opt->json) return check_json(opt, arena, text, len, diag);
  DeclList decls;
  if (!front_load(arena, opt->prog_path, text, len, &decls, diag)) return 1;
  Machine machine;
  if (!check_program(arena, &decls, &machine, diag, opt->mode)) return 1;
  switch (opt->command) {
    case CMD_CHECK:
      fputs("ok\n", opt->out);
      return 0;
    case CMD_EVAL:
      return eval_command(&machine, opt->entry, opt->args, opt->arg_count, opt->out, opt->err);
    case CMD_RUN: {
      char *script = NULL;
      size_t script_len = 0;
      if (!read_source(arena, opt->script_path, &script, &script_len, diag)) return 2;
      return run_command(&machine, opt->script_path, script, script_len, opt->out);
    }
    case CMD_PROVE:
      signal(SIGPIPE, SIG_IGN);
      return prove_command(&machine, opt->entry, opt->arg_count == 1 ? opt->args[0] : NULL, opt->out, opt->err);
    case CMD_ABI:
    case CMD_IR:
    case CMD_BUILD:
      return run_target(opt, &machine, diag);
    case CMD_FMT: /* main runs run_fmt */
    case CMD_EXPLAIN: /* main runs run_explain */
    case CMD_MCP: /* main runs mcp_serve */
      break;
  }
  diag_fail(diag, "INTERNAL", NULL, "no handler for the %s command", command_word(opt->command));
  return 1;
}

/* The line of the first byte where TEXT and the canonical text CANON
   differ (1 + the newlines in the common prefix), or 0 when they are equal. */
static size_t first_diff_line(const char *text, size_t len, const FmtText *canon) {
  size_t same = 0;
  size_t line = 1;
  for (; same < len && same < canon->len && text[same] == canon->data[same]; same++) line += text[same] == '\n' ? 1u : 0u;
  return same == len && same == canon->len ? 0 : line;
}

/* langc fmt (S11 A1, ported from charter-lang): lexes and parses the
   program only (no domain, no check), then prints the canonical text. With
   --check it prints nothing when the file is canonical, else
   `PROG:LINE: not canonical` at the first line that differs (exit 1). */
static int run_fmt(const Options *opt, Arena *arena, Diag *diag) {
  char *text = NULL;
  size_t len = 0;
  if (!read_source(arena, opt->prog_path, &text, &len, diag)) return 2;
  TokenList toks;
  DeclList decls = {NULL, 0, 0};
  if (!lex_source(arena, opt->prog_path, text, len, &toks, diag)) return 1;
  if (!parse_source(arena, opt->prog_path, ORIGIN_PROGRAM, &toks, &decls, diag)) return 1;
  FmtText out;
  fmt_text_init(&out, arena);
  if (!fmt_program(&out, decls.items, decls.count, &toks)) {
    return diag_fail(diag, "OOM", NULL, "no memory for the canonical text of %s", opt->prog_path) + 2;
  }
  if (!fmt_validate(&out, opt->prog_path, diag)) return strcmp(diag->code, "OOM") == 0 ? 2 : 1;
  if (opt->fmt_check) {
    size_t line = first_diff_line(text, len, &out);
    if (line != 0) fprintf(opt->err, "%s:%zu: not canonical\n", opt->prog_path, line);
    return line != 0;
  }
  size_t written = out.len == 0 ? 0 : fwrite(out.data, 1, out.len, opt->out);
  if (written != out.len || fflush(opt->out) != 0) return diag_fail(diag, "IO", NULL, "cannot write the canonical text") + 2;
  return 0;
}

/* langc explain CODE (S13 Q-S13-7 (a), Q-S13-10 (a)): one line on OUT,
   `CODE: MEANING (docs/host/README.md, SECTION)`, and exit 0. An unknown
   code is exit 1 with an EXPLAIN diag. It reads no program and uses no
   arena, so the MCP tool can call it with its own OUT. */
static int run_explain(const Options *opt, Diag *diag, FILE *out) {
  const CodeRow *row = code_find(opt->code);
  if (row == NULL) return diag_fail(diag, "EXPLAIN", NULL, "'%s' is not a code of docs/host/README.md", opt->code) + 1;
  fprintf(out, "%s: %s (docs/host/README.md, %s)\n", row->code, row->meaning, row->section);
  return fflush(out) == 0 ? 0 : diag_fail(diag, "IO", NULL, "cannot write the explain line") + 2;
}

/* The diag rule of main, on the err of OPT: fmt --check prints its own
   `PROG:LINE: not canonical` line, with no diag; check --json puts the
   diag of exit 1 in its JSON line (Q-S12-7). Returns STATUS. */
static int finish_status(const Options *opt, Diag *diag, int status) {
  int in_json = opt->command == CMD_CHECK && opt->json && status == 1;
  if (status != 0 && !in_json && (opt->command != CMD_FMT || diag->set)) diag_print(diag, opt->err);
  return status;
}

/* The check mode of `langc [--kit] mcp`: main sets it before mcp_serve. */
static CheckMode mcp_mode = CHECK_GUEST;

/* TOOL on OPT, ARENA and DIAG, as the CLI runs the verb. mcp.c checks ARGS
   before the call, so each case trusts the fields of its tool. */
static int mcp_run(McpTool tool, const McpArgs *args, Options *opt, Arena *arena, Diag *diag) {
  switch (tool) {
    case MCP_TOOL_CHECK:
      opt->command = CMD_CHECK;
      opt->json = 1;
      return run(opt, arena, diag);
    case MCP_TOOL_FMT:
      opt->command = CMD_FMT;
      return run_fmt(opt, arena, diag);
    case MCP_TOOL_EVAL:
      opt->command = CMD_EVAL;
      opt->entry = args->name;
      opt->args = args->args;
      opt->arg_count = args->arg_count;
      return run(opt, arena, diag);
    case MCP_TOOL_BUILD:
      opt->command = CMD_BUILD;
      opt->out_path = args->out_path;
      opt->runtime = args->target == MCP_TARGET_RUNTIME;
      opt->js = args->target == MCP_TARGET_JS;
      opt->wgsl = args->target == MCP_TARGET_WGSL;
      return run(opt, arena, diag);
    case MCP_TOOL_EXPLAIN:
      opt->command = CMD_EXPLAIN;
      opt->code = args->code;
      return run_explain(opt, diag, opt->out);
  }
  return diag_fail(diag, "INTERNAL", NULL, "no handler for the MCP tool") + 1;
}

/* The tools of `langc mcp` (mcp.h): each call has fresh Options, a fresh
   Diag and a fresh Arena that is released after the call (S13 Q-S13-3
   (a)), and prints its output on OUT and its diag on ERR. */
static int mcp_tool(McpTool tool, const McpArgs *args, FILE *out, FILE *err) {
  Options opt;
  memset(&opt, 0, sizeof opt);
  opt.mode = mcp_mode;
  opt.out = out;
  opt.err = err;
  opt.prog_path = args->path;
  Diag diag;
  diag_init(&diag);
  Arena arena;
  arena_init(&arena, ARENA_LIMIT_BYTES);
  int status = mcp_run(tool, args, &opt, &arena, &diag);
  arena_release(&arena);
  return finish_status(&opt, &diag, status);
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
  opt.out = stdout;
  opt.err = stderr;
  /* mcp serves the tools on stdin and stdout to the end of stdin; each
     call has its own Options, Diag and Arena (S13 Q-S13-3 (a)). */
  if (opt.command == CMD_MCP) {
    mcp_mode = opt.mode;
    return mcp_serve(stdin, stdout, mcp_tool);
  }
  /* explain runs before the arena work (S13 Q-S13-7 (a)). */
  if (opt.command == CMD_EXPLAIN) return finish_status(&opt, &diag, run_explain(&opt, &diag, opt.out));
  Arena arena;
  arena_init(&arena, ARENA_LIMIT_BYTES);
  int status = opt.command == CMD_FMT ? run_fmt(&opt, &arena, &diag) : run(&opt, &arena, &diag);
  arena_release(&arena);
  return finish_status(&opt, &diag, status);
}
