/* mcptool: the MCP rows of test/gate.sh (S13, Q-S13-8 (a)).
 *
 * `mcptool DIR/NAME.in ...` runs each row. `build/langc mcp` (`build/langc
 * --kit mcp` when NAME starts with kit-) reads NAME.in on stdin. Its stdout
 * must equal DIR/NAME.out byte for byte, its stderr must be empty and it must
 * exit 0. The method is the one of evolve-lang test/mcp.c: `system` runs the
 * server with the three streams redirected. mcptool prints a FAIL line for
 * each bad row, then `mcp rows: N passed`. It exits 0 when there is a row and
 * each row passes, else 1. Run it from the repo root.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define OUT_PATH "build/mcptool.out"
#define ERR_PATH "build/mcptool.err"

/* same_bytes returns 1 when the files at a and b open and hold the same
 * bytes. */
static int same_bytes(const char *a, const char *b) {
  FILE *fa = fopen(a, "rb");
  FILE *fb = fopen(b, "rb");
  int same = 0;
  if (fa != NULL && fb != NULL) {
    int ca = fgetc(fa);
    int cb = fgetc(fb);
    while (ca == cb && ca != EOF) {
      ca = fgetc(fa);
      cb = fgetc(fb);
    }
    same = ca == cb;
  }
  if (fa != NULL) {
    fclose(fa);
  }
  if (fb != NULL) {
    fclose(fb);
  }
  return same;
}

/* is_empty returns 1 when the file at path opens and holds no byte. */
static int is_empty(const char *path) {
  FILE *f = fopen(path, "rb");
  int empty = 0;
  if (f != NULL) {
    empty = fgetc(f) == EOF;
    fclose(f);
  }
  return empty;
}

/* run_row runs the row of the request file in and returns 1 when it passes. */
static int run_row(const char *in) {
  char want[512];
  char command[1024];
  size_t len = strlen(in);
  const char *slash = strrchr(in, '/');
  const char *base = slash == NULL ? in : slash + 1;
  const char *mode = strncmp(base, "kit-", 4) == 0 ? "--kit " : "";
  int n;
  int status;
  if (len < 4 || strcmp(in + len - 3, ".in") != 0 || len + 2 > sizeof want) {
    printf("FAIL mcp %s: the name does not end in .in, or it is too long\n", in);
    return 0;
  }
  memcpy(want, in, len - 3);
  memcpy(want + len - 3, ".out", 5);
  n = snprintf(command, sizeof command, "build/langc %smcp <%s >%s 2>%s", mode, in, OUT_PATH, ERR_PATH);
  if (n < 0 || (size_t)n >= sizeof command) {
    printf("FAIL mcp %s: the command is too long\n", in);
    return 0;
  }
  status = system(command);
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("FAIL mcp %s: langc mcp did not exit 0 (status %d)\n", in, status);
    return 0;
  }
  if (!same_bytes(want, OUT_PATH)) {
    printf("FAIL mcp %s: the stdout is not %s\n", in, want);
    return 0;
  }
  if (!is_empty(ERR_PATH)) {
    printf("FAIL mcp %s: the stderr is not empty\n", in);
    return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  int passed = 0;
  int i;
  for (i = 1; i < argc; i++) {
    passed += run_row(argv[i]);
  }
  printf("mcp rows: %d passed\n", passed);
  return argc > 1 && passed == argc - 1 ? 0 : 1;
}
