# `langc mcp`

`langc mcp` (S13, A4) is a Model Context Protocol (MCP) server on stdin
and stdout. It gives the verbs `check --json`, `fmt`, `eval`, `build`
and `explain` of `langc` to an agent as 5 tools. The server is in
`src/mcp.c`. It calls no function of `src/main.c`: `src/main.c` gives it
the verbs as a callback (Q-S13-3 (a)). The rulings Q-S13-1 to Q-S13-10
are in `cairn-lang-work/S13-survey.md` section 4.

## Usage

```sh
build/langc mcp
build/langc --kit mcp
```

With `--kit`, each tool runs in the kit mode, as `langc --kit VERB`. At
EOF the server exits 0, also after error replies. It exits 2 when stdin
cannot be read or stdout cannot be written.

## Framing

- The server reads one JSON-RPC 2.0 message on each line of stdin until
  EOF, and writes each reply on one line of stdout.
- The longest line is 1048576 bytes (1 MiB, `MCP_LINE_MAX_BYTES` in
  `src/mcp.h`), without the newline. A longer line gives -32700 with id
  null. The server drops the rest of that line and reads on (Q-S13-2).
- A line that is not JSON gives -32700 with id null.
- An id is absent, a string or an integer. A message that is not a valid
  request gives -32600, with its id when the id is valid, else null.
- A notification (a valid request without an id) gets no reply, also
  when its method is not known.

## Version

`initialize` always gives the protocol revision `2025-11-25`, only the
capability `tools`, and the server info
`{"name":"cairn-lang","version":"S13"}` (Q-S13-1). The request must have
a `protocolVersion` string in its params, else it gives -32602. The
server does not compare that string with its own revision.

## Methods

| Method | Result |
|---|---|
| `initialize` | the revision, the capabilities and the server info (Version) |
| `ping` | `{}` |
| `tools/list` | the 5 tools, each with its `name`, `description` and `inputSchema` |
| `tools/call` | the tool result (Tools) |

Each other method gives -32601. The server does not need `initialize`
before another method.

## Tools

| Tool | Arguments (`inputSchema`) | The CLI verb |
|---|---|---|
| `check` | `path` | `langc check --json PATH` (`docs/CHECK-JSON.md`) |
| `fmt` | `path` | `langc fmt PATH` |
| `eval` | `path`, `name`, `args` (optional, an array of strings) | `langc eval PATH NAME ARGS...` |
| `build` | `path`, `target`, `out` | `langc build PATH` with the target |
| `explain` | `code` | `langc explain CODE` |

Each argument is a string, except `args`. Each argument is required,
except `args`. Strings passed to a tool, including each eval argument,
must not contain a NUL byte; these give -32602. The `target` of `build`
is one of 4 strings (Q-S13-6):
`main` gives `-o OUT`, `runtime` gives `--runtime -o OUT`, `js` gives
`--js OUT` and `wgsl` gives `--wgsl OUT`. The `build` tool writes the
file `out`.

A tool result is

```json
{"content":[{"type":"text","text":"STDOUT"},{"type":"text","text":"STDERR"}],"isError":false}
```

The first item is the stdout of the CLI verb. The second item is its
stderr, and it is there only when the stderr is not empty. Invalid
UTF-8 bytes in either stream become `\ufffd`, including an incomplete
character in a truncated diagnostic. `isError` is `true` when the exit
code of the verb is not 0 (Q-S13-5). Thus a refused
program or an unknown code of `explain` is a tool result with `isError`
true, not a JSON-RPC error. A trap of `eval` exits 0, thus its result
has `isError` false and the `EVAL_OVERFLOW` line in the second item.

## Errors

| Code | Cause |
|---|---|
| -32700 | The line is not JSON, or it is longer than 1 MiB. The id is null. |
| -32600 | The message is not a valid request (`Invalid Request`). |
| -32601 | The method is not one of the 4 methods. |
| -32602 | `initialize` has no `protocolVersion` string, or a `tools/call` has an unknown tool, a missing argument, an argument of the wrong type or an unknown `target`. |
| -32603 | The server cannot run the tool (`Internal error`). |

## Limits and state

- The server keeps no state between messages. Each message gets a fresh
  arena (1 GiB limit, `MCP_ARENA_LIMIT_BYTES` in `src/mcp.c`), and each
  `tools/call` runs its verb with fresh options and a fresh diagnostic
  (Q-S13-3).
- The server gives tools only: no `resources`, no `prompts`, no
  cancellation and no progress notices (PLAN O35).
- Each tool reads its program from the file `path`. No tool takes the
  source text (Q-S13-4, PLAN O36). A relative path starts at the working
  directory of the server.

## The test rows

`test/mcp/` has 9 rows. Each `X.in` is a list of messages that starts
with `initialize` and `notifications/initialized`, and `X.out` has the
exact bytes of the replies. `build/mcptool` (`test/mcptool.c`) runs
`langc mcp` on each row (`langc --kit mcp` when `X` starts with `kit-`).
The stdout must give the bytes of `X.out`, the stderr must be empty and
the exit status must be 0. The gate also runs `build/mcptool` on 3 bad
inputs (a changed `.out`, a missing `.out` and no rows), and each must
give exit 1. The gate rows are `mcp rows: 9 passed`,
`mcp negatives: 3 of 3`, `mcp tools: 5 of 5` and
`explain codes: 17 of 17` (`docs/host/README.md`, Gate;
`docs/VALIDATION.md`, V19).
