/* The JS run tool (S9 B4, Q-S9-6 (a)). `jstool MODULE.mjs ABI.txt` takes the
   call lines of build/wasmtool on stdin (`NAME ARG...`, the same ARG text)
   and runs each entry of the ES module that `langc build PROG --js` writes
   under node. It prints one result line per call in the wasmtool text: a Nat
   or Flag in decimal, a U256 in the word_text form, `trap` for a trap, and a
   `bad:` line for a call that does not run. All the calls go to node in one
   driver program (a temp file on its stdin); node prints one line per call,
   and this tool checks and formats each line. node has a heap cap of 2048
   MiB. */
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

/* The form of one call line: CALL_WORD and CALL_WIDE go to node, the others
   print their bad line here. */
typedef enum { CALL_WORD, CALL_WIDE, CALL_NO_ENTRY, CALL_BAD_ARG } CallKind;

typedef struct {
  CallKind kind;
  size_t arg; /* the argument number of a CALL_BAD_ARG */
} Call;

static int fail2(const char *what, const char *detail) {
  fprintf(stderr, "jstool: %s: %s\n", what, detail);
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

/* Writes S as a JS string literal (each character below 0x20, each quote and
   each backslash as a \u escape). */
static void put_js_string(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s != '\0'; s++) {
    unsigned c = (unsigned char)*s;
    int plain = c >= 0x20u && c != '"' && c != '\\';
    if (plain) fputc((int)c, f);
    if (!plain) fprintf(f, "\\u%04x", c);
  }
  fputc('"', f);
}

/* A static import takes a URL. Encode the absolute path so #, ?, % and
   characters outside ASCII remain part of the file name. */
static void put_module_url(FILE *f, const char *path) {
  fputs("\"file://", f);
  for (; *path != '\0'; path++) {
    unsigned c = (unsigned char)*path;
    int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '/' || c == '-' ||
                c == '_' || c == '.' || c == '~';
    if (plain) fputc((int)c, f);
    if (!plain) fprintf(f, "%%%02X", c);
  }
  fputc('"', f);
}

/* The driver head: it loads the module at PATH, and c(f) adds the result line
   of one call: `v DIGITS` for a BigInt result, `trap` for an Error with the
   message TRAP, else a bad line. */
static void put_head(FILE *f, const char *path) {
  /* Dynamic import calls an export named then as a Promise callback. A
     static namespace import preserves every legal Cairn entry name. */
  fputs("import * as m from ", f);
  put_module_url(f, path);
  fputs(";\n"
        "const out = [];\n"
        "const c = (f) => {\n"
        "  try {\n"
        "    const r = f();\n"
        "    out.push(typeof r === 'bigint' ? 'v ' + r.toString() : 'bad: the result is not a BigInt');\n"
        "  } catch (e) {\n"
        "    out.push(e instanceof Error && e.message === 'TRAP' ? 'trap' : 'bad: the entry does not run');\n"
        "  }\n"
        "};\n",
        f);
}

/* Reads one call line and writes its node call to F. */
static Call read_call(const Abi *abi, char *line, FILE *f) {
  char *word[MAX_PARAMS + 2];
  U256 wide[MAX_PARAMS];
  uint64_t narrow[MAX_PARAMS];
  size_t words = 0;
  size_t k;
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
  fputs("c(() => m[", f);
  put_js_string(f, e->name);
  fputs("](", f);
  for (k = 0; k < e->param_count; k++) {
    fputs(k > 0 ? ", " : "", f);
    if (e->wide_param[k])
      fprintf(f, "0x%016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%016" PRIx64 "n", wide[k].limb[3], wide[k].limb[2],
              wide[k].limb[1], wide[k].limb[0]);
    if (!e->wide_param[k]) fprintf(f, "%" PRIu64 "n", narrow[k]);
  }
  fputs("));\n", f);
  call.kind = e->wide_result ? CALL_WIDE : CALL_WORD;
  return call;
}

/* A U256 result: DIGITS (the decimal that node prints) in the word_text form.
   LINE is the node line, NULL when node gave no more lines. */
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
                                  : "bad: node gives no result line");
}

/* Prints the result line of one call. LINE is its node line, NULL when node
   gave no more lines or the call did not go to node. */
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
      printf("%s\n", digits != NULL ? digits : line != NULL ? line : "bad: node gives no result line");
      return;
    case CALL_WIDE:
      print_wide(digits, line);
      return;
  }
}

/* Quote the driver path for the shell used by popen, including any quote
   supplied through TMPDIR. */
static int node_command(char *cmd, size_t cap, const char *path) {
  size_t at;
  int n = snprintf(cmd, cap, "node --max-old-space-size=2048 --input-type=module <'");
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

int main(int argc, char **argv) {
  static Abi abi;
  static Call call[MAX_CALLS];
  static char line[4096];
  static char cwd[4096];
  static char path[8192];
  static char tmpl[4096];
  static char cmd[4u * sizeof tmpl + 128u];
  const char *dir = getenv("TMPDIR");
  size_t calls = 0;
  size_t k;
  int more;
  int fd;
  int status;
  FILE *f;
  FILE *node;
  if (argc != 3) return fail2("usage", "jstool MODULE.mjs ABI.txt (calls on stdin)");
  if (!read_abi(&abi, argv[2])) return fail2(argv[2], "not a langc abi --text file");
  f = fopen(argv[1], "r");
  if (f == NULL) return fail2(argv[1], "cannot open");
  fclose(f);
  if (argv[1][0] == '/' ? snprintf(path, sizeof path, "%s", argv[1]) >= (int)sizeof path
                        : getcwd(cwd, sizeof cwd) == NULL || snprintf(path, sizeof path, "%s/%s", cwd, argv[1]) >= (int)sizeof path)
    return fail2(argv[1], "the path is too long");
  if (snprintf(tmpl, sizeof tmpl, "%s/jstool-XXXXXX", dir != NULL && dir[0] != '\0' ? dir : "/tmp") >= (int)sizeof tmpl)
    return fail2("TMPDIR", "the path is too long");
  fd = mkstemp(tmpl);
  if (fd < 0) return fail2(tmpl, "cannot make the driver file");
  f = fdopen(fd, "w");
  if (f == NULL) {
    close(fd);
    unlink(tmpl);
    return fail2(tmpl, "cannot write the driver file");
  }
  put_head(f, path);
  more = fgets(line, sizeof line, stdin) != NULL;
  for (; more && calls < MAX_CALLS; more = fgets(line, sizeof line, stdin) != NULL) call[calls++] = read_call(&abi, line, f);
  fputs("process.stdout.write(out.map((l) => l + '\\n').join(''));\n", f);
  if (fclose(f) != 0 || more) {
    unlink(tmpl);
    return fail2(tmpl, more ? "more than 65536 calls" : "cannot write the driver file");
  }
  if (!node_command(cmd, sizeof cmd, tmpl)) {
    unlink(tmpl);
    return fail2(tmpl, "the node command is too long");
  }
  node = popen(cmd, "r");
  if (node == NULL) {
    unlink(tmpl);
    return fail2("node", "does not start");
  }
  for (k = 0; k < calls; k++) {
    int to_node = call[k].kind == CALL_WORD || call[k].kind == CALL_WIDE;
    int got = to_node && fgets(line, sizeof line, node) != NULL;
    line[got ? strcspn(line, "\n") : 0] = '\0';
    print_result(&call[k], got ? line : NULL);
  }
  status = pclose(node);
  unlink(tmpl);
  return status == 0 ? 0 : fail2("node", "exits with an error");
}
