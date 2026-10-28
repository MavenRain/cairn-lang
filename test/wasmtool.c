/* The Wasm runner (cairn-lang S8, Q-S8-7): runs the entries of a langc module
   on the wasmtime C API and prints each result as `langc eval` does.

   Usage: wasmtool MODULE.wasm ABI.txt
   ABI.txt is the output of `langc abi PROG --text`. Each stdin line is
   `NAME [ARGS...]`; each output line is a decimal Nat or Flag, a decimal U256
   and `u`, `trap` when the module traps, or `bad: REASON`. A U256 argument is
   decimal or 0x hex, then `u`; it goes to the entry as 4 i64, high limb first.
   A U256 result is the 32 bytes at address 0..31, low limb first (M8): any
   other address is `bad`. Each call runs in a new store, so a trap does not
   reach the next call. Exit 0: each call printed a line. Exit 2: usage, IO or
   a module that does not load. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wasmtime.h>

#include "front/u256.h"

#define MAX_ENTRIES 256
#define MAX_PARAMS 32
#define MAX_NAME 128

typedef struct {
  char name[MAX_NAME];
  size_t param_count;
  int wide_param[MAX_PARAMS]; /* 1: a U256 (4 i64), 0: a Nat or Flag */
  int wide_result;            /* 1: a U256 at address 0 */
} Entry;

typedef struct {
  Entry entry[MAX_ENTRIES];
  size_t count;
} Abi;

static int fail2(const char *what, const char *detail) {
  fprintf(stderr, "wasmtool: %s: %s\n", what, detail);
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

/* The U256 at address ADDR of the first exported memory, low limb first. */
static int read_result(wasmtime_context_t *ctx, wasmtime_instance_t *inst, uint64_t addr, U256 *out) {
  wasmtime_extern_t item;
  char *name = NULL;
  size_t name_len = 0;
  size_t k;
  int found = 0;
  for (k = 0; !found && wasmtime_instance_export_nth(ctx, inst, k, &name, &name_len, &item); k++) {
    found = item.kind == WASMTIME_EXTERN_MEMORY;
  }
  if (!found || addr + 32u > wasmtime_memory_data_size(ctx, &item.of.memory)) return 0;
  for (k = 0; k < 4u; k++) {
    const uint8_t *b = wasmtime_memory_data(ctx, &item.of.memory) + addr + 8u * k;
    uint64_t w = 0;
    int j;
    for (j = 7; j >= 0; j--) w = (w << 8) | b[j];
    out->limb[k] = w;
  }
  return 1;
}

/* Runs one call line and prints its result line. */
static void run_call(wasm_engine_t *engine, wasmtime_module_t *module, const Abi *abi, char *line) {
  char *word[MAX_PARAMS + 2];
  size_t words = 0;
  char *tok;
  const Entry *e;
  wasmtime_val_t args[4 * MAX_PARAMS];
  size_t nargs = 0;
  size_t k;
  for (tok = strtok(line, " \n"); tok != NULL && words < MAX_PARAMS + 2; tok = strtok(NULL, " \n")) word[words++] = tok;
  e = words > 0 ? find_entry(abi, word[0]) : NULL;
  if (e == NULL || words != e->param_count + 1) {
    printf("bad: no entry or a wrong argument count\n");
    return;
  }
  for (k = 0; k < e->param_count; k++) {
    U256 x;
    uint64_t n = 0;
    int j;
    int ok = e->wide_param[k] ? parse_u256(word[k + 1], &x) : parse_word(word[k + 1], &n);
    if (!ok) {
      printf("bad: argument %zu\n", k + 1);
      return;
    }
    for (j = e->wide_param[k] ? 3 : 0; j >= 0; j--) {
      args[nargs].kind = WASMTIME_I64;
      args[nargs].of.i64 = (int64_t)(e->wide_param[k] ? x.limb[j] : n);
      nargs++;
    }
  }
  {
    wasmtime_store_t *store = wasmtime_store_new(engine, NULL, NULL);
    wasmtime_context_t *ctx = wasmtime_store_context(store);
    wasmtime_instance_t inst;
    wasmtime_extern_t fn;
    wasm_trap_t *trap = NULL;
    wasmtime_val_t result;
    wasmtime_error_t *error = wasmtime_instance_new(ctx, module, NULL, 0, &inst, &trap);
    int have = error == NULL && trap == NULL && wasmtime_instance_export_get(ctx, &inst, e->name, strlen(e->name), &fn) &&
               fn.kind == WASMTIME_EXTERN_FUNC;
    if (have) error = wasmtime_func_call(ctx, &fn.of.func, args, nargs, &result, 1, &trap);
    if (trap != NULL) {
      printf("trap\n");
      wasm_trap_delete(trap);
    } else if (!have || error != NULL) {
      printf("bad: the entry does not run\n");
    } else {
      U256 x;
      char text[96];
      uint64_t value = result.kind == WASMTIME_I32 ? (uint64_t)(uint32_t)result.of.i32 : (uint64_t)result.of.i64;
      int read = e->wide_result && value == 0 && read_result(ctx, &inst, value, &x);
      if (read) word_text(WORD_U256, &x, text, sizeof text);
      if (!e->wide_result) printf("%llu\n", (unsigned long long)value);
      if (e->wide_result) printf("%s\n", read ? text : "bad: a U256 result is not at address 0");
    }
    if (error != NULL) wasmtime_error_delete(error);
    wasmtime_store_delete(store);
  }
}

int main(int argc, char **argv) {
  static Abi abi;
  static uint8_t bytes[1u << 22];
  char line[4096];
  size_t len;
  FILE *f;
  wasm_engine_t *engine;
  wasmtime_module_t *module = NULL;
  wasmtime_error_t *error;
  if (argc != 3) return fail2("usage", "wasmtool MODULE.wasm ABI.txt (calls on stdin)");
  if (!read_abi(&abi, argv[2])) return fail2(argv[2], "not a langc abi --text file");
  f = fopen(argv[1], "rb");
  if (f == NULL) return fail2(argv[1], "cannot open");
  len = fread(bytes, 1, sizeof bytes, f);
  fclose(f);
  engine = wasm_engine_new();
  error = wasmtime_module_new(engine, bytes, len, &module);
  if (error != NULL) {
    wasmtime_error_delete(error);
    wasm_engine_delete(engine);
    return fail2(argv[1], "the module does not load");
  }
  while (fgets(line, sizeof line, stdin) != NULL) run_call(engine, module, &abi, line);
  wasmtime_module_delete(module);
  wasm_engine_delete(engine);
  return 0;
}
