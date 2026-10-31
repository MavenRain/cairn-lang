/* The `langc check --json` writer (S12 A2). The escapes are those of
   quorum-lang src/front/json.c. */
#include "front/json.h"

#include <string.h>

static void put_char(unsigned char c, FILE *out) {
  const char *escape = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\t' ? "\\t" : NULL;
  if (escape != NULL) {
    fputs(escape, out);
    return;
  }
  if (c < 0x20) {
    fprintf(out, "\\u%04x", c);
    return;
  }
  fputc(c, out);
}

void json_string_n(const char *text, size_t len, FILE *out) {
  fputc('"', out);
  for (size_t pos = 0; pos < len;) {
    size_t width = utf8_char_size(text + pos, len - pos);
    /* Paths and truncated diagnostic buffers can contain invalid bytes. */
    if (width == 0) {
      fputs("\\ufffd", out);
      pos++;
      continue;
    }
    for (size_t i = 0; i < width; i++) put_char((unsigned char)text[pos + i], out);
    pos += width;
  }
  fputc('"', out);
}

void json_string(const char *text, FILE *out) {
  json_string_n(text, strlen(text), out);
}

/* A span with line 0 has no source position: null. */
static void put_span(const char *path, Span span, FILE *out) {
  if (span.line == 0) {
    fputs("null", out);
    return;
  }
  fputs("{\"file\":", out);
  json_string(path, out);
  fprintf(out, ",\"line\":%d,\"col\":%d,\"end_line\":%d,\"end_col\":%d}", span.line, span.col, span.end_line, span.end_col);
}

/* The fix of DIAG: null, or {"span":{..},"text":".."} (S12). */
static void put_fix(const char *path, const Diag *diag, FILE *out) {
  if (!diag->fix_set) {
    fputs("null", out);
    return;
  }
  fputs("{\"span\":", out);
  put_span(path, diag->fix_span, out);
  fputs(",\"text\":", out);
  json_string(diag->fix, out);
  fputc('}', out);
}

void json_check(FILE *out, const char *path, const Diag *diag, const CheckProgress *progress) {
  fputs("{\"diagnostics\":[", out);
  if (diag->set) {
    /* The message is the text after `CODE: ` that diag_print writes. */
    char message[sizeof diag->def + sizeof diag->msg + 2];
    snprintf(message, sizeof message, "%s: %s", diag->def, diag->msg);
    fputs("{\"code\":", out);
    json_string(diag->code, out);
    fputs(",\"span\":", out);
    put_span(path, diag->span, out);
    fputs(",\"message\":", out);
    json_string(message, out);
    fputs(",\"fix\":", out);
    put_fix(path, diag, out);
    fputc('}', out);
  }
  fprintf(out, "],\"progress\":{\"parsed\":%d,\"typed\":%d,\"laws_discharged\":%d,\"laws_total\":%d}}\n",
          progress->parsed, progress->typed, progress->laws_discharged, progress->laws_total);
}
