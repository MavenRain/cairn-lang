/* C regression for jstool's module import and entry-name handling. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int check_calls(void) {
  static const char command[] =
      "printf \"default 7\\na' 7\\nthen 7\\n\" | "
      "build/jstool 'build/js-names #?%.mjs' build/js-names.abi";
  static const char *const expected[] = {"7\n", "8\n", "9\n"};
  char line[256];
  size_t i;
  int ok = 1;
  FILE *f = popen(command, "r");
  if (f == NULL) {
    fputs("FAIL js import: cannot start jstool\n", stderr);
    return 1;
  }
  for (i = 0; i < sizeof expected / sizeof expected[0]; i++) {
    int got = fgets(line, sizeof line, f) != NULL;
    if (!got || strcmp(line, expected[i]) != 0) {
      fprintf(stderr, "FAIL js import: row %zu: want %s, got %s", i + 1u,
              expected[i], got ? line : "no result\n");
      ok = 0;
    }
  }
  if (fgets(line, sizeof line, f) != NULL || ferror(f)) ok = 0;
  if (pclose(f) != 0) ok = 0;
  return ok ? 0 : 1;
}

int main(void) {
  char dir[128];
  int ok = check_calls() == 0;
  snprintf(dir, sizeof dir, "build/js-tmp-'%ld", (long)getpid());
  if (mkdir(dir, 0700) != 0) {
    fputs("FAIL js import: cannot make TMPDIR\n", stderr);
    return 1;
  }
  if (setenv("TMPDIR", dir, 1) != 0 || check_calls() != 0) ok = 0;
  if (rmdir(dir) != 0) ok = 0;
  if (ok) puts("js import regressions: passed");
  return ok ? 0 : 1;
}
