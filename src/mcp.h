/* The MCP stdio server of `langc mcp` (S13 A4, Q-S13-1 to Q-S13-6). The S13
   port of evolve-lang src/mcp.h (8c5d8e1). This file calls no function of
   main.c: main.c gives the verbs as a callback (Q-S13-3 (a)). */
#ifndef LANG_MCP_H
#define LANG_MCP_H

#include <stdio.h>

/* The longest request line that the server reads, without the newline. A
   longer line gives -32700 with id null; the server drops the rest of the
   line and reads on (Q-S13-2 (a)). */
#define MCP_LINE_MAX_BYTES ((size_t)1 << 20)

/* The protocol revision that `initialize` always gives (Q-S13-1 (a)). */
#define MCP_PROTOCOL_VERSION "2025-11-25"

typedef enum {
  MCP_TOOL_CHECK,
  MCP_TOOL_FMT,
  MCP_TOOL_EVAL,
  MCP_TOOL_BUILD,
  MCP_TOOL_EXPLAIN
} McpTool;

/* The `target` of the build tool (Q-S13-6 (a)): main is `-o OUT`, runtime
   is `--runtime -o OUT`, js is `--js OUT` and wgsl is `--wgsl OUT`. */
typedef enum {
  MCP_TARGET_MAIN,
  MCP_TARGET_RUNTIME,
  MCP_TARGET_JS,
  MCP_TARGET_WGSL
} McpTarget;

/* The arguments of one tools/call (Q-S13-4 (a)). The callback reads only
   the fields of its tool. */
typedef struct {
  const char *path;     /* check, fmt, eval, build: the program file */
  const char *name;     /* eval: the definition name */
  char *const *args;    /* eval: ARG_COUNT strings */
  int arg_count;
  McpTarget target;     /* build */
  const char *out_path; /* build: the file of the module */
  const char *code;     /* explain: the refusal code */
} McpArgs;

/* Runs the verb of TOOL as the CLI does (`check --json PATH`, `fmt PATH`,
   `eval PATH NAME ARGS`, `build` with TARGET and OUT_PATH, `explain CODE`)
   with fresh Options, a fresh Diag and a fresh Arena (Q-S13-3 (a)). Writes
   the CLI stdout to OUT and the CLI stderr to ERR. Returns the CLI exit
   code. */
typedef int (*McpRunTool)(McpTool tool, const McpArgs *args, FILE *out, FILE *err);

/* Reads one JSON-RPC 2.0 message on each line of IN until EOF and writes
   each reply on one line of OUT. Returns 0, or 2 when IN cannot be read or
   OUT cannot be written. */
int mcp_serve(FILE *in, FILE *out, McpRunTool run_tool);

#endif
