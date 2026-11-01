#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

/* The token tool of docs/TOKEN-BASELINE.md (S15, A6; Q-S15-6 (a), Q-S15-7
   (a), Q-S15-9 (a), Q-S15-10 (c)). Run it from the repo root. `make tokens`
   runs the first 3 modes and `make check` runs the last 2:
   - `build/tokcount --model FILE`: FILE is the reply of GET /v1/models. It
     prints the id and the `created_at` of the model with the newest
     `created_at`.
   - `build/tokcount --request MODEL [FILE]`: it prints the count_tokens
     request body with 1 user message: the text of FILE, or the 1-byte
     frame message `x` when there is no FILE.
   - `build/tokcount --table DIR`: DIR has the replies, NAME.json for each
     counted file (NAME is the file name without its directory) and
     frame.json for the frame message. It prints the counts table (tokens,
     net tokens = tokens minus the frame tokens, SHA-256), the frame line
     and the ratio lines.
   - `build/tokcount --check DOC`: for each counted file, DOC must have 1
     table row `| FILE | ... | HASH |`, and HASH must be the SHA-256 of the
     file now. It prints a FAIL line for each mismatch, then `token report
     hashes: M of 15`.
   - `build/tokcount --estimate`: it prints the estimate block of the
     report (Q-S15-10 (c)): for each counted file the bytes, the estimate
     ceil(bytes / 4) and the SHA-256, the estimate line and the ratio lines
     over the estimates. An estimate is not a tokenizer count.
   The first 2 modes print their FAIL lines on stderr, because their stdout
   is data. Each mode exits 0 on success, 1 after a FAIL line and 2 on a
   usage or write error. */

#define FILE_COUNT 15
#define PROGRAM_COUNT 5
#define LANGUAGE_COUNT 3
#define FRAME_TEXT "x"
#define FRAME_NAME "frame"
#define SPACE " \t\r\n"
#define MODEL_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"

/* The counted files, LANGUAGE_COUNT for each G in the order cairn-lang,
   Python, Rust. */
static const char *const counted[FILE_COUNT] = {
  "examples/skill/g1-record.lang", "test/baseline/g1-record.py", "test/baseline/rust/src/bin/g1.rs",
  "examples/skill/g2-committee.lang", "test/baseline/g2-committee.py", "test/baseline/rust/src/bin/g2.rs",
  "examples/skill/g3-vote.lang", "test/baseline/g3-vote.py", "test/baseline/rust/src/bin/g3.rs",
  "examples/skill/g4-amend.lang", "test/baseline/g4-amend.py", "test/baseline/rust/src/bin/g4.rs",
  "examples/skill/g5-self-amend.lang", "test/baseline/g5-self-amend.py", "test/baseline/rust/src/bin/g5.rs",
};

static const char *const language_name[LANGUAGE_COUNT] = {"cairn-lang", "Python", "Rust"};

/* Reads the file PATH into *DATA (with a NUL after the last byte) and *SIZE.
   Returns 0 when the file cannot be read or when there is no memory. */
static int read_all(const char *path, unsigned char **data, size_t *size) {
  FILE *file = fopen(path, "rb");
  unsigned char *buf = NULL;
  size_t len = 0;
  size_t cap = 0;
  int ok = file != NULL;
  while (ok && !feof(file)) {
    if (len == cap) {
      size_t grown_cap = cap == 0 ? 4096 : 2 * cap;
      unsigned char *grown = realloc(buf, grown_cap + 1);
      ok = grown != NULL;
      if (ok) {
        buf = grown;
        cap = grown_cap;
      }
    }
    if (ok) {
      len += fread(buf + len, 1, cap - len, file);
      ok = !ferror(file);
    }
  }
  if (file != NULL) fclose(file);
  if (!ok) {
    free(buf);
    return 0;
  }
  buf[len] = '\0';
  *data = buf;
  *size = len;
  return 1;
}

/* Writes the SHA-256 of the file PATH to HEX as 64 lowercase hex digits.
   Returns 0 when the file cannot be read. */
static int file_hash(const char *path, char hex[65]) {
  unsigned char *data = NULL;
  size_t size = 0;
  if (!read_all(path, &data, &size)) return 0;
  unsigned char digest[32];
  sha256(data, size, digest);
  free(data);
  for (size_t i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return 1;
}

/* Returns the start of the value of the JSON key KEY (with its quotes) at
   or after AT, or NULL. */
static const char *value_after(const char *at, const char *key) {
  const char *found = strstr(at, key);
  if (found == NULL) return NULL;
  const char *value = found + strlen(key);
  value += strspn(value, SPACE);
  if (*value != ':') return NULL;
  value++;
  return value + strspn(value, SPACE);
}

/* Returns the string value of the key KEY at or after AT, with its length
   in *LEN, or NULL. The replies have no escapes in these values. */
static const char *string_after(const char *at, const char *key, size_t *len) {
  const char *value = value_after(at, key);
  if (value == NULL || *value != '"') return NULL;
  value++;
  *len = strcspn(value, "\"");
  return value[*len] == '"' ? value : NULL;
}

/* Compares A (AL bytes) with B (BL bytes) as strings. */
static int compare(const char *a, size_t al, const char *b, size_t bl) {
  int order = memcmp(a, b, al < bl ? al : bl);
  return order != 0 ? order : (al > bl) - (al < bl);
}

/* Returns 0 after the flush of stdout, or 2 on a write error. */
static int flush_status(void) {
  return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 2;
}

/* The name of PATH without its directory. */
static const char *file_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash == NULL ? path : slash + 1;
}

/* The models are the pairs of the Nth "id" and the Nth "created_at" of the
   list. The `created_at` values are RFC 3339 times in one form, thus their
   byte order is their time order. */
static int model_mode(const char *path) {
  unsigned char *data = NULL;
  size_t size = 0;
  if (!read_all(path, &data, &size)) {
    fprintf(stderr, "FAIL: tokens: cannot read %s\n", path);
    return 1;
  }
  const char *text = (const char *)data;
  const char *more = value_after(text, "\"has_more\"");
  size_t id_len = 0;
  size_t stamp_len = 0;
  const char *id = string_after(text, "\"id\"", &id_len);
  const char *stamp = string_after(text, "\"created_at\"", &stamp_len);
  const char *best = NULL;
  size_t best_len = 0;
  const char *best_stamp = NULL;
  size_t best_stamp_len = 0;
  while (id != NULL && stamp != NULL) {
    if (best == NULL || compare(stamp, stamp_len, best_stamp, best_stamp_len) > 0) {
      best = id;
      best_len = id_len;
      best_stamp = stamp;
      best_stamp_len = stamp_len;
    }
    id = string_after(id + id_len, "\"id\"", &id_len);
    stamp = string_after(stamp + stamp_len, "\"created_at\"", &stamp_len);
  }
  int ok = id == NULL && stamp == NULL && best != NULL && (more == NULL || strncmp(more, "false", 5) == 0);
  if (ok) printf("%.*s %.*s\n", (int)best_len, best, (int)best_stamp_len, best_stamp);
  else fprintf(stderr, "FAIL: tokens: %s is not 1 full page of models, each with an id and a created_at\n", path);
  free(data);
  int status = flush_status();
  return ok ? status : 1;
}

/* Prints the byte C in a JSON string. The bytes from 0x80 stay as they are
   (the files are UTF-8). */
static void put_json_byte(unsigned char c) {
  switch (c) {
  case '"':
    fputs("\\\"", stdout);
    break;
  case '\\':
    fputs("\\\\", stdout);
    break;
  case '\n':
    fputs("\\n", stdout);
    break;
  case '\r':
    fputs("\\r", stdout);
    break;
  case '\t':
    fputs("\\t", stdout);
    break;
  default:
    if (c < 0x20) printf("\\u%04x", (unsigned)c);
    else putchar(c);
  }
}

static int request_mode(const char *model, const char *path) {
  if (model[0] == '\0' || model[strspn(model, MODEL_CHARS)] != '\0') {
    fprintf(stderr, "FAIL: tokens: the model id '%s' is empty or has a byte outside [A-Za-z0-9._-]\n", model);
    return 1;
  }
  unsigned char *data = NULL;
  size_t size = 0;
  if (path != NULL && !read_all(path, &data, &size)) {
    fprintf(stderr, "FAIL: tokens: cannot read %s\n", path);
    return 1;
  }
  const unsigned char *text = path != NULL ? data : (const unsigned char *)FRAME_TEXT;
  size_t len = path != NULL ? size : strlen(FRAME_TEXT);
  printf("{\"model\": \"%s\", \"messages\": [{\"role\": \"user\", \"content\": \"", model);
  for (size_t i = 0; i < len; i++) put_json_byte(text[i]);
  printf("\"}]}\n");
  free(data);
  return flush_status();
}

/* Reads N of the reply DIR/NAME.json (`{"input_tokens": N}`) into *TOKENS.
   Prints a FAIL line and returns 0 when the reply has no count. */
static int reply_tokens(const char *dir, const char *name, unsigned long *tokens) {
  char path[1024];
  int n = snprintf(path, sizeof path, "%s/%s.json", dir, name);
  unsigned char *data = NULL;
  size_t size = 0;
  *tokens = 0;
  if (n < 0 || (size_t)n >= sizeof path || !read_all(path, &data, &size)) {
    printf("FAIL: tokens: cannot read the reply %s/%s.json\n", dir, name);
    return 0;
  }
  const char *value = value_after((const char *)data, "\"input_tokens\"");
  char *end = NULL;
  if (value != NULL && *value >= '0' && *value <= '9') *tokens = strtoul(value, &end, 10);
  int ok = end != NULL && end != value;
  free(data);
  if (!ok) printf("FAIL: tokens: the reply %s/%s.json has no input_tokens count\n", dir, name);
  return ok;
}

/* Prints A over B, half up, with 2 decimals. B is not 0. */
static void put_ratio(unsigned long a, unsigned long b) {
  unsigned long hundredths = (200 * a + b) / (2 * b);
  printf("%lu.%02lu\n", hundredths / 100, hundredths % 100);
}

/* Prints the ratio lines over V, 1 value for each counted file: each
   Python and each Rust value over the cairn-lang value of the same G, then
   the sum of each language over the sum of the cairn-lang values. WHAT
   names the values. Each cairn-lang value is not 0. */
static void put_ratios(const char *what, const unsigned long *v) {
  for (size_t g = 0; g < PROGRAM_COUNT; g++) {
    for (size_t k = 1; k < LANGUAGE_COUNT; k++) {
      printf("ratio of the %s (%s over %s): ", what, counted[LANGUAGE_COUNT * g + k], counted[LANGUAGE_COUNT * g]);
      put_ratio(v[LANGUAGE_COUNT * g + k], v[LANGUAGE_COUNT * g]);
    }
  }
  unsigned long total[LANGUAGE_COUNT] = {0, 0, 0};
  for (size_t i = 0; i < FILE_COUNT; i++) total[i % LANGUAGE_COUNT] += v[i];
  for (size_t k = 1; k < LANGUAGE_COUNT; k++) {
    printf("ratio of the %s (the %d %s files over the %d %s files): ", what, PROGRAM_COUNT, language_name[k], PROGRAM_COUNT, language_name[0]);
    put_ratio(total[k], total[0]);
  }
}

static int table_mode(const char *dir) {
  unsigned long frame = 0;
  int fails = reply_tokens(dir, FRAME_NAME, &frame) ? 0 : 1;
  unsigned long tokens[FILE_COUNT];
  unsigned long net[FILE_COUNT];
  char hex[FILE_COUNT][65];
  for (size_t i = 0; i < FILE_COUNT; i++) {
    int ok = reply_tokens(dir, file_name(counted[i]), &tokens[i]);
    if (ok && tokens[i] <= frame) {
      printf("FAIL: tokens: %s has %lu tokens, not more than the frame (%lu)\n", counted[i], tokens[i], frame);
      ok = 0;
    }
    if (ok && !file_hash(counted[i], hex[i])) {
      printf("FAIL: tokens: cannot read %s\n", counted[i]);
      ok = 0;
    }
    net[i] = ok ? tokens[i] - frame : 0;
    fails += ok ? 0 : 1;
  }
  if (fails > 0) return 1;
  printf("| file | tokens | net | sha256 |\n");
  printf("| :--- | ---: | ---: | :--- |\n");
  for (size_t i = 0; i < FILE_COUNT; i++) printf("| %s | %lu | %lu | %s |\n", counted[i], tokens[i], net[i], hex[i]);
  printf("\nframe: the 1-byte message `%s` is %lu tokens; net = tokens - %lu.\n\n", FRAME_TEXT, frame, frame);
  put_ratios("net tokens", net);
  return flush_status();
}

/* The FAIL text of a counted file, or NULL when its row is right. */
static const char *row_fault(int readable, size_t rows, size_t equal) {
  if (!readable) return "cannot read the file";
  if (rows != 1) return "the report does not have exactly 1 table row of the file";
  return equal == 1 ? NULL : "the hash in the report is not the SHA-256 of the file now";
}

static int check_mode(const char *doc) {
  unsigned char *data = NULL;
  size_t size = 0;
  if (!read_all(doc, &data, &size)) {
    printf("FAIL: tokens: cannot read %s\n", doc);
    printf("token report hashes: 0 of %d\n", FILE_COUNT);
    return 1;
  }
  char head[FILE_COUNT][128];
  char tail[FILE_COUNT][80];
  int readable[FILE_COUNT];
  size_t rows[FILE_COUNT] = {0};
  size_t equal[FILE_COUNT] = {0};
  for (size_t i = 0; i < FILE_COUNT; i++) {
    char hex[65];
    readable[i] = file_hash(counted[i], hex);
    snprintf(head[i], sizeof head[i], "| %s |", counted[i]);
    if (readable[i]) snprintf(tail[i], sizeof tail[i], "| %s |", hex);
    else tail[i][0] = '\0';
  }
  const char *line = (const char *)data;
  const char *end = line + size;
  while (line < end) {
    size_t len = strcspn(line, "\n");
    for (size_t i = 0; i < FILE_COUNT; i++) {
      size_t head_len = strlen(head[i]);
      size_t tail_len = strlen(tail[i]);
      if (len >= head_len && strncmp(line, head[i], head_len) == 0) {
        rows[i]++;
        if (readable[i] && len >= head_len + tail_len && strncmp(line + len - tail_len, tail[i], tail_len) == 0) equal[i]++;
      }
    }
    line += len + 1;
  }
  size_t matched = 0;
  for (size_t i = 0; i < FILE_COUNT; i++) {
    const char *fault = row_fault(readable[i], rows[i], equal[i]);
    if (fault != NULL) printf("FAIL: tokens: %s: %s\n", counted[i], fault);
    matched += fault == NULL ? 1 : 0;
  }
  printf("token report hashes: %zu of %d\n", matched, FILE_COUNT);
  free(data);
  int status = flush_status();
  return matched == FILE_COUNT ? status : 1;
}

/* Prints the estimate block of docs/TOKEN-BASELINE.md (Q-S15-10 (c)): the
   bytes, the estimate ceil(bytes / 4) and the SHA-256 of each counted file,
   the estimate line and the ratio lines over the estimates. */
static int estimate_mode(void) {
  unsigned long bytes[FILE_COUNT];
  unsigned long estimate[FILE_COUNT];
  char hex[FILE_COUNT][65];
  int fails = 0;
  for (size_t i = 0; i < FILE_COUNT; i++) {
    unsigned char *data = NULL;
    size_t size = 0;
    int readable = read_all(counted[i], &data, &size) && file_hash(counted[i], hex[i]);
    free(data);
    if (!readable) printf("FAIL: tokens: cannot read %s\n", counted[i]);
    if (readable && size == 0) printf("FAIL: tokens: %s is empty\n", counted[i]);
    int ok = readable && size > 0;
    bytes[i] = ok ? size : 0;
    estimate[i] = (bytes[i] + 3) / 4;
    fails += ok ? 0 : 1;
  }
  if (fails > 0) return 1;
  printf("| file | bytes | estimate | sha256 |\n");
  printf("| :--- | ---: | ---: | :--- |\n");
  for (size_t i = 0; i < FILE_COUNT; i++) printf("| %s | %lu | %lu | %s |\n", counted[i], bytes[i], estimate[i], hex[i]);
  printf("\nestimate: ceil(bytes / 4) of each file; this is not a tokenizer count.\n\n");
  put_ratios("estimates", estimate);
  return flush_status();
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--model") == 0) return model_mode(argv[2]);
  if ((argc == 3 || argc == 4) && strcmp(argv[1], "--request") == 0) return request_mode(argv[2], argc == 4 ? argv[3] : NULL);
  if (argc == 3 && strcmp(argv[1], "--table") == 0) return table_mode(argv[2]);
  if (argc == 3 && strcmp(argv[1], "--check") == 0) return check_mode(argv[2]);
  if (argc == 2 && strcmp(argv[1], "--estimate") == 0) return estimate_mode();
  printf("FAIL: tokens: usage: build/tokcount --model FILE | --request MODEL [FILE] | --table DIR | --check DOC | --estimate\n");
  return 2;
}
