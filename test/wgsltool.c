/* The WGSL run tool (S10 B4). `wgsltool MODULE.wgsl ABI.txt` takes the call
   lines of build/wasmtool on stdin (`NAME ARG...`, the same ARG text) and runs
   each entry of the WGSL module that `langc build PROG --wgsl` writes on the
   GPU under deno (WebGPU). It prints one result line per call in the wasmtool
   text: a Nat or Flag in decimal, a U256 in the word_text form, `trap` for a
   trap, and a `bad:` line for a call that does not run. All the calls go to
   deno in one driver program (a temp file on its stdin) that holds the module
   text and the in row of each call. A row is u32 words, low first: a Nat or
   Flag is 2 words, a U256 is 8. The out row of an entry is a status word (0
   ok, 1 trap), then the result. The driver runs the calls of each entry in
   one dispatch, one invocation for each call, and prints one line per call in
   the call order; this tool checks and formats each line. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "front/u256.h"

#define MAX_ENTRIES 256
#define MAX_PARAMS 32
#define MAX_NAME 128
#define MAX_CALLS 65536

typedef struct {
  char name[MAX_NAME];
  size_t param_count;
  int wide_param[MAX_PARAMS]; /* 1: a U256, 0: a Nat or Flag */
  int wide_result;            /* 1: a U256 */
} Entry;

typedef struct {
  Entry entry[MAX_ENTRIES];
  size_t count;
} Abi;

/* The form of one call line: CALL_WORD and CALL_WIDE go to deno, the others
   print their bad line here. */
typedef enum { CALL_WORD, CALL_WIDE, CALL_NO_ENTRY, CALL_BAD_ARG } CallKind;

typedef struct {
  CallKind kind;
  size_t arg; /* the argument number of a CALL_BAD_ARG */
} Call;

static int fail2(const char *what, const char *detail) {
  fprintf(stderr, "wgsltool: %s: %s\n", what, detail);
  return 2;
}

/* One ABI line: `NAME (P, ...) -> R`. Returns 0 on a bad line. */
static int abi_line(Entry *e, const char *line) {
  const char *open = strstr(line, " (");
  const char *close = strstr(line, ") -> ");
  const char *p;
  size_t n = (size_t)(open - line);
  if (open == NULL || close == NULL || close < open || n == 0 || n >= MAX_NAME) return 0;
  memcpy(e->name, line, n);
  e->name[n] = '\0';
  e->param_count = 0;
  for (p = open + 2; p < close && e->param_count < MAX_PARAMS; e->param_count++) {
    const char *end = strchr(p, ']');
    const char *comma;
    e->wide_param[e->param_count] = strncmp(p, "u256", 4) == 0;
    /* A u256 kind has a comma in its brackets, so the next comma is after them. */
    comma = strstr(e->wide_param[e->param_count] && end != NULL ? end : p, ", ");
    p = comma != NULL && comma < close ? comma + 2 : close;
  }
  e->wide_result = strncmp(close + 5, "u256", 4) == 0;
  return p == close;
}

static int read_abi(Abi *abi, const char *path) {
  char line[4096];
  FILE *f = fopen(path, "r");
  int ok = f != NULL;
  abi->count = 0;
  while (ok && fgets(line, sizeof line, f) != NULL) {
    ok = abi->count < MAX_ENTRIES && abi_line(&abi->entry[abi->count], line);
    abi->count += (size_t)ok;
  }
  if (f != NULL) fclose(f);
  return ok;
}

static const Entry *find_entry(const Abi *abi, const char *name) {
  size_t k;
  for (k = 0; k < abi->count; k++) {
    if (strcmp(abi->entry[k].name, name) == 0) return &abi->entry[k];
  }
  return NULL;
}

/* A Nat or Flag argument: decimal, at most 2^64 - 1. */
static int parse_word(const char *text, uint64_t *out) {
  char *end = NULL;
  if (text[0] < '0' || text[0] > '9') return 0;
  *out = strtoull(text, &end, 10);
  return *end == '\0' && !(*out == UINT64_MAX && strcmp(text, "18446744073709551615") != 0);
}

/* A U256 argument: decimal or 0x hex, then u. */
static int parse_u256(const char *text, U256 *out) {
  size_t len = strlen(text);
  int hex = strncmp(text, "0x", 2) == 0;
  size_t k = hex ? 2u : 0u;
  int ok = len > k + 1 && text[len - 1] == 'u';
  u256_from_u64(out, 0);
  for (; ok && k + 1 < len; k++) {
    char c = text[k];
    int digit = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    ok = digit >= 0 && u256_scale_add(out, hex ? 16u : 10u, (uint32_t)digit);
  }
  return ok;
}

/* Writes C in a JS string literal (a character below 0x20, a quote or a
   backslash as a \u escape). */
static void put_js_char(FILE *f, unsigned c) {
  int plain = c >= 0x20u && c != '"' && c != '\\';
  if (plain) fputc((int)c, f);
  if (!plain) fprintf(f, "\\u%04x", c);
}

static void put_js_string(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s != '\0'; s++) put_js_char(f, (unsigned char)*s);
  fputc('"', f);
}

/* The driver head: the text of CODE as a string, and k(name, row, width) adds
   one call. Returns 0 when CODE does not read. */
static int put_head(FILE *f, FILE *code) {
  int c;
  fputs("const code = \"", f);
  for (c = fgetc(code); c != EOF; c = fgetc(code)) put_js_char(f, (unsigned)c);
  fputs("\";\n"
        "const calls = [];\n"
        "const k = (name, row, width) => calls.push({ name, row, width });\n",
        f);
  return !ferror(code);
}

/* The driver tail: the calls of each entry go in one dispatch (at most `per`
   rows, so that the heap slices fit in one binding). An out row starts with
   status 2, so an invocation that writes no status gives a bad line. Each
   call gets one line: `v DIGITS`, `trap` or a bad line. */
static void put_tail(FILE *f) {
  fputs("const out = calls.map(() => 'bad: the call does not run');\n"
        "const adapter = await navigator.gpu.requestAdapter();\n"
        "if (adapter === null) {\n"
        "  console.error('wgsltool: deno gives no GPU adapter');\n"
        "  Deno.exit(3);\n"
        "}\n"
        "const lim = adapter.limits;\n"
        "const device = await adapter.requestDevice({ requiredLimits: { maxStorageBufferBindingSize: "
        "lim.maxStorageBufferBindingSize, maxBufferSize: lim.maxBufferSize } });\n"
        "const heapWords = Number((code.match(/const HEAP_WORDS: u32 = (\\d+)u;/) ?? ['', '0'])[1]);\n"
        "const per = Math.max(1, Math.floor(Math.min(lim.maxStorageBufferBindingSize, lim.maxBufferSize) / "
        "(heapWords * 8)));\n"
        "const module = device.createShaderModule({ code });\n"
        "const U = GPUBufferUsage;\n"
        "const buffer = (words, usage) => device.createBuffer({ size: 4 * Math.max(1, words), usage });\n"
        "const value = (r) => r.reduce((s, w, i) => s + (BigInt(w) << BigInt(32 * i)), 0n);\n"
        "const first = (e) => String(e && e.message ? e.message : e).split('\\n')[0];\n"
        "const run = async (name, ids) => {\n"
        "  const width = calls[ids[0]].width;\n"
        "  const inw = calls[ids[0]].row.length;\n"
        "  const n = ids.length;\n"
        "  const rows = new Uint32Array(Math.max(1, n * inw));\n"
        "  const blank = new Uint32Array(n * width);\n"
        "  ids.forEach((id, j) => rows.set(calls[id].row, j * inw));\n"
        "  ids.forEach((id, j) => (blank[j * width] = 2));\n"
        "  device.pushErrorScope('out-of-memory');\n"
        "  device.pushErrorScope('internal');\n"
        "  device.pushErrorScope('validation');\n"
        "  const pipeline = device.createComputePipeline({ layout: 'auto', compute: { module, entryPoint: name } });\n"
        "  const bufs = [buffer(n * inw, U.STORAGE | U.COPY_DST), buffer(n * width, U.STORAGE | U.COPY_SRC | "
        "U.COPY_DST), buffer(n * heapWords * 2, U.STORAGE)];\n"
        "  const back = buffer(n * width, U.MAP_READ | U.COPY_DST);\n"
        "  device.queue.writeBuffer(bufs[0], 0, rows);\n"
        "  device.queue.writeBuffer(bufs[1], 0, blank);\n"
        "  const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: bufs.map((b, "
        "binding) => ({ binding, resource: { buffer: b } })) });\n"
        "  const enc = device.createCommandEncoder();\n"
        "  const pass = enc.beginComputePass();\n"
        "  pass.setPipeline(pipeline);\n"
        "  pass.setBindGroup(0, group);\n"
        "  pass.dispatchWorkgroups(Math.ceil(n / 64));\n"
        "  pass.end();\n"
        "  enc.copyBufferToBuffer(bufs[1], 0, back, 0, n * width * 4);\n"
        "  device.queue.submit([enc.finish()]);\n"
        "  const invalid = await device.popErrorScope();\n"
        "  const internal = await device.popErrorScope();\n"
        "  const oom = await device.popErrorScope();\n"
        "  const err = internal ?? invalid ?? oom;\n"
        "  if (err !== null) {\n"
        "    ids.forEach((id) => (out[id] = 'bad: ' + first(err.message || err.constructor.name)));\n"
        "    return;\n"
        "  }\n"
        "  try {\n"
        "    await back.mapAsync(GPUMapMode.READ);\n"
        "    const got = new Uint32Array(back.getMappedRange().slice(0));\n"
        "    back.unmap();\n"
        "    ids.forEach((id, j) => {\n"
        "      const r = got.subarray(j * width, (j + 1) * width);\n"
        "      out[id] = r[0] === 0 ? 'v ' + value(Array.from(r.subarray(1))).toString() : r[0] === 1 ? 'trap' : "
        "'bad: the entry writes no status';\n"
        "    });\n"
        "  } catch (e) {\n"
        "    ids.forEach((id) => (out[id] = 'bad: ' + first(e)));\n"
        "  }\n"
        "  [...bufs, back].forEach((b) => b.destroy());\n"
        "};\n"
        "for (const name of [...new Set(calls.map((c) => c.name))]) {\n"
        "  const ids = calls.flatMap((c, i) => (c.name === name ? [i] : []));\n"
        "  for (let a = 0; a < ids.length; a += per) await run(name, ids.slice(a, a + per));\n"
        "}\n"
        "if (out.length > 0) console.log(out.join('\\n'));\n",
        f);
}

/* Reads one call line and writes its in row to F: each argument as 2 u32
   words per 64-bit word, low first. */
static Call read_call(const Abi *abi, char *line, FILE *f) {
  char *word[MAX_PARAMS + 2];
  U256 wide[MAX_PARAMS];
  uint64_t narrow[MAX_PARAMS];
  size_t words = 0;
  size_t k;
  size_t w;
  char *tok;
  const Entry *e;
  Call call;
  call.kind = CALL_NO_ENTRY;
  call.arg = 0;
  for (tok = strtok(line, " \n"); tok != NULL && words < MAX_PARAMS + 2; tok = strtok(NULL, " \n")) word[words++] = tok;
  e = words > 0 ? find_entry(abi, word[0]) : NULL;
  if (e == NULL || words != e->param_count + 1) return call;
  for (k = 0; k < e->param_count && call.arg == 0; k++) {
    narrow[k] = 0;
    call.arg = (e->wide_param[k] ? parse_u256(word[k + 1], &wide[k]) : parse_word(word[k + 1], &narrow[k])) ? 0 : k + 1;
  }
  if (call.arg != 0) {
    call.kind = CALL_BAD_ARG;
    return call;
  }
  fputs("k(", f);
  put_js_string(f, e->name);
  fputs(", [", f);
  for (k = 0; k < e->param_count; k++) {
    for (w = 0; w < (e->wide_param[k] ? 4u : 1u); w++) {
      uint64_t x = e->wide_param[k] ? wide[k].limb[w] : narrow[k];
      fprintf(f, "%s%" PRIu32 ", %" PRIu32, k + w > 0 ? ", " : "", (uint32_t)x, (uint32_t)(x >> 32));
    }
  }
  fprintf(f, "], %d);\n", e->wide_result ? 9 : 3);
  call.kind = e->wide_result ? CALL_WIDE : CALL_WORD;
  return call;
}

/* A U256 result: DIGITS (the decimal that deno prints) in the word_text form.
   LINE is the deno line, NULL when deno gave no more lines. */
static void print_wide(const char *digits, const char *line) {
  U256 x;
  char text[96] = "";
  int ok = digits != NULL && digits[0] != '\0';
  size_t k;
  u256_from_u64(&x, 0);
  for (k = 0; ok && digits[k] != '\0'; k++) {
    ok = digits[k] >= '0' && digits[k] <= '9' && u256_scale_add(&x, 10u, (uint32_t)(digits[k] - '0'));
  }
  if (ok) word_text(WORD_U256, &x, text, sizeof text);
  printf("%s\n", ok                ? text
                 : digits != NULL ? "bad: a U256 result is out of range"
                 : line != NULL   ? line
                                  : "bad: deno gives no result line");
}

/* Prints the result line of one call. LINE is its deno line, NULL when deno
   gave no more lines or the call did not go to deno. */
static void print_result(const Call *call, const char *line) {
  const char *digits = line != NULL && strncmp(line, "v ", 2) == 0 ? line + 2 : NULL;
  switch (call->kind) {
    case CALL_NO_ENTRY:
      printf("bad: no entry or a wrong argument count\n");
      return;
    case CALL_BAD_ARG:
      printf("bad: argument %zu\n", call->arg);
      return;
    case CALL_WORD:
      printf("%s\n", digits != NULL ? digits : line != NULL ? line : "bad: deno gives no result line");
      return;
    case CALL_WIDE:
      print_wide(digits, line);
      return;
  }
}

/* Quote the driver path for the shell used by popen, including any quote
   supplied through TMPDIR. deno reads the driver as JS on its stdin. */
static int deno_command(char *cmd, size_t cap, const char *path) {
  size_t at;
  int n = snprintf(cmd, cap, "deno run --quiet --unstable-webgpu --ext=js - <'");
  if (n < 0 || (size_t)n >= cap) return 0;
  at = (size_t)n;
  for (; *path != '\0'; path++) {
    n = *path == '\'' ? snprintf(cmd + at, cap - at, "%s", "'\\''")
                      : snprintf(cmd + at, cap - at, "%c", *path);
    if (n < 0 || (size_t)n >= cap - at) return 0;
    at += (size_t)n;
  }
  n = snprintf(cmd + at, cap - at, "'");
  return n >= 0 && (size_t)n < cap - at;
}

/* The --valid driver tail: compile the module, then one compute pipeline for
   each @compute entry (the S10 r3 P2b probe). It prints one `error` line for
   each compile error and each bad pipeline, then `valid N entries` (exit 0)
   or `invalid N errors` (exit 1). */
static void put_valid_tail(FILE *f) {
  fputs("const cut = (s) => s.split('\\n').filter((l) => l.trim() !== '').slice(0, 4).join(' | ').slice(0, 400);\n"
        "const adapter = await navigator.gpu.requestAdapter();\n"
        "if (adapter === null) {\n"
        "  console.error('wgsltool: deno gives no GPU adapter');\n"
        "  Deno.exit(3);\n"
        "}\n"
        "const device = await adapter.requestDevice();\n"
        "const names = [...code.matchAll(/@compute[^\\n]*\\nfn (\\w+)/g)].map((x) => x[1]);\n"
        "device.pushErrorScope('validation');\n"
        "const sm = device.createShaderModule({ code });\n"
        "const info = typeof sm.getCompilationInfo === 'function'\n"
        "  ? await sm.getCompilationInfo()\n"
        "  : { messages: [] };\n"
        "const scope = await device.popErrorScope();\n"
        "const errors = info.messages.filter((msg) => msg.type === 'error');\n"
        "errors.forEach((msg) => console.log(`error ${msg.lineNum}:${msg.linePos} ${cut(msg.message)}`));\n"
        "if (scope !== null) console.log(`error scope ${cut(scope.message)}`);\n"
        "const pipeline = async (n) => {\n"
        "  device.pushErrorScope('internal');\n"
        "  device.pushErrorScope('validation');\n"
        "  const thrown = (() => {\n"
        "    try {\n"
        "      device.createComputePipeline({ layout: 'auto', compute: { module: sm, entryPoint: n } });\n"
        "      return '';\n"
        "    } catch (e) {\n"
        "      return cut(String(e));\n"
        "    }\n"
        "  })();\n"
        "  const invalid = await device.popErrorScope();\n"
        "  const internal = await device.popErrorScope();\n"
        "  const e = internal ?? invalid;\n"
        "  const bad = thrown !== '' || e !== null;\n"
        "  if (bad) console.log(`error ${n} pipeline ${thrown}${e ? cut(e.message || e.constructor.name) : ''}`);\n"
        "  return bad ? 1 : 0;\n"
        "};\n"
        "const badPipelines = await names.reduce(async (acc, n) => (await acc) + (await pipeline(n)), Promise.resolve(0));\n"
        "const bad = errors.length + (scope !== null ? 1 : 0) + badPipelines;\n"
        "console.log(bad === 0 ? `valid ${names.length} entries` : `invalid ${bad} errors`);\n"
        "Deno.exit(bad === 0 ? 0 : 1);\n",
        f);
}

/* wgsltool --valid MODULE.wgsl: no ABI and no calls. The deno lines go to
   stdout as they come. */
static int valid_module(const char *module, char *tmpl) {
  static char line[4096];
  static char cmd[4u * 4096u + 128u];
  int head;
  int fd;
  int status;
  FILE *code = fopen(module, "r");
  FILE *f;
  FILE *deno;
  if (code == NULL) return fail2(module, "cannot open");
  fd = mkstemp(tmpl);
  if (fd < 0) {
    fclose(code);
    return fail2(tmpl, "cannot make the driver file");
  }
  f = fdopen(fd, "w");
  if (f == NULL) {
    close(fd);
    unlink(tmpl);
    fclose(code);
    return fail2(tmpl, "cannot write the driver file");
  }
  head = put_head(f, code);
  fclose(code);
  put_valid_tail(f);
  if (fclose(f) != 0 || !head) {
    unlink(tmpl);
    return fail2(head ? tmpl : module, head ? "cannot write the driver file" : "cannot read");
  }
  if (!deno_command(cmd, sizeof cmd, tmpl)) {
    unlink(tmpl);
    return fail2(tmpl, "the deno command is too long");
  }
  deno = popen(cmd, "r");
  if (deno == NULL) {
    unlink(tmpl);
    return fail2("deno", "does not start");
  }
  for (; fgets(line, sizeof line, deno) != NULL;) fputs(line, stdout);
  status = pclose(deno);
  unlink(tmpl);
  return status == 0 ? 0 : fail2("deno", "exits with an error");
}

int main(int argc, char **argv) {
  static Abi abi;
  static Call call[MAX_CALLS];
  static char line[4096];
  static char tmpl[4096];
  static char cmd[4u * sizeof tmpl + 128u];
  const char *dir = getenv("TMPDIR");
  size_t calls = 0;
  size_t k;
  int more;
  int head;
  int fd;
  int status;
  FILE *code;
  FILE *f;
  FILE *deno;
  if (argc != 3) return fail2("usage", "wgsltool MODULE.wgsl ABI.txt (calls on stdin), or wgsltool --valid MODULE.wgsl");
  if (snprintf(tmpl, sizeof tmpl, "%s/wgsltool-XXXXXX", dir != NULL && dir[0] != '\0' ? dir : "/tmp") >= (int)sizeof tmpl)
    return fail2("TMPDIR", "the path is too long");
  if (strcmp(argv[1], "--valid") == 0) return valid_module(argv[2], tmpl);
  if (!read_abi(&abi, argv[2])) return fail2(argv[2], "not a langc abi --text file");
  code = fopen(argv[1], "r");
  if (code == NULL) return fail2(argv[1], "cannot open");
  fd = mkstemp(tmpl);
  if (fd < 0) {
    fclose(code);
    return fail2(tmpl, "cannot make the driver file");
  }
  f = fdopen(fd, "w");
  if (f == NULL) {
    close(fd);
    unlink(tmpl);
    fclose(code);
    return fail2(tmpl, "cannot write the driver file");
  }
  head = put_head(f, code);
  fclose(code);
  more = fgets(line, sizeof line, stdin) != NULL;
  for (; more && calls < MAX_CALLS; more = fgets(line, sizeof line, stdin) != NULL) call[calls++] = read_call(&abi, line, f);
  put_tail(f);
  if (fclose(f) != 0 || more || !head) {
    unlink(tmpl);
    return fail2(head ? tmpl : argv[1], !head ? "cannot read" : more ? "more than 65536 calls" : "cannot write the driver file");
  }
  if (!deno_command(cmd, sizeof cmd, tmpl)) {
    unlink(tmpl);
    return fail2(tmpl, "the deno command is too long");
  }
  deno = popen(cmd, "r");
  if (deno == NULL) {
    unlink(tmpl);
    return fail2("deno", "does not start");
  }
  for (k = 0; k < calls; k++) {
    int to_deno = call[k].kind == CALL_WORD || call[k].kind == CALL_WIDE;
    int got = to_deno && fgets(line, sizeof line, deno) != NULL;
    line[got ? strcspn(line, "\n") : 0] = '\0';
    print_result(&call[k], got ? line : NULL);
  }
  status = pclose(deno);
  unlink(tmpl);
  return status == 0 ? 0 : fail2("deno", "exits with an error");
}
