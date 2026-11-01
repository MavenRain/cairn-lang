#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* The test of skills/cairn-lang/SKILL.md (S14, Q-S14-1 (a), Q-S14-5 (a)).
   Run it from the repo root as `build/skilltool [PATH N]`; PATH is
   skills/cairn-lang/SKILL.md and N is 5 by default. Each example starts at a
   line `### Gn`. It compares the program block (the line `File: FILE`, then
   the fence ```lang) with the file FILE byte for byte. For each row (a line
   `CMD` gives:, a text block and the sentence `The exit code is N.`), it runs
   `CMD 2>&1`, compares the output with the text block line for line, and
   compares the exit code with the sentence. It prints a FAIL line for each
   mismatch, then `skill quotes: Q of N` and `skill outputs: M of R`, and
   exits 0 only when there is no FAIL line. */

#define SKILL_PATH "skills/cairn-lang/SKILL.md"
#define EXAMPLE_COUNT 5
#define HEADING "### G"
#define FILE_PREFIX "File: "
#define PROGRAM_FENCE "```lang"
#define TEXT_FENCE "```text"
#define END_FENCE "```"
#define COMMAND_PREFIX "`build/langc "
#define COMMAND_SUFFIX "` gives:"
#define EXIT_PREFIX "The exit code is "

/* The characters of a command that the test puts on a command line. */
#define COMMAND_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._- "

/* The lines of a file or of the stdout of a command, without their
   newlines. */
typedef struct {
  char **lines;
  size_t count;
} Lines;

static void free_lines(Lines *lines) {
  for (size_t i = 0; i < lines->count; i++) free(lines->lines[i]);
  free(lines->lines);
  lines->lines = NULL;
  lines->count = 0;
}

/* Reads the lines of FILE into LINES. Returns 0 on a read error or when
   there is no memory. */
static int read_stream(FILE *file, Lines *lines) {
  char *line = NULL;
  size_t size = 0;
  ssize_t len;
  int ok = 1;
  while (ok && (len = getline(&line, &size, file)) >= 0) {
    char **grown = realloc(lines->lines, (lines->count + 1) * sizeof *grown);
    ok = grown != NULL;
    if (ok) {
      lines->lines = grown;
      if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
      lines->lines[lines->count++] = line;
      line = NULL;
      size = 0;
    }
  }
  free(line);
  return ok && !ferror(file);
}

/* Reads the lines of the file PATH into LINES. Returns 0 when the file
   cannot be read. */
static int read_file(const char *path, Lines *lines) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return 0;
  int ok = read_stream(file, lines);
  return fclose(file) == 0 && ok;
}

/* Runs COMMAND and reads its stdout into LINES. Returns the exit code, or -1
   when the command cannot run or its stdout cannot be read. */
static int run_command(const char *command, Lines *lines) {
  FILE *pipe = popen(command, "r");
  if (pipe == NULL) return -1;
  int ok = read_stream(pipe, lines);
  int status = pclose(pipe);
  return ok && status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Returns the index of the first line in FROM to TO - 1 that starts with
   PREFIX, or a value of TO or more when there is none. */
static size_t find_line(const Lines *lines, size_t from, size_t to, const char *prefix) {
  size_t i = from;
  while (i < to && strncmp(lines->lines[i], prefix, strlen(prefix)) != 0) i++;
  return i;
}

/* Returns the index of the first heading (`## ` or `### `) after line AT, or
   the line count when there is none. */
static size_t section_end(const Lines *lines, size_t at) {
  size_t a = find_line(lines, at + 1, lines->count, "## ");
  size_t b = find_line(lines, at + 1, lines->count, "### ");
  return a < b ? a : b;
}

/* Returns 1 when LINE is the heading `### Gn` of example N. */
static int is_heading(const char *line, size_t n) {
  char want[32];
  return snprintf(want, sizeof want, HEADING "%zu", n) < (int)sizeof want && strcmp(line, want) == 0;
}

/* Returns 1 when the bytes of the file PATH are the lines FROM to TO - 1 of
   LINES, each with its newline. */
static int same_as_block(const char *path, const Lines *lines, size_t from, size_t to) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return 0;
  int same = 1;
  for (size_t i = from; same && i < to; i++) {
    for (const char *c = lines->lines[i]; same && *c != '\0'; c++) same = getc(file) == (unsigned char)*c;
    same = same && getc(file) == '\n';
  }
  same = same && getc(file) == EOF;
  return fclose(file) == 0 && same;
}

/* Checks the row of example N whose command line is line ROW of SKILL; the
   row ends before line TO. Sets *MATCH to 1 when the output and the exit
   code match. Returns the number of FAIL lines. */
static int check_row(const Lines *skill, size_t row, size_t to, size_t n, int *match) {
  const char *line = skill->lines[row];
  size_t len = strlen(line);
  size_t suffix = strlen(COMMAND_SUFFIX);
  int shaped = len > strlen(COMMAND_PREFIX) + suffix && strcmp(line + len - suffix, COMMAND_SUFFIX) == 0;
  size_t cmd_len = shaped ? len - suffix - 1 : 0;
  size_t text = find_line(skill, row + 1, to, TEXT_FENCE);
  size_t text_to = find_line(skill, text + 1, to, END_FENCE);
  size_t exit_at = find_line(skill, text_to + 1, to, EXIT_PREFIX);
  *match = 0;
  if (strspn(line + 1, COMMAND_CHARS) != cmd_len || exit_at >= to) {
    printf("FAIL: skill: G%zu: the row at line %zu has no command, text block or exit sentence\n", n, row + 1);
    return 1;
  }
  int fails = 0;
  char command[512];
  Lines out = {NULL, 0};
  int code = snprintf(command, sizeof command, "%.*s 2>&1", (int)cmd_len, line + 1) < (int)sizeof command ? run_command(command, &out) : -1;
  int same = out.count == text_to - text - 1;
  for (size_t i = 0; same && i < out.count; i++) same = strcmp(out.lines[i], skill->lines[text + 1 + i]) == 0;
  if (!same) {
    printf("FAIL: skill: G%zu: the output of `%.*s` is not the text block\n", n, (int)cmd_len, line + 1);
    fails++;
  }
  const char *text_code = skill->lines[exit_at] + strlen(EXIT_PREFIX);
  char *end = NULL;
  long want = strtol(text_code, &end, 10);
  if (end == text_code || strcmp(end, ".") != 0 || code != want) {
    printf("FAIL: skill: G%zu: `%.*s` exits %d, the sentence gives '%s'\n", n, (int)cmd_len, line + 1, code, text_code);
    fails++;
  }
  *match = fails == 0;
  free_lines(&out);
  return fails;
}

/* Checks example N, whose heading is line AT of SKILL; the example ends
   before line TO. Sets *QUOTED to 1 when the program block is the file, adds
   its rows to *ROWS and its matching rows to *MATCHES. Returns the number of
   FAIL lines. */
static int check_example(const Lines *skill, size_t at, size_t to, size_t n, int *quoted, size_t *rows, size_t *matches) {
  size_t file_at = find_line(skill, at + 1, to, FILE_PREFIX);
  size_t fence = find_line(skill, file_at + 1, to, PROGRAM_FENCE);
  size_t fence_to = find_line(skill, fence + 1, to, END_FENCE);
  *quoted = 0;
  if (fence_to >= to || !is_heading(skill->lines[at], n)) {
    printf("FAIL: skill: example %zu (%s) has no heading G%zu, File line or program block\n", n, skill->lines[at], n);
    return 1;
  }
  const char *path = skill->lines[file_at] + strlen(FILE_PREFIX);
  int fails = 0;
  *quoted = strcmp(skill->lines[fence], PROGRAM_FENCE) == 0 && same_as_block(path, skill, fence + 1, fence_to);
  if (!*quoted) {
    printf("FAIL: skill: G%zu: the program block is not %s\n", n, path);
    fails++;
  }
  size_t row = find_line(skill, fence_to + 1, to, COMMAND_PREFIX);
  while (row < to) {
    size_t next = find_line(skill, row + 1, to, COMMAND_PREFIX);
    int match = 0;
    fails += check_row(skill, row, next, n, &match);
    *rows += 1;
    *matches += (size_t)match;
    row = next;
  }
  return fails;
}

int main(int argc, char **argv) {
  const char *skill_path = argc == 3 ? argv[1] : SKILL_PATH;
  char *end = NULL;
  unsigned long want = argc == 3 ? strtoul(argv[2], &end, 10) : EXAMPLE_COUNT;
  if (argc != 1 && (argc != 3 || end == argv[2] || *end != '\0')) {
    printf("FAIL: skill: usage: build/skilltool [PATH N]\n");
    return 2;
  }
  Lines skill = {NULL, 0};
  int fails = read_file(skill_path, &skill) ? 0 : 1;
  if (fails > 0) printf("FAIL: skill: cannot read %s\n", skill_path);
  size_t examples = 0;
  size_t quotes = 0;
  size_t rows = 0;
  size_t matches = 0;
  size_t at = find_line(&skill, 0, skill.count, HEADING);
  while (at < skill.count) {
    size_t to = section_end(&skill, at);
    int quoted = 0;
    fails += check_example(&skill, at, to, ++examples, &quoted, &rows, &matches);
    quotes += (size_t)quoted;
    at = find_line(&skill, to, skill.count, HEADING);
  }
  if (examples != want) {
    printf("FAIL: skill: %s has %zu examples, not %lu\n", skill_path, examples, want);
    fails++;
  }
  printf("skill quotes: %zu of %lu\n", quotes, want);
  printf("skill outputs: %zu of %zu\n", matches, rows);
  free_lines(&skill);
  return fails == 0 ? 0 : 1;
}
