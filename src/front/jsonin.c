#include "front/jsonin.h"

#include <string.h>

/* The state of one json_parse call. */
typedef struct {
  Arena *arena;
  const char *text;
  size_t len;
  size_t pos;
  int depth; /* the open arrays and objects */
  Diag *diag;
} Reader;

/* The byte at POS, or -1 after the end. */
static int peek_at(const Reader *r, size_t pos) {
  return pos < r->len ? (unsigned char)r->text[pos] : -1;
}

static int peek(const Reader *r) {
  return peek_at(r, r->pos);
}

static int fail(Reader *r, const char *what) {
  return diag_fail(r->diag, "USAGE", NULL, "JSON: %s at byte %zu", what, r->pos);
}

static int no_memory(Reader *r) {
  return diag_fail(r->diag, "OOM", NULL, "no memory for a JSON value");
}

static void skip_space(Reader *r) {
  while (peek(r) == ' ' || peek(r) == '\t' || peek(r) == '\n' || peek(r) == '\r') r->pos++;
}

static int is_digit(int c) {
  return c >= '0' && c <= '9';
}

static size_t skip_digits(Reader *r) {
  size_t start = r->pos;
  while (is_digit(peek(r))) r->pos++;
  return r->pos - start;
}

/* true, false or null. */
static int read_word(Reader *r, const char *word, JsonKind kind, JsonValue *out) {
  size_t n = strlen(word);
  if (r->len - r->pos < n || memcmp(r->text + r->pos, word, n) != 0) return fail(r, "expected a value");
  r->pos += n;
  out->kind = kind;
  return 1;
}

/* -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)? as RFC 8259 says. TEXT
   keeps the source text. */
static int read_number(Reader *r, JsonValue *out) {
  size_t start = r->pos;
  r->pos += peek(r) == '-';
  int zero = peek(r) == '0';
  r->pos += zero;
  if (!zero && skip_digits(r) == 0) return fail(r, "expected a digit");
  int fraction = peek(r) == '.';
  r->pos += fraction;
  if (fraction && skip_digits(r) == 0) return fail(r, "expected a digit");
  int exponent = peek(r) == 'e' || peek(r) == 'E';
  r->pos += exponent;
  r->pos += exponent && (peek(r) == '+' || peek(r) == '-');
  if (exponent && skip_digits(r) == 0) return fail(r, "expected a digit");
  char *text = arena_strndup(r->arena, r->text + start, r->pos - start);
  if (text == NULL) return no_memory(r);
  out->kind = JSON_NUMBER;
  out->text = text;
  out->len = r->pos - start;
  return 1;
}

static int hex_digit(int c) {
  return is_digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The value of the 4 hex digits at POS, or -1. */
static long hex4(const Reader *r, size_t pos) {
  long value = 0;
  for (size_t i = 0; i < 4; i++) {
    int digit = hex_digit(peek_at(r, pos + i));
    if (digit < 0) return -1;
    value = value * 16 + digit;
  }
  return value;
}

/* Writes the scalar CP as UTF-8 at DST. Returns the byte count. */
static size_t put_utf8(unsigned char *dst, unsigned long cp) {
  static const unsigned char LEAD[] = {0x00, 0x00, 0xC0, 0xE0, 0xF0};
  size_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
  for (size_t i = n; i > 1; i--) {
    dst[i - 1] = (unsigned char)(0x80 | (cp & 0x3F));
    cp >>= 6;
  }
  dst[0] = (unsigned char)(LEAD[n] | cp);
  return n;
}

/* Decodes the escape at POS into BUF at *N. The \u escape of a high
   surrogate takes the \u escape of the low surrogate after it. */
static int read_escape(Reader *r, unsigned char *buf, size_t *n) {
  static const char FROM[] = "\"\\/bfnrt";
  static const char TO[] = "\"\\/\b\f\n\r\t";
  int c = peek_at(r, r->pos + 1);
  const char *hit = c > 0 ? strchr(FROM, c) : NULL;
  if (hit != NULL) {
    buf[*n] = (unsigned char)TO[hit - FROM];
    *n += 1;
    r->pos += 2;
    return 1;
  }
  long cp = c == 'u' ? hex4(r, r->pos + 2) : -2;
  if (cp == -2) return fail(r, "an unknown escape");
  if (cp < 0) return fail(r, "expected 4 hex digits after \\u");
  int high = cp >= 0xD800 && cp <= 0xDBFF;
  long low = high && peek_at(r, r->pos + 6) == '\\' && peek_at(r, r->pos + 7) == 'u' ? hex4(r, r->pos + 8) : -1;
  if (cp >= 0xDC00 && cp <= 0xDFFF) return fail(r, "a low surrogate without a high surrogate");
  if (high && (low < 0xDC00 || low > 0xDFFF)) return fail(r, "a high surrogate without a low surrogate");
  cp = high ? 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00) : cp;
  r->pos += high ? 12 : 6;
  *n += put_utf8(buf + *n, (unsigned long)cp);
  return 1;
}

/* Sets END to the position of the closing quote of the string at POS. */
static int string_end(const Reader *r, size_t *end) {
  size_t pos = r->pos + 1;
  int c = peek_at(r, pos);
  while (c >= 0 && c != '"') {
    pos += c == '\\' ? 2 : 1;
    c = peek_at(r, pos);
  }
  *end = pos;
  return c == '"';
}

/* Reads the string at POS into TEXT, NUL-terminated, and its byte count
   into LEN. The decoded bytes are not more than the source bytes. */
static int read_string(Reader *r, const char **text, size_t *len) {
  size_t end;
  if (!string_end(r, &end)) return fail(r, "the string has no closing quote");
  unsigned char *buf = arena_alloc(r->arena, end - r->pos);
  if (buf == NULL) return no_memory(r);
  size_t n = 0;
  r->pos++;
  while (r->pos < end) {
    int c = peek(r);
    if (c == '\\') {
      if (!read_escape(r, buf, &n)) return 0;
      continue;
    }
    if (c < 0x20) return fail(r, "a control byte in a string");
    size_t width = utf8_char_size(r->text + r->pos, end - r->pos);
    if (width == 0) return fail(r, "invalid UTF-8 in a string");
    memcpy(buf + n, r->text + r->pos, width);
    n += width;
    r->pos += width;
  }
  buf[n] = '\0';
  r->pos++;
  *text = (const char *)buf;
  *len = n;
  return 1;
}

static int read_value(Reader *r, JsonValue *out);

/* Reads the key of an object member and the colon after it. */
static int read_key(Reader *r, JsonValue *member) {
  if (peek(r) != '"') return fail(r, "expected a string key");
  if (!read_string(r, &member->key, &member->key_len)) return 0;
  skip_space(r);
  if (peek(r) != ':') return fail(r, "expected :");
  r->pos++;
  skip_space(r);
  return 1;
}

/* Reads the items of an array (CLOSE ']') or the members of an object
   (CLOSE '}') after the open bracket. */
static int read_list(Reader *r, int close, JsonValue *out) {
  const JsonValue **link = &out->first;
  r->pos++;
  skip_space(r);
  int c = peek(r);
  r->pos += c == close;
  while (c != close) {
    JsonValue *item = arena_alloc(r->arena, sizeof *item);
    if (item == NULL) return no_memory(r);
    memset(item, 0, sizeof *item);
    if (close == '}' && !read_key(r, item)) return 0;
    if (!read_value(r, item)) return 0;
    *link = item;
    link = &item->next;
    out->count++;
    skip_space(r);
    c = peek(r);
    if (c != close && c != ',') return fail(r, close == '}' ? "expected , or }" : "expected , or ]");
    r->pos++;
    skip_space(r);
  }
  return 1;
}

static int read_nested(Reader *r, JsonValue *out) {
  int object = peek(r) == '{';
  if (r->depth >= JSON_DEPTH_MAX) return fail(r, "the nesting is too deep");
  out->kind = object ? JSON_OBJECT : JSON_ARRAY;
  r->depth++;
  int ok = read_list(r, object ? '}' : ']', out);
  r->depth--;
  return ok;
}

static int read_value(Reader *r, JsonValue *out) {
  int c = peek(r);
  switch (c) {
    case '{':
    case '[':
      return read_nested(r, out);
    case '"':
      out->kind = JSON_STRING;
      return read_string(r, &out->text, &out->len);
    case 't':
      return read_word(r, "true", JSON_TRUE, out);
    case 'f':
      return read_word(r, "false", JSON_FALSE, out);
    case 'n':
      return read_word(r, "null", JSON_NULL, out);
    default:
      return c == '-' || is_digit(c) ? read_number(r, out) : fail(r, "expected a value");
  }
}

int json_parse(Arena *arena, const char *text, size_t len, JsonValue *out, Diag *diag) {
  Reader r = {arena, text, len, 0, 0, diag};
  memset(out, 0, sizeof *out);
  skip_space(&r);
  if (!read_value(&r, out)) return 0;
  skip_space(&r);
  return r.pos == len ? 1 : fail(&r, "expected the end of the text");
}

const JsonValue *json_member(const JsonValue *object, const char *key) {
  size_t n = strlen(key);
  const JsonValue *m = object != NULL && object->kind == JSON_OBJECT ? object->first : NULL;
  while (m != NULL && !(m->key_len == n && memcmp(m->key, key, n) == 0)) m = m->next;
  return m;
}

const char *json_get_string(const JsonValue *value) {
  return value != NULL && value->kind == JSON_STRING && strlen(value->text) == value->len ? value->text : NULL;
}
