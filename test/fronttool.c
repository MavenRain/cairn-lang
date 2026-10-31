/* Domain declaration regressions use the same parser and checker as langc.
   `fronttool dump FILE` prints the syntax tree of FILE, one node a line and
   no positions, so a file and its fmt text can be compared. `fronttool fix
   FILE FIXFILE` prints FILE with the edit of FIXFILE applied, so the fix of
   a `langc check --json` diagnostic can be tested. */
#include <stdio.h>
#include <string.h>

#include "front/blsag.h"
#include "front/check.h"
#include "front/fmt.h"
#include "front/front.h"
#include "front/json.h"
#include "front/lexer.h"
#include "front/parser.h"

/* checked parses TEXT with ORIGIN and checks it in MODE. A domain row is
   trusted text in kit mode; a guest row (orbit-lang S3) is program text in
   guest mode. */
static int checked_in(const char *domain_text, const char *text, Origin origin, CheckMode mode, const char *want) {
  Arena arena;
  Diag diag;
  TokenList tokens = {0};
  DeclList decls = {0};
  Machine machine;
  arena_init(&arena, (size_t)1 << 24);
  diag_init(&diag);
  int ok = domain_text == NULL
    || (lex_source(&arena, "test-prelude", domain_text, strlen(domain_text), &tokens, &diag)
        && parse_source(&arena, "test-prelude", ORIGIN_DOMAIN, &tokens, &decls, &diag));
  ok = ok && lex_source(&arena, "test-domain", text, strlen(text), &tokens, &diag)
    && parse_source(&arena, "test-domain", origin, &tokens, &decls, &diag)
    && check_program(&arena, &decls, &machine, &diag, mode);
  int passed = want == NULL ? ok : !ok && diag.code != NULL && strcmp(diag.code, want) == 0;
  if (passed && want == NULL && machine.has_state) {
    const CtorInfo *ci = &machine.ctors[machine.families[machine.state_family].first_ctor];
    for (uint32_t i = 0; i < ci->field_count; i++)
      passed &= eval_core(&machine, NULL, ci->fields[i].type) != NULL;
  }
  if (!passed) {
    fprintf(stderr, "FAIL %s: wanted %s, got %s: %s\n", mode == CHECK_KIT ? "domain" : "guest",
            want == NULL ? "ok" : want, ok ? "ok" : diag.code == NULL ? "no diagnostic" : diag.code, text);
  }
  arena_release(&arena);
  return passed;
}

static int checked(const char *text, Origin origin, CheckMode mode, const char *want) {
  return checked_in(NULL, text, origin, mode, want);
}

static int law_guest(const char *text, const char *want) {
  return checked_in(
    "family Size := sz0 | szS (szPred : Size)\n"
    "family Trunc1 (A : Type 0) := trunc1 (trunc1Of : A)\n"
    "  | squash1 (squash1X : Trunc1 A) (squash1Y : Trunc1 A) (i : I) [ ~i -> squash1X, i -> squash1Y ]\n"
    "sealed family Enc (A : Type 0) := encEta (encOf : A)\n"
    "law def decrypt : (A : Type 0) -> Enc A -> A := fun A c => fold (fun a => a) c\n"
    "family Quorum := quorum (quorumT : Size) (quorumN : Size)\n"
    "law def open : (A : Type 0) -> Quorum -> Enc A -> A := fun A q c => decrypt A c\n"
    "def ZKProof : Type 0 -> Type 0 := fun W => Trunc1 W\n"
    "sealed family Transcript (W : Type 0) := transcript (transcriptOf : W)\n"
    "law def verified : (W : Type 0) -> Transcript W -> ZKProof W := fun W t => trunc1 W (elim (fun u => W) (fun w => w) t)\n",
    text, ORIGIN_PROGRAM, CHECK_GUEST, want);
}

static int domain(const char *text, const char *want) {
  return checked(text, ORIGIN_DOMAIN, CHECK_KIT, want);
}

static int guest(const char *text, const char *want) {
  return checked(text, ORIGIN_PROGRAM, CHECK_GUEST, want);
}

static int normal_roundtrip_in(const char *domain_text, const char *type, const char *body) {
  char source[4096];
  char normal[2048];
  Arena arena;
  Diag diag;
  TokenList tokens = {0};
  DeclList decls = {0};
  Machine machine;
  snprintf(source, sizeof source, "%s\ndef value : %s := %s", domain_text, type, body);
  arena_init(&arena, (size_t)1 << 24);
  diag_init(&diag);
  int ok = lex_source(&arena, "test-roundtrip", source, strlen(source), &tokens, &diag)
    && parse_source(&arena, "test-roundtrip", ORIGIN_DOMAIN, &tokens, &decls, &diag)
    && check_program(&arena, &decls, &machine, &diag, CHECK_KIT);
  if (ok)
    ok = value_print(&machine, NULL, 0, def_value(&machine, machine.def_count - 1u), normal, sizeof normal);
  if (!ok) {
    fputs("FAIL normal roundtrip: ", stderr);
    diag_print(&diag, stderr);
  }
  arena_release(&arena);
  if (!ok)
    return 0;
  snprintf(source, sizeof source, "%s\ndef value : %s := %s", domain_text, type, normal);
  return domain(source, NULL);
}

static int normal_roundtrip(const char *type, const char *body) {
  return normal_roundtrip_in("", type, body);
}

/* len is a domain fold since S5 (O16), so the mini domain gives the same
   fold over its three cells: an op counts 1, the unit 0, a squash by
   natIsSet. */
static int len_roundtrip(const char *type, const char *body) {
  return normal_roundtrip_in(
    "family FreeMon (A : Type 0) := fmEta (fmEtaOf : A) | fmUnit\n"
    "  | fmSquash (fmFrom : FreeMon A) (fmTo : FreeMon A)"
    " (fmPathL : Path (FreeMon A) fmFrom fmTo) (fmPathR : Path (FreeMon A) fmFrom fmTo) (i : I) (j : I)\n"
    "    [ ~i -> fmPathL @ j, i -> fmPathR @ j, ~j -> fmFrom, j -> fmTo ]\n"
    "def isProp : (A : Type 0) -> Type 0 :=\n"
    "  fun (A : Type 0) => (x : A) -> (y : A) -> Path A x y\n"
    "def isSet : (A : Type 0) -> Type 0 :=\n"
    "  fun (A : Type 0) => (x : A) -> (y : A) -> isProp (Path A x y)\n"
    "def len : (A : Type 0) -> FreeMon A -> Nat :=\n"
    "  fun (A : Type 0) (r : FreeMon A) => fold (fun (a : A) => 1) 0\n"
    "    (fun (x : Nat) (y : Nat) (p : Path Nat x y) (q : Path Nat x y) => fun (i : I) => fun (j : I) => (natIsSet x y p q @ i) @ j) r\n",
    type, body);
}

static int internal_print_refused(const char *type, const char *body) {
  char source[512];
  Arena arena;
  Diag diag;
  TokenList tokens = {0};
  DeclList decls = {0};
  Machine machine;
  FILE *out = tmpfile();
  FILE *err = tmpfile();
  int ok;
  snprintf(source, sizeof source,
    "cofamily Strm (A : Type 0) := strmCorec (strmHead : A) (strmTail : Strm A)\n"
    "def value : %s := %s\n", type, body);
  if (out == NULL || err == NULL) {
    if (out != NULL) fclose(out);
    if (err != NULL) fclose(err);
    return 0;
  }
  arena_init(&arena, (size_t)1 << 24);
  diag_init(&diag);
  ok = lex_source(&arena, "test-internal-print", source, strlen(source), &tokens, &diag)
    && parse_source(&arena, "test-internal-print", ORIGIN_DOMAIN, &tokens, &decls, &diag)
    && check_program(&arena, &decls, &machine, &diag, CHECK_KIT);
  ok = ok && eval_command(&machine, "value", NULL, 0, out, err) == 1
    && diag.code != NULL && strcmp(diag.code, "EVAL_PRINT") == 0
    && ftell(out) == 0;
  if (!ok) fputs("FAIL generated cofamily names must not print as source\n", stderr);
  fclose(out);
  fclose(err);
  arena_release(&arena);
  return ok;
}

static int zk_trap(const char *type, const char *body) {
  char source[2048];
  Arena arena;
  Diag diag;
  TokenList tokens = {0};
  DeclList decls = {0};
  Machine machine;
  const Value *value = NULL;
  snprintf(source, sizeof source,
           "def max : U256 := 115792089237316195423570985008687907853269984665640564039457584007913129639935u\n"
           "def bad : U256 := u256Add max 1u\n"
           "def value : %s := %s", type, body);
  arena_init(&arena, (size_t)1 << 24);
  diag_init(&diag);
  int ok = lex_source(&arena, "test-zk-trap", source, strlen(source), &tokens, &diag)
    && parse_source(&arena, "test-zk-trap", ORIGIN_DOMAIN, &tokens, &decls, &diag)
    && check_program(&arena, &decls, &machine, &diag, CHECK_KIT);
  if (ok)
    value = def_value(&machine, 2u);
  ok = ok && value != NULL && value->kind == VAL_TRAP;
  if (!ok)
    fprintf(stderr, "FAIL zk nested trap: %s\n", body);
  arena_release(&arena);
  return ok;
}

static int zk_kernel(const char *member_type, const char *member,
                     const char *choice_type, const char *choice,
                     const char *checked_choice, const char *want) {
  char source[4096];
  snprintf(source, sizeof source,
           "family Size := sz0 | szS (szPred : Size)\n"
           "family Pos : Size -> Type 0 := posFirst (firstSize : Size) : Pos (szS firstSize)\n"
           "  | posNext (nextSize : Size) (nextPos : Pos nextSize) : Pos (szS nextSize)\n"
           "def M : Type 0 := %s\n"
           "def C : Type 0 := %s\n"
           "def b : ZkBallot C := zkCast M C (%s) (%s)\n"
           "def complete : Path (Type 0) (zkCheck M C (zkProve b) (%s)) Unit := refl\n",
           member_type, choice_type, member, choice, checked_choice);
  return domain(source, want);
}

/* Check command output with the independent verifier, and reject unwritable
   output streams for both successful signatures and signing traps. */
static int prove_output(int trap, int broken, const char *poll) {
  char source[256];
  char text[512];
  Arena arena;
  Diag diag;
  TokenList tokens = {0};
  DeclList decls = {0};
  Machine machine;
  FILE *out = broken ? fopen("/dev/null", "r") : tmpfile();
  FILE *err = tmpfile();
  if (out == NULL || err == NULL) {
    if (out != NULL) fclose(out);
    if (err != NULL) fclose(err);
    fputs("FAIL prove output: cannot open test streams\n", stderr);
    return 0;
  }
  snprintf(source, sizeof source,
           "def b : ZkBallot (Sum Unit Unit) := zkCast (%s) (Sum Unit Unit) (%s) (inr unit)",
           trap ? "Fin 29" : "Unit", trap ? "0" : "unit");
  arena_init(&arena, (size_t)1 << 24);
  diag_init(&diag);
  int ok = lex_source(&arena, "test-prove", source, strlen(source), &tokens, &diag)
    && parse_source(&arena, "test-prove", ORIGIN_DOMAIN, &tokens, &decls, &diag)
    && check_program(&arena, &decls, &machine, &diag, CHECK_KIT);
  if (ok) {
    int result = prove_command(&machine, "b", poll, out, err);
    ok = result == (broken ? 2 : 0)
      && (broken ? diag.code != NULL && strcmp(diag.code, "IO_WRITE") == 0 : diag.code == NULL);
  }
  if (ok && !broken) {
    rewind(out);
    size_t size = fread(text, 1, sizeof text - 1u, out);
    text[size] = '\0';
    ok = !ferror(out) && feof(out);
    if (trap) {
      ok = ok && strcmp(text, "trap\n") == 0;
      rewind(err);
      ok = ok && fgets(text, sizeof text, err) != NULL
        && strncmp(text, "langc: EVAL_OVERFLOW: b:", 23u) == 0;
    } else {
      GroupPoint gen;
      GroupPoint ring;
      U256 one;
      U256 key;
      U256 poll_word;
      U256 wrong_poll;
      U256 sig[4];
      ok = ok && lex_source(&arena, "test-calldata", text, size, &tokens, &diag)
        && tokens.count == 6u;
      for (uint32_t i = 0; ok && i < 5u; i++)
        ok = tokens.items[i].kind == TOK_WORD && tokens.items[i].nat == WORD_U256
          && tokens.items[i].line == (int)i + 1;
      u256_from_u64(&one, 1u);
      u256_from_u64(&poll_word, poll == NULL ? 0u : 7u);
      u256_from_u64(&wrong_poll, poll == NULL ? 7u : 0u);
      ok = ok && u256_cmp(&tokens.items[0].word, &one) == 0;
      group_gen(&gen);
      if (ok) {
        blsag_key(&key, 0u);
        group_mul(&ring, &gen, &key);
        for (uint32_t i = 0; i < 4u; i++) sig[i] = tokens.items[i + 1u].word;
        ok = blsag_verify(&ring, 1u, &poll_word, &one, sig) == BLSAG_ACCEPT
          && blsag_verify(&ring, 1u, &wrong_poll, &one, sig) == BLSAG_REJECT;
      }
    }
  }
  if (!ok) fprintf(stderr, "FAIL prove output: trap=%d broken=%d poll=%s\n",
                   trap, broken, poll == NULL ? "default" : poll);
  fclose(out);
  fclose(err);
  arena_release(&arena);
  return ok;
}

static const char *const TERM_KINDS[] = {"var",  "nat",   "word",  "app",   "lam",    "pi",     "sigma",
                                         "univ", "papp",  "ineg",  "imeet", "ijoin",  "system", "branch"};
static const char *const DECL_KINDS[] = {"def", "axiom", "family", "cofamily"};

static void dump_term(const char *role, const Term *t, int depth) {
  if (t == NULL) return;
  printf("%*s%s %s", depth * 2, "", role, TERM_KINDS[t->kind]);
  if (t->name != NULL) printf(" %s", t->name);
  if (t->kind == TERM_NAT || t->kind == TERM_UNIV || t->kind == TERM_WORD) printf(" %llu", (unsigned long long)t->nat);
  if (t->kind == TERM_WORD && t->word != NULL)
    printf(" %016llx%016llx%016llx%016llx", (unsigned long long)t->word->limb[3], (unsigned long long)t->word->limb[2],
           (unsigned long long)t->word->limb[1], (unsigned long long)t->word->limb[0]);
  putchar('\n');
  dump_term("left", t->left, depth + 1);
  dump_term("right", t->right, depth + 1);
}

static void dump_fields(const char *role, const Field *fields, size_t count, int depth) {
  size_t i;
  for (i = 0; i < count; i++) {
    printf("%*s%s %s%s\n", depth * 2, "", role, fields[i].name == NULL ? "_" : fields[i].name,
           fields[i].history ? " history" : "");
    dump_term("type", fields[i].type, depth + 1);
  }
}

/* dump parses FILE as a program (as langc fmt does) and prints each decl:
   its kind, name, flags and block, then its params, index, type, body and
   ctors. Exit 1 on a lex or parse error, 2 on a read error. */
static int dump(const char *file) {
  static char text[1 << 20];
  FILE *in = fopen(file, "rb");
  if (in == NULL) {
    fprintf(stderr, "fronttool: cannot read %s\n", file);
    return 2;
  }
  size_t len = fread(text, 1, sizeof text, in);
  fclose(in);
  if (len == sizeof text) {
    fprintf(stderr, "fronttool: %s is too large\n", file);
    return 2;
  }
  Arena arena;
  Diag diag;
  TokenList tokens = {0};
  DeclList decls = {0};
  arena_init(&arena, (size_t)1 << 24);
  diag_init(&diag);
  int ok = lex_source(&arena, file, text, len, &tokens, &diag)
    && parse_source(&arena, file, ORIGIN_PROGRAM, &tokens, &decls, &diag);
  if (!ok) diag_print(&diag, stderr);
  size_t i, j;
  for (i = 0; ok && i < decls.count; i++) {
    const Decl *d = &decls.items[i];
    printf("%s %s%s%s%s%s%s block %zu %zu\n", DECL_KINDS[d->kind], d->name, d->is_rec ? " rec" : "",
           d->is_state ? " state" : "", d->is_event ? " event" : "", d->is_sealed ? " sealed" : "",
           d->is_law ? " law" : "", d->block_first, d->block_count);
    dump_fields("param", d->params, d->param_count, 1);
    dump_term("index", d->index, 1);
    dump_term("type", d->type, 1);
    dump_term("body", d->body, 1);
    for (j = 0; j < d->ctor_count; j++) {
      printf("  ctor %s\n", d->ctors[j].name);
      dump_fields("field", d->ctors[j].fields, d->ctors[j].field_count, 2);
      dump_term("result", d->ctors[j].result, 2);
      dump_term("boundary", d->ctors[j].boundary, 2);
    }
  }
  arena_release(&arena);
  return ok ? 0 : 1;
}

/* fix_load reads FILE into TEXT, a buffer of SIZE bytes, and sets *LEN. It
   prints the error and gives 0 on a read error or a file that fills TEXT. */
static int fix_load(const char *file, char *text, size_t size, size_t *len) {
  FILE *in = fopen(file, "rb");
  if (in == NULL) {
    fprintf(stderr, "fronttool: cannot read %s\n", file);
    return 0;
  }
  *len = fread(text, 1, size, in);
  fclose(in);
  if (*len == size) {
    fprintf(stderr, "fronttool: %s is too large\n", file);
    return 0;
  }
  return 1;
}

/* fix_offset sets *AT to the byte offset of LINE:COL in TEXT. It counts as
   the lexer does (lexer.c advance): lines and columns start at 1, a column
   is one byte, and a newline starts column 1 of the next line. The end of
   TEXT is a position; a column past the newline of its line is not. */
static int fix_offset(const char *text, size_t len, long line, long col, size_t *at) {
  long l = 1, c = 1;
  size_t i;
  for (i = 0; i < len && (l != line || c != col); i++) {
    if (l == line && text[i] == '\n') return 0;
    int newline = text[i] == '\n';
    l += newline;
    c = newline ? 1 : c + 1;
  }
  *at = i;
  return l == line && c == col;
}

/* fix prints FILE with one edit applied (S12 A2: a test of the fix of a
   `langc check --json` diagnostic). FIXFILE line 1 is `LINE COL END_LINE
   END_COL`, the fix span, with an exclusive end; the rest of FIXFILE, less
   one final newline, is the text that replaces the span. Exit 2 on a read
   error, a bad line 1, or a span outside FILE or with its end before its
   start. */
static int fix(const char *file, const char *fixfile) {
  static char text[1 << 20];
  static char edit[1 << 20];
  size_t len, edit_len, from, to;
  long line, col, end_line, end_col;
  int head = 0;
  if (!fix_load(file, text, sizeof text, &len) || !fix_load(fixfile, edit, sizeof edit, &edit_len)) return 2;
  edit[edit_len] = '\0';
  if (sscanf(edit, "%ld %ld %ld %ld%n", &line, &col, &end_line, &end_col, &head) != 4
      || edit[head] != '\n' || memchr(edit, '\n', (size_t)head) != NULL) {
    fprintf(stderr, "fronttool: %s: line 1 is not LINE COL END_LINE END_COL\n", fixfile);
    return 2;
  }
  if (!fix_offset(text, len, line, col, &from) || !fix_offset(text, len, end_line, end_col, &to) || to < from) {
    fprintf(stderr, "fronttool: %s: the span is outside %s or ends before it starts\n", fixfile, file);
    return 2;
  }
  size_t at = (size_t)head + 1u;
  size_t count = edit_len - at;
  if (count > 0 && edit[edit_len - 1] == '\n') count--;
  fwrite(text, 1, from, stdout);
  fwrite(edit + at, 1, count, stdout);
  fwrite(text + to, 1, len - to, stdout);
  return 0;
}

/* The original text parses, but a canonical spelling can cross a compiler
   limit. The command must refuse that text before it reaches stdout. */
static int fmt_limit(const char *kind, size_t count, const char *want) {
  static char source[SOURCE_MAX_BYTES + 1u];
  size_t at = 0;
  if (strcmp(kind, "size") == 0) {
    const char *tail = "\ndef n:Nat:=0\n";
    at = SOURCE_MAX_BYTES - strlen(tail);
    memset(source, 'a', at);
    source[0] = '-';
    source[1] = '-';
    memcpy(source + at, tail, strlen(tail) + 1u);
  } else {
    int sigma = strcmp(kind, "sigma") == 0;
    int lambda = strcmp(kind, "lambda") == 0;
    at += (size_t)snprintf(source + at, sizeof source - at, "def deep : Type 0 := %s", lambda ? "fun (" : sigma ? "" : "(");
    for (size_t i = 0; i < count; i++)
      at += (size_t)snprintf(source + at, sizeof source - at, sigma ? "Sigma (x%zu : Nat) " : "x%zu ", i);
    snprintf(source + at, sizeof source - at, "%s", sigma ? "Nat\n" : lambda ? ": Nat) => x0\n" : ": Nat) -> Nat\n");
  }
  Arena arena;
  Diag diag;
  TokenList toks = {0};
  DeclList decls = {0};
  FmtText out;
  arena_init(&arena, (size_t)1 << 29);
  diag_init(&diag);
  fmt_text_init(&out, &arena);
  int ok = lex_source(&arena, "fmt-limit", source, strlen(source), &toks, &diag)
    && parse_source(&arena, "fmt-limit", ORIGIN_PROGRAM, &toks, &decls, &diag)
    && fmt_program(&out, decls.items, decls.count, &toks);
  if (ok) {
    int valid = fmt_validate(&out, "fmt-limit", &diag);
    ok = want == NULL ? valid && !diag.set : !valid && diag.set && strcmp(diag.code, want) == 0;
  }
  if (!ok) fprintf(stderr, "FAIL fmt %s limit: want %s, got %s\n", kind,
                   want == NULL ? "accepted" : want, diag.code == NULL ? "accepted" : diag.code);
  arena_release(&arena);
  return ok;
}

/* S12 (Q-S12-12): two paths that no program text reaches through `langc
   check --json`. A quote goes into a JSON string only from a file path, and
   no fix is pending when branch_index resets diag->set (the reset needs a
   first TYPE_MISMATCH, and no check code with a fix is TYPE_MISMATCH). */
static int json_quote(void) {
  FILE *out = tmpfile();
  char got[64];
  size_t n;
  int ok;
  if (out == NULL) {
    fputs("FAIL json quote: no tmpfile\n", stderr);
    return 0;
  }
  json_string("a\"b\\c\nd", out);
  rewind(out);
  n = fread(got, 1u, sizeof got - 1u, out);
  fclose(out);
  got[n] = '\0';
  ok = strcmp(got, "\"a\\\"b\\\\c\\nd\"") == 0;
  if (!ok) fprintf(stderr, "FAIL json quote: got %s\n", got);
  return ok;
}

static int diag_fix_reset(void) {
  Diag diag;
  Span span;
  int ok;
  memset(&span, 0, sizeof span);
  diag_init(&diag);
  diag_fix(&diag, span, "x");
  diag_fail(&diag, "TYPE_MISMATCH", NULL, "first");
  ok = diag.fix_set == 1;
  diag.set = 0;
  diag_fail(&diag, "HIT_INDEX", NULL, "second");
  ok = ok && diag.fix_set == 0 && strcmp(diag.code, "HIT_INDEX") == 0;
  if (!ok) fprintf(stderr, "FAIL diag fix reset: fix_set %d, code %s\n", diag.fix_set, diag.code);
  return ok;
}

/* Apply each op-slot fix and check the resulting program. Its Nat body
   still has the wrong type, but the replacement type must parse. */
static int signature_fixes(void) {
  const char *prelude = "def isSet : Type 0 -> Type 0 := fun A => Unit\n"
    "def Constitution : Type 0 := Unit\n";
  const char *slots[] = {"act", "outcomeSet", "menu"};
  int passed = 1;
  for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) {
    Arena arena;
    Diag diag;
    TokenList tokens = {0};
    DeclList decls = {0};
    Machine machine;
    char source[1024], fixed[2048];
    size_t from = 0, to = 0;
    snprintf(source, sizeof source, "def Member : Type 0 := Unit\n"
      "def Choice : Type 0 := Unit\ndef Group : Type 0 := Unit\n"
      "def Outcome : Type 0 := Unit\ndef %s : Nat := 0\n", slots[i]);
    arena_init(&arena, (size_t)1 << 24);
    diag_init(&diag);
    int ok = lex_source(&arena, "prelude", prelude, strlen(prelude), &tokens, &diag)
      && parse_source(&arena, "prelude", ORIGIN_DOMAIN, &tokens, &decls, &diag)
      && lex_source(&arena, "program", source, strlen(source), &tokens, &diag)
      && parse_source(&arena, "program", ORIGIN_PROGRAM, &tokens, &decls, &diag);
    ok = ok && !check_program(&arena, &decls, &machine, &diag, CHECK_KIT)
      && diag.code != NULL && strcmp(diag.code, "REFUSE_SIG") == 0 && diag.fix_set
      && fix_offset(source, strlen(source), diag.fix_span.line, diag.fix_span.col, &from)
      && fix_offset(source, strlen(source), diag.fix_span.end_line, diag.fix_span.end_col, &to)
      && to >= from;
    if (ok) {
      snprintf(fixed, sizeof fixed, "%.*s%s%s", (int)from, source, diag.fix, source + to);
      ok = checked_in(prelude, fixed, ORIGIN_PROGRAM, CHECK_KIT, "TYPE_MISMATCH");
    }
    if (!ok) fprintf(stderr, "FAIL signature fix: %s\n", slots[i]);
    passed &= ok;
    arena_release(&arena);
  }
  return passed;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "dump") == 0) return dump(argv[2]);
  if (argc == 4 && strcmp(argv[1], "fix") == 0) return fix(argv[2], argv[3]);
  if (argc != 1) {
    fputs("usage: fronttool [dump FILE | fix FILE FIXFILE]\n", stderr);
    return 2;
  }
  int ok = 1;
  ok &= fmt_limit("sigma", 400u, NULL);
  ok &= fmt_limit("sigma", 510u, "PARSE_DEPTH");
  ok &= fmt_limit("pi", 1001u, "PARSE_DEPTH");
  ok &= fmt_limit("lambda", 1001u, "PARSE_DEPTH");
  ok &= fmt_limit("size", 0u, "FMT_SIZE");
  /* Kernel set constants have fixed types, even without the domain aliases. */
  ok &= domain("def s : (x y : Nat) -> (p q : Path Nat x y) -> Path (Path Nat x y) p q := natIsSet", NULL);
  ok &= domain("def s : (x y : U256) -> (p q : Path U256 x y) -> Path (Path U256 x y) p q := u256IsSet", NULL);
  ok &= domain("def s : (x y : Flag) -> (p q : Path Flag x y) -> Path (Path Flag x y) p q := flagIsSet", NULL);
  ok &= domain("def isSet : Type 0 -> Type 0 := fun A => Path Nat 0 1\n"
                "def forged : Path Nat 0 1 := natIsSet", "TYPE_MISMATCH");
  ok &= domain("def isSet : Type 0 -> Type 0 := fun A => Path Nat 0 1\n"
                "def forged : Path Nat 0 1 := u256IsSet", "TYPE_MISMATCH");
  ok &= domain("def isSet : Type 0 -> Type 0 := fun A => Path Nat 0 1\n"
                "def forged : Path Nat 0 1 := flagIsSet", "TYPE_MISMATCH");
  ok &= domain("def bound : (A : Type 0) -> (z : A) -> (x y : Nat) -> (p q : Path Nat x y) -> Path (Path Nat x y) p q := fun A z => natIsSet", NULL);
  /* Time words are forbidden in local scopes as well as declarations. */
  ok &= domain("def bad : (now : Nat) -> Nat := fun x => x", "REFUSE_TIME");
  ok &= domain("def bad : Nat -> Nat := fun clock => clock", "REFUSE_TIME");
  ok &= domain("def bad : Nat -> Nat := fun time => 0", "REFUSE_TIME");
  ok &= domain("def bad : Path Nat 0 0 := fun (block : I) => 0", "REFUSE_TIME");
  ok &= domain("family Test (timestamp : Type 0) := makeTest (payload : timestamp)", "REFUSE_TIME");
  ok &= domain("def bad : Path Nat 0 0 := fun epoch => 0", "REFUSE_TIME");
  /* Counting an open higher cell must preserve its path boundaries. */
  ok &= len_roundtrip(
    "(r : FreeMon Unit) -> (p : Path (FreeMon Unit) (fmUnit Unit) r) -> Path Nat 0 (len Unit r)",
    "fun r p => fun (j : I) => len Unit (hcomp (FreeMon Unit) j (fun (i : I) => [ j -> p @ i ]) (fmUnit Unit))");
  ok &= len_roundtrip(
    "(r : FreeMon Unit) -> (p : Path (FreeMon Unit) (fmUnit Unit) r) -> Path Nat 0 (len Unit r)",
    "fun r p => fun j => len Unit (fmSquash Unit (fmUnit Unit) r p p j j)");
  ok &= len_roundtrip(
    "(p : Path (FreeMon Unit) (fmUnit Unit) (fmUnit Unit)) -> "
    "PathP (fun (j : I) => Path Nat (len Unit (p @ j)) (len Unit (p @ j))) refl refl",
    "fun p => fun j => fun i => len Unit (fmSquash Unit (fmUnit Unit) (fmUnit Unit) p p i j)");
  ok &= len_roundtrip(
    "Path Nat (len Unit (hcomp (FreeMon Unit) i0 (fun (i : I) => [ ]) (fmUnit Unit))) 0", "refl");
  ok &= internal_print_refused("Type 0", "Strm Unit");
  ok &= internal_print_refused("Strm Unit", "strmCorec Unit Unit (fun s => s) (fun s => s) unit");
  ok &= normal_roundtrip("Map Nat Nat -> Nat", "fun m => mapGet m 0");
  ok &= normal_roundtrip("Map Nat Nat -> Map Nat Nat", "fun m => mapSet m 0 1");
  ok &= prove_output(0, 1, NULL);
  ok &= prove_output(1, 1, NULL);
  ok &= prove_output(0, 0, NULL);
  ok &= prove_output(0, 0, "7u");
  ok &= prove_output(1, 0, NULL);
  /* A ballot cannot project its choice at an unrelated type. */
  ok &= domain("family Empty := emptyLoop (emptyOf : Empty)\n"
               "def forged : Empty := zkProject Empty (zkCast Unit Unit unit unit)",
               "TYPE_MISMATCH");
  ok &= domain("def forged : ZkBallot Unit -> Sum Unit Unit := "
               "fun b => zkProject (Sum Unit Unit) b", "TYPE_MISMATCH");
  ok &= domain("def projected : (C : Type 0) -> ZkBallot C -> C := "
               "fun (C : Type 0) (b : ZkBallot C) => zkProject C b", NULL);
  ok &= domain("def bad : ZkWitness := zkProve unit", "TYPE_MISMATCH");
  ok &= domain("def constant : (C : Type 0) -> (b : ZkBallot C) -> "
               "Path (ZkBallot C) (transp (fun (_ : I) => ZkBallot C) i0 b) b := "
               "fun (C : Type 0) (b : ZkBallot C) => refl", NULL);
  ok &= normal_roundtrip("(p : Path (Type 0) Unit (Sum Unit Unit)) -> ZkBallot (Sum Unit Unit)",
                         "fun (p : Path (Type 0) Unit (Sum Unit Unit)) => "
                         "transp (fun (i : I) => ZkBallot (p @ i)) i0 (zkCast Unit Unit unit unit)");
  /* Recursive occurrences under function domains remain forbidden in paths. */
  ok &= domain("family Bad := base | mkBad (x : Bad) "
               "(p : Path (Bad -> Bad) (fun (y : Bad) => x) (fun (y : Bad) => x))",
               "DOMAIN_FAMILY");
  /* Projection only reads the stored choice, including under binders. */
  ok &= domain("def projected : (M : Type 0) -> (C : Type 0) -> (m : M) -> (c : C) -> "
               "Path C (zkProject C (zkCast M C m c)) c := "
               "fun (M : Type 0) (C : Type 0) (m : M) (c : C) => refl", NULL);
  /* A choice index must not wrap to that of a different constructor. */
  ok &= zk_kernel("Unit", "unit", "Sum (Fin 18446744073709551615) (Sum Unit Unit)",
                  "inl 0", "inr (inr unit)", "PATH_END");
  ok &= zk_kernel("Unit", "unit", "Sum (Fin 18446744073709551615) Unit",
                  "inr unit", "inr unit", NULL);
  /* Pos is the member type of the bag-orbit DAO, with zero-based positions. */
  ok &= zk_kernel("Pos (szS sz0)", "posFirst sz0", "Sum Unit Unit",
                  "inl unit", "inl unit", NULL);
  ok &= zk_kernel("Pos (szS (szS (szS sz0)))", "posNext (szS (szS sz0)) (posNext (szS sz0) (posFirst sz0))",
                  "Pos (szS (szS sz0))", "posNext (szS sz0) (posFirst sz0)",
                  "posNext (szS sz0) (posFirst sz0)", NULL);
  ok &= zk_kernel("Pos (szS (szS (szS sz0)))", "posNext (szS (szS sz0)) (posNext (szS sz0) (posFirst sz0))",
                  "Pos (szS (szS sz0))", "posNext (szS sz0) (posFirst sz0)",
                  "posFirst (szS sz0)", "PATH_END");
  ok &= domain("family Size := sz0 | szS (szPred : Size)\n"
               "family Pos : Size -> Type 0 := posFirst (firstSize : Size) : Pos (szS firstSize)\n"
               "  | posNext (nextSize : Size) (nextPos : Pos nextSize) : Pos (szS nextSize)\n"
               "def sameEncoding : Path ZkWitness "
               "(zkProve (zkCast (Pos (szS (szS (szS sz0)))) (Pos (szS (szS sz0))) "
               "(posNext (szS (szS sz0)) (posNext (szS sz0) (posFirst sz0))) "
               "(posNext (szS sz0) (posFirst sz0)))) "
               "(zkProve (zkCast (Sum Unit (Sum Unit Unit)) (Sum Unit Unit) "
               "(inr (inr unit)) (inr unit))) := refl", NULL);
  ok &= zk_trap("Prod U256 U256", "zkHp (pair bad 2u)");
  ok &= zk_trap("U256", "zkDigest (cons (pair bad 2u) nil)");
  ok &= zk_trap("U256", "zkDigest (cons (flagIf (u256Eq bad 0u) (pair 1u 2u) (pair 1u 2u)) nil)");
  ok &= zk_trap("U256", "zkDigest (cons (pair 1u 2u) (flagIf (u256Eq bad 0u) nil nil))");
  ok &= zk_trap("Flag", "zkVerify (cons (pair 1u 2u) nil) 0u 1u (cons bad (cons 1u (cons 1u (cons 2u nil))))");
  ok &= normal_roundtrip("Prod U256 U256 -> Prod U256 U256", "fun (p : Prod U256 U256) => zkHp p");
  ok &= normal_roundtrip("List (Prod U256 U256) -> U256", "fun (ring : List (Prod U256 U256)) => zkDigest ring");
  ok &= normal_roundtrip("List U256 -> Flag", "fun (sig : List U256) => zkVerify (cons (pair 1u 2u) nil) 0u 1u sig");
  /* The empty domain (sealed-lang S2, R21) lexes, parses and checks. */
  ok &= domain("", NULL);
  /* Elimination consumes its scrutinee before applying a functional result. */
  ok &= guest("def applyUnit : Unit -> Unit -> Unit := fun (u : Unit) (v : Unit) => "
              "elim (fun (w : Unit) => Unit -> Unit) (fun x => x) u v", NULL);
  ok &= guest("def applySum : Sum Unit Unit -> Unit -> Unit := fun (s : Sum Unit Unit) (v : Unit) => "
              "elim (fun (t : Sum Unit Unit) => Unit -> Unit) (fun a x => x) (fun b x => x) s v", NULL);
  ok &= guest("def choose : Sum Unit Unit -> Unit := "
              "elim (fun (s : Sum Unit Unit) => Unit) (fun u => u) (fun u => u)", NULL);
  ok &= normal_roundtrip("Unit -> Unit", "fun (u : Unit) => elim (fun (v : Unit) => Unit) unit u");
  ok &= normal_roundtrip("Sum Unit Unit -> Unit", "fun (s : Sum Unit Unit) => "
                         "elim (fun (t : Sum Unit Unit) => Unit) (fun u => u) (fun u => u) s");
  ok &= normal_roundtrip("(A : Type 0) -> (s : Sum A Unit) -> Path (Sum A Unit) s s", "fun (A : Type 0) (s : Sum A Unit) => "
                         "elim (fun (t : Sum A Unit) => Path (Sum A Unit) t t) (fun a => refl) (fun u => refl) s");
  ok &= guest("def s : Sum Unit Unit := inl unit\n"
              "def left : (v : Unit) -> Path Unit (elim (fun (t : Sum Unit Unit) => Unit -> Unit) "
              "(fun a x => x) (fun b x => unit) s v) v := fun v => refl\n"
              "def t : Sum Unit Unit := inr unit\n"
              "def right : (v : Unit) -> Path Unit (elim (fun (t : Sum Unit Unit) => Unit -> Unit) "
              "(fun a x => x) (fun b x => unit) t v) unit := fun v => refl\n"
              "def one : (v : Unit) -> Path Unit (elim (fun (u : Unit) => Unit -> Unit) "
              "(fun x => x) unit v) v := fun v => refl", NULL);
  ok &= guest("def bad : Unit -> Unit := fun (u : Unit) => elim (fun v => unit) unit u", "TYPE_MISMATCH");
  ok &= guest("def bad : Unit -> Unit := fun (u : Unit) => "
              "elim (fun (t : Sum Unit Unit) => Unit) (fun a => a) (fun b => b) u", "TYPE_MISMATCH");
  ok &= guest("def bad : Unit -> Unit := fun (u : Unit) => "
              "elim (fun (v : Unit) => Unit) unit (inl u)", "TYPE_INFER");
  /* Family eliminators can await their scrutinee. The arity comes from
     the final motive domain, including for named and indexed motives. */
  ok &= domain("family N := zero | succ (pred : N)\n"
               "def id : N -> N := elim (fun (n : N) => N) zero (fun n ih => succ ih)", NULL);
  ok &= domain("family N := zero | succ (pred : N)\n"
               "def P : N -> Type 0 := fun n => N\n"
               "def id : N -> N := elim P zero (fun n ih => succ ih)", NULL);
  ok &= normal_roundtrip_in("family N := zero | succ (pred : N)", "N -> N",
                            "elim (fun n => N) zero (fun n ih => succ ih)");
  ok &= domain("family N := zero | succ (pred : N)\n"
               "family V : N -> Type 0 := nilV : V zero | consV (vlen : N) (tailV : V vlen) : V (succ vlen)\n"
               "def id : (n : N) -> V n -> N := fun n => "
               "elim (fun (k : N) (v : V k) => N) zero (fun k t ih => succ ih)", NULL);
  ok &= domain("family N := zero | succ (pred : N)\n"
               "family V : N -> Type 0 := nilV : V zero | consV (vlen : N) (tailV : V vlen) : V (succ vlen) | altV (altLen : N) : V altLen\n"
               "def id : (n : N) -> V n -> N := fun n => "
               "elim (fun (k : N) (v : V k) => N) zero (fun k t ih => succ ih) (fun k => k)", NULL);
  /* A saturated eliminator with a functional result still consumes the
     scrutinee before the result arguments. */
  ok &= domain("family N := zero | succ (pred : N)\n"
               "def id : N -> N -> N := fun n => elim (fun (k : N) => N -> N) (fun x => x) (fun k ih x => succ (ih x)) n\n"
               "def reduction : Path N (id (succ zero) zero) (succ zero) := refl", NULL);
  /* Bare indexed motives accept and check annotations on later binders. */
  ok &= domain("family N := zero | succ (pred : N)\n"
               "family V : N -> Type 0 := nilV : V zero | consV (vlen : N) (tailV : V vlen) : V (succ vlen)\n"
               "def id : (n : N) -> V n -> N := fun n v => "
               "elim (fun k => fun (t : V k) => N) zero (fun k t ih => succ ih) v", NULL);
  ok &= domain("family N := zero | succ (pred : N)\n"
               "family V : N -> Type 0 := nilV : V zero | consV (vlen : N) (tailV : V vlen) : V (succ vlen)\n"
               "def bad : (n : N) -> V n -> N := fun n v => "
               "elim (fun k => fun (t : N) => N) zero (fun k t ih => succ ih) v", "TYPE_MISMATCH");
  ok &= domain("family F := makeF [ i1 -> missing ]", "DOMAIN_FAMILY");
  ok &= domain("family F := makeF [ ]", "DOMAIN_FAMILY");
  /* A path constructor's fields are not record projections: projection
     would discard its boundary at a neutral interval. */
  ok &= domain("family Only := arc (arcFrom : Only) (arcTo : Only) (i : I) "
               "[ ~i -> arcFrom, i -> arcTo ]\n"
               "def bad : (x : Only) -> (y : Only) -> Path Only (arcFrom x) (arcFrom y) := "
               "fun x y => fun (i : I) => arcFrom (arc x y i)", "NAME_UNKNOWN");
  ok &= domain("family T := pt (value : Nat) | arc (previous : T) (i : I) "
               "[ ~i -> pt (fold (fun n => n) 1 previous), i -> pt (fold (fun n => n) 1 previous) ]", "HIT_BOUNDARY");
  /* Indexed families and blocks (S6). hiit-wrong-index: an end of a path
     constructor is the family at another index. hiit-no-result: a
     constructor of an indexed family states no result. block-order: a
     header names a later sort of its block. index-prop: Prop is not a type
     in an index telescope (rule R4). block-ok: a field names a later sort. */
  ok &= domain("family Sz := zs | ss (szPrev : Sz)\n"
               "family V : Sz -> Type 0 := vnil : V zs | vcons (vSize : Sz) (vTail : V vSize) : V (ss vSize) "
               "| vbad (badSize : Sz) (badTail : V badSize) (i : I) : V (ss badSize) "
               "[ ~i -> vcons badSize badTail, i -> badTail ]", "HIT_INDEX");
  ok &= domain("family Sz := zs | ss (szPrev : Sz)\n"
               "family V : Sz -> Type 0 := vnil | vcons (vSize : Sz) (vTail : V vSize) : V (ss vSize)", "HIT_INDEX");
  ok &= domain("family P0 : Q0 -> Type 0 := p0 : P0 q0 and Q0 := q0", "BLOCK_ORDER");
  /* hiit-wrong-head: the result of a constructor is not its family. */
  ok &= domain("family Sz := zs | ss (szPrev : Sz)\n"
               "family V : Sz -> Type 0 := vnil : Sz | vcons (vSize : Sz) (vTail : V vSize) : V (ss vSize)", "HIT_INDEX");
  ok &= domain("family W : Prop -> Type 0 := w0", "NAME_UNKNOWN");
  ok &= domain("family Ch := gen | adopt (adoptOf : Ch) (adoptBy : Rt adoptOf) "
               "and Rt : Ch -> Type 0 := rat (ratOf : Ch) : Rt ratOf", NULL);
  /* Index arguments are not constructor parameters in point or path cases. */
  ok &= domain("family Sz := zs | ss (pred : Sz)\n"
               "family V (A : Type 0) : Sz -> Type 0 := vn : V A zs "
               "| vc (item : A) : V A (ss zs) "
               "| seg (i : I) : V A zs [ ~i -> vn A, i -> vn A ]\n"
               "def result : Path Nat (fold 0 (fun a => a) refl (vc Nat 7)) 7 := refl", NULL);
  /* A transport's printed result must still inhabit the destination fibre. */
  ok &= normal_roundtrip_in("family Sz := zs | ss (pred : Sz)\n"
                            "family V (A : Type 0) : Sz -> Type 0 := vn : V A zs | vg (vgOf : A) : V A zs",
                            "V Nat zs", "transp (fun (i : I) => V Nat zs) i0 (vn Nat)");
  ok &= normal_roundtrip_in("family Sz := zs | ss (pred : Sz) "
                            "| edge (i : I) [ ~i -> zs, i -> ss zs ]\n"
                            "family V : Sz -> Type 0 := vn : V zs | vc : V (ss zs)",
                            "V (ss zs)", "transp (fun (i : I) => V (edge i)) i0 vn");
  ok &= normal_roundtrip_in("family Even := ez | es (oddPrev : Odd) "
                            "and Odd := os (evenPrev : Even)",
                            "Even", "transp (fun (i : I) => Even) i0 (es (os ez))");
  ok &= domain("family Even := ez | es (oddPrev : Odd) and Odd := os (evenPrev : Even)\n"
               "def result : Nat := fold 0 (fun n => natAdd n 1) (es (os ez))", "TYPE_MISMATCH");
  /* Block folds print their targets before their constructor cases. */
  ok &= normal_roundtrip_in("family Even := ez | es (oddPrev : Odd) and Odd := os (evenPrev : Even)",
                            "Even -> Nat", "fun x => fold Nat Unit 0 (fun u => 1) (fun n => unit) x");
  ok &= normal_roundtrip_in("family Even := ez | es (oddPrev : Odd) and Odd := os (evenPrev : Even)",
                            "Odd -> Unit", "fun x => fold Nat Unit 0 (fun u => 1) (fun n => unit) x");
  ok &= domain("family Even := ez | es (oddPrev : Odd) and Odd := os (evenPrev : Even)\n"
               "def point : Path Nat (fold Nat Unit 0 (fun u => 1) (fun n => unit) (es (os ez))) 1 := refl\n"
               "def other : Path Unit (fold Nat Unit 0 (fun u => 1) (fun n => unit) (os ez)) unit := refl", NULL);
  /* A recursive field of a sibling sort is typed at the target of that
     sort, and it folds at that sort. */
  ok &= domain("family Even := ez | es (oddPrev : Odd) and Odd := os (evenPrev : Even)\n"
               "def one : Path Nat (fold Nat Unit 0 (fun (u : Unit) => 1) (fun (n : Nat) => unit) (es (os ez))) 1 := refl", NULL);
  ok &= domain("family Even := ez | es (oddPrev : Odd) and Odd := os (evenPrev : Even)\n"
               "def two : Path Nat (fold Nat Nat 0 (fun (u : Nat) => natAdd u 1) (fun (n : Nat) => natAdd n 1) (es (os ez))) 2 := refl", NULL);
  /* block-fold-extra: a block fold with an extra argument before the
     value. Without the arity test the extra argument is never read. */
  ok &= domain("family Even := ez | es (oddPrev : Odd) and Odd := os (evenPrev : Even)\n"
               "def bad : Nat := fold (Even -> Nat) Unit (fun (e : Even) => 0) (fun (u : Unit) => fun (e : Even) => 1) "
               "(fun (g : Even -> Nat) => unit) unit ez", "TYPE_MISMATCH");
  /* A path field over a sibling sort folds at that sibling. */
  ok &= domain("family Box := box (boxLeft : Item) (boxRight : Item) (boxPath : Path Item boxLeft boxRight) "
               "and Item := item\n"
               "def result : Path Nat 7 7 := fun (i : I) => "
               "fold Nat Unit (fun x y p => elim (fun (u : Unit) => Nat) 7 (p @ i)) unit (box item item refl)", NULL);
  /* Every sort's cases must use the scrutinee's parameter telescope. */
  ok &= domain("family F (m : Flag) := makeF (fValue : Nat) and G (n : Nat) := makeG\n"
               "def result : Unit := fold Unit Unit (fun x => unit) unit (makeG 7)", "TYPE_MISMATCH");
  ok &= domain("family F (m : Nat) := makeF (fValue : Nat) and G := makeG\n"
               "def result : Unit := fold Unit Unit (fun x => unit) unit makeG", "TYPE_MISMATCH");
  ok &= domain("family L (n : Nat) (e : Path Nat n n) := leftL (lValue : Nat) "
               "and R (n : Nat) (e : Path Nat n n) := rightR (rLeft : L n e)\n"
               "def firstL : Path Nat (fold Nat Unit (fun v => v) (fun v => unit) (leftL 7 refl 8)) 8 := refl\n"
               "def secondR : Path Unit (fold Nat Unit (fun v => v) (fun v => unit) (rightR 7 refl (leftL 7 refl 8))) unit := refl", NULL);
  /* Header folds must not see the zeroed, reserved constructor signatures. */
  ok &= domain("family A := a (n : Nat) and B (x : A) (y : fold Unit x) := b\n"
               "def value : B (a 0) unit := b (a 0) unit", "HIT_BOUNDARY");
  /* A later sort's universe propagates backwards through the block. */
  ok &= domain("family Low := low (high : High) and High := highCtor (big : Type 0)\n"
               "def small : Type 0 := Low", "TYPE_MISMATCH");
  ok &= domain("family Low := low (high : High) and High := highCtor (big : Type 0)\n"
               "def large : Type 1 := Low", NULL);
  ok &= domain("family F : ((G : Type 0) -> G -> G) -> Type 0 := "
               "mk (x : (T : Type 0) -> T -> T) : F x and G := g", NULL);
  ok &= domain("family F (G : Nat) : Path Nat G G -> Type 0 := mk (x : Path Nat G G) : F G x and G := g", NULL);
  ok &= domain("state State : Nat -> Type 0 := st (counter : Nat) : State counter", "REFUSE_STATE");
  ok &= domain("family U := u\n"
               "family T := pt (value : Nat) | arc (i : I) "
               "[ ~i -> pt (fold 0 u), i -> pt (fold 0 u) ]", NULL);
  ok &= domain("family T (n : Nat) := leafT (v : Nat) | nodeT (l : T 0) (r : T n)", "DOMAIN_FAMILY");
  /* A type constructor is a cell signature (S3, rule R1): a cons list, a
     value parameter, a parameter in no constructor, an and block and two
     generators for one parameter (open point QA1) are refused. */
  ok &= domain("family Lst (A : Type 0) := lstNil | lstCons (lstHead : A) (lstTail : Lst A)", "REFUSE_CELL");
  ok &= domain("family Tagged (A : Type 0) (n : Nat) := tagged (taggedOf : A)", "REFUSE_CELL");
  ok &= domain("family Phantom (A : Type 0) := phantom", "REFUSE_CELL");
  ok &= domain("family Even (A : Type 0) := ez | es (oddPrev : Odd A) and Odd (A : Type 0) := os (evenPrev : Even A)", "REFUSE_CELL");
  ok &= domain("family Two (A : Type 0) := genL (genLOf : A) | genR (genROf : A)", "REFUSE_CELL");
  /* Aliases and indices must not hide type arguments from the cell check. */
  ok &= domain("def U : Type 1 := Type 0\n"
               "family AliasList (A : U) := aliasNil | aliasCons (aliasHead : A) (aliasTail : AliasList A)", "REFUSE_CELL");
  ok &= domain("def U : Type 1 := Type 0\n"
               "family AliasCell (A : U) := aliasEta (aliasValue : A)", NULL);
  ok &= domain("family IndexedList : Type 0 -> Type 0 := idxNil (idxA : Type 0) : IndexedList idxA "
               "| idxCons (idxB : Type 0) (idxHead : idxB) (idxTail : IndexedList idxB) : IndexedList idxB", "REFUSE_CELL");
  ok &= domain("def U : Type 1 := Type 0\n"
               "family IndexedAlias : U -> Type 0 := indexedAlias (indexedType : U) : IndexedAlias indexedType", "REFUSE_CELL");
  ok &= domain("family Hkt (F : Type 0 -> Type 0) := hktNil | hktNode (hktValue : F Nat) (hktTail : Hkt F)", "REFUSE_CELL");
  ok &= domain("def Kind : Type 1 := Type 0 -> Type 0\n"
               "family HktAlias (F : Kind) := hktAliasNil", "REFUSE_CELL");
  /* Fixed operation labels are closed base data in a cell signature. */
  ok &= domain("family Labelled (A : Type 0) := labelledEta (labelledValue : A) "
               "| labelledNode (labelledTag : Nat) (labelledTail : Labelled A)", NULL);
  ok &= domain("family ClosedFn (A : Type 0) := closedEta (closedValue : A) "
               "| closedNode (closedLabel : (n : Nat) -> Path Nat n n) (closedTail : ClosedFn A)", NULL);
  ok &= domain("family BadLabel (A : Type 0) := badEta (badValue : A) "
               "| badNode (badLabel : Unit -> A) (badTail : BadLabel A)", "REFUSE_CELL");
  ok &= domain("family Wrapped (A : Type 0) := wrappedEta (wrappedValue : A) "
               "| wrappedNode (wrappedTail : Option (Wrapped A))", "REFUSE_CELL");
  ok &= domain("family F := makeF (field : Nat) (field : Flag)", "REFUSE_NAME");
  ok &= domain("family F := makeF (makeF : Nat)", "REFUSE_NAME");
  ok &= domain("family F := makeF (firstField : Nat) (secondField : Flag)", NULL);
  ok &= domain("family F := a (field : Nat) | b (field : Nat)", "REFUSE_NAME");
  ok &= domain("def s : Sum Nat Nat := inl 7\ndef bad : Sum Flag Nat := bind s (fun x => inr x)", "TYPE_MISMATCH");
  ok &= domain("family Lot (n : Nat) := makeLot (amount : Nat)\n"
               "def s : Sum Nat Nat := inl 0\n"
               "def f : (n : Nat) -> Nat -> Lot n := fun n x => makeLot n x\n"
               "def bad : (n : Nat) -> Lot n := first (pair (either f f s) 0)", "TYPE_MISMATCH");
  ok &= domain("def bad : Type 1 := Nat", "TYPE_MISMATCH");
  ok &= domain("def bad : Type 2 := Type 1", "TYPE_UNIVERSE");
  ok &= domain("family U := makeU (decode : Type 0)\ndef bad : Type 0 := U", "TYPE_MISMATCH");
  ok &= domain("family U := makeU (decode : Type 0)\nfamily W := makeW (wrapped : U)\ndef bad : Type 0 := W", "TYPE_MISMATCH");
  ok &= domain("family Box (A : Type 1) := box (unbox : A)\ndef bad : Type 0 := Box (Type 0)", "TYPE_MISMATCH");
  ok &= domain("family U := makeU (decode : Type 0)\ndef good : Type 1 := U", NULL);
  ok &= domain("def bad : Type 0 := Path (Type 0) Nat Nat", "TYPE_MISMATCH");
  ok &= domain("def good : Type 1 := Path (Type 0) Nat Nat", NULL);
  ok &= domain("def ConstNat : Nat -> Type 0 := fun n => Nat\n"
               "state State := makeState (count : Nat) (other : ConstNat count)\n"
               "def init : State := makeState 3 7\n"
               "def value : Path Nat (other init) 7 := refl", NULL);
  ok &= domain("state State := makeState (count : Nat) (proof : Path Nat count count)", "REFUSE_STATE");
  ok &= domain("family U := makeU (decode : Type 0)\n"
               "def bad : Type 0 := Path U (makeU Nat) (makeU Nat)", "TYPE_MISMATCH");
  ok &= domain("family U := makeU (decode : Type 0)\n"
               "def good : Type 1 := Path U (makeU Nat) (makeU Nat)", NULL);
  ok &= domain("family U := makeU (decode : Type 0)\n"
               "family W := makeW (proof : Path U (makeU Nat) (makeU Nat))\n"
               "def bad : Type 0 := W", "TYPE_MISMATCH");
  ok &= domain("family U := makeU (decode : Type 0)\n"
               "family W := makeW (proof : Path U (makeU Nat) (makeU Nat))\n"
               "def good : Type 1 := W", NULL);
  ok &= domain("def eta : (p : Prod Nat Nat) -> Path (Prod Nat Nat) (pair (first p) (second p)) p := fun p => refl", NULL);
  ok &= domain("def eta : (p : Sigma (n : Nat) (Path Nat n n)) -> Path (Sigma (n : Nat) (Path Nat n n)) (pack (witness p) (payload p)) p := fun p => refl", NULL);
  /* A cofamily is a cocell signature (S4, rule R1): one type parameter, one
     clause, exactly one observation `A`, and cofields `D A` or `E -> D A`
     with E closed. */
  ok &= domain("family Size := sz0 | szS (szPrev : Size)\n"
               "cofamily Strm (A : Type 0) := strmCorec (strmHead : A) (strmTail : Strm A)", NULL);
  ok &= domain("cofamily Tree (A : Type 0) := treeCorec (treeLabel : A) (treeLeft : Tree A) (treeRight : Tree A)", NULL);
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Br (A : Type 0) := brCorec (brHere : A) (brNext : Lab -> Br A)", NULL);
  ok &= domain("cofamily Ident (A : Type 0) := identCorec (identOut : A)", NULL);
  /* Closed labels are judged after normalization. Erased references to
     the parameter, self and earlier observations need no local context. */
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Br (A : Type 0) := brCorec (brHere : A) "
               "(brNext : ((fun (x : A) => Lab) brHere) -> Br A)\n"
               "def branch : Br Lab := brCorec Lab Lab (fun (s : Lab) => s) (fun (s : Lab) (e : Lab) => e) l0\n"
               "def branchLaw : Path Lab (brHere Lab (brNext Lab branch l1)) l1 := refl", NULL);
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Br (A : Type 0) := brCorec (brHere : A) "
               "(brNext : ((fun (T : Type 0) => Lab) (Br A)) -> Br A)", NULL);
  ok &= domain("cofamily Hi (A : Type 1) := hiCorec (hiOut : A) "
               "(hiType : Type 0 -> Hi A) (hiNat : Nat -> Hi A)", NULL);
  ok &= domain("cofamily Dep (A : Type 0) := depCorec (depOut : A) "
               "(depNext : Path A depOut depOut -> Dep A)", "REFUSE_CELL");
  ok &= domain("def U : Type 1 := Type 0\n"
               "cofamily AliasCo (A : U) := aliasCoCorec (aliasCoOut : A) (aliasCoNext : AliasCo A)", NULL);
  ok &= domain("cofamily Np := npCorec (npOut : Nat)", "REFUSE_CELL");
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Vp (n : Lab) := vpCorec (vpOut : Lab)", "REFUSE_CELL");
  ok &= domain("cofamily Tp (A : Type 0) (B : Type 0) := tpCorec (tpOut : A)", "REFUSE_CELL");
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Ix (A : Type 0) : Lab -> Type 0 := ixCorec (ixOut : A) : Ix A l0", "REFUSE_CELL");
  ok &= domain("cofamily Ab (A : Type 0) := abCorec (abOut : A) and Other := other", "REFUSE_CELL");
  ok &= domain("cofamily No (A : Type 0) := noCorec (noNext : No A)", "REFUSE_CELL");
  ok &= domain("cofamily Two (A : Type 0) := twoCorec (twoA : A) (twoB : A)", "REFUSE_CELL");
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Tg (A : Type 0) := tgCorec (tgOut : A) (tgTag : Lab)", "REFUSE_CELL");
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Dl (A : Type 0) := dlCorec (dlOut : A) (dlNext : Dl Lab)", "REFUSE_CELL");
  ok &= domain("cofamily Dd (A : Type 0) := ddCorec (ddOut : A) (ddNext : Dd (Dd A))", "REFUSE_CELL");
  ok &= domain("cofamily Ad (A : Type 0) := adCorec (adOut : A) (adNext : A -> Ad A)", "REFUSE_CELL");
  ok &= domain("cofamily Pf (A : Type 0) := pfCorec (pfOut : A) (pfT : Pf A) (pfP : Path (Pf A) pfT pfT)", "REFUSE_CELL");
  ok &= domain("cofamily Cf (A : Type 0) := cfCorec (cfOut : A) (cfNext : Cf A -> List (Cf A))", "REFUSE_CELL");
  ok &= domain("cofamily Lu (A : Type 0) := luCorec (luOut : A) (luNext : Type 0 -> Lu A)", "REFUSE_CELL");
  /* `sealed family` and `law def` are domain-only modifiers (S5, rule O7).
     A sealed family is a cell monad; a program cannot use either. */
  ok &= domain("sealed family Sl (A : Type 0) := slEta (slOf : A)", NULL);
  ok &= domain("sealed family Sl (A : Type 0) := slEta (slOf : A)\n"
               "law def slOut : (A : Type 0) -> Sl A -> A := fun A c => fold (fun a => a) c", NULL);
  ok &= domain("sealed family Sc := sc0 | sc1", "REFUSE_CELL");
  ok &= guest("sealed family Sl (A : Type 0) := slEta (slOf : A)", "REFUSE_DATA");
  ok &= guest("law def lawId : Type 1 := Type 0", "REFUSE_DATA");
  /* Law references in types and annotations must survive normalization and
     erasure, including when the annotated variable is unused. */
  ok &= law_guest("def erased : Enc Size -> Size := fun (c : Enc Size) =>\n"
                  "  (fun (f : Path Size (decrypt Size c) (decrypt Size c) -> Size) => sz0) (fun p => sz0)", "REFUSE_LAW");
  ok &= law_guest("def erasedType : Path Size (decrypt Size (encEta Size sz0)) sz0 -> Size := fun p => sz0", "REFUSE_LAW");
  ok &= law_guest("def lawBody : (c : Enc Size) -> Path Size sz0 sz0 :=\n"
                  "  fun c => fun (i : I) => (fun (x : Size) => sz0) (decrypt Size c)\n"
                  "def erasedRelay : Path Size (lawBody (encEta Size sz0) @ i0) sz0 -> Size := fun p => sz0", "REFUSE_LAW");
  ok &= law_guest("def checkedAnn : Size :=\n"
                  "  (fun (f : Path Size sz0 sz0 -> Size) => sz0)\n"
                  "    (fun (p : Path Size (decrypt Size (encEta Size sz0)) sz0) => sz0)", "REFUSE_LAW");
  ok &= law_guest("def truncatedAnn : Enc Size -> Size := fun (c : Enc Size) =>\n"
                  "  (fun (f : Trunc1 (Path Size (decrypt Size c) (decrypt Size c)) -> Size) => sz0) (fun p => sz0)", NULL);
  ok &= law_guest("def shadowed : (decrypt : Size) -> Size := fun decrypt => decrypt", NULL);
  /* open is a law def (S6, rule O7): a guest use outside a law type is
     refused, a use in a Trunc1 argument is not law-only, and the fields of
     a Quorum are public data. */
  ok &= law_guest("def peekOpen : Quorum -> Enc Size -> Size := fun q c => open Size q c", "REFUSE_LAW");
  ok &= law_guest("def settleOpen : (q : Quorum) -> (c : Enc Size) -> Trunc1 (Path Size (open Size q c) sz0) -> Size :=\n"
                  "  fun q c p => sz0", NULL);
  ok &= law_guest("def quorumOf : Quorum -> Size := fun q => quorumT q", NULL);
  /* ZKProof, Transcript and verified (S7, rule O7): a decrypt in the
     argument of ZKProof is not law-only, and a program def cannot take the
     name of the law def verified or of the family Transcript. */
  ok &= law_guest("def openedZK : (c : Enc Size) -> ZKProof (Path Size (decrypt Size c) sz0) -> Size :=\n"
                  "  fun c p => sz0", NULL);
  ok &= law_guest("def verified : Size := sz0", "REFUSE_NAME");
  ok &= law_guest("def Transcript : Size := sz0", "REFUSE_NAME");
  /* Part S (S8, Q-S8-4 (a)): meaning has the codomain Type 0 and decrypts
     only in the argument of ZKProof, so it is not law-only. */
  ok &= law_guest("def meaningAt : (c : Enc Size) -> (d : Enc Size) -> Type 0 :=\n"
                  "  fun c d => ZKProof (Path Size (decrypt Size c) (decrypt Size d))", NULL);
  /* Fill is the two openings and meaning: decrypt only under ZKProof. */
  ok &= law_guest("def FillAt : (c : Enc Size) -> (d : Enc Size) -> (a o : Size) -> Type 0 :=\n"
                  "  fun c d a o => Prod (ZKProof (Path Size (decrypt Size c) a)) (ZKProof (Path Size (decrypt Size d) o))", NULL);
  /* The generated names are R4 names: a bisimulation name, a destructor
     name and a corecursor name that an earlier def holds are refused. */
  ok &= domain("def StrmBisim : Type 1 := Type 0\n"
               "cofamily Strm (A : Type 0) := strmCorec (strmHead : A) (strmTail : Strm A)", "REFUSE_NAME");
  ok &= domain("def strmTail : Type 1 := Type 0\n"
               "cofamily Strm (A : Type 0) := strmCorec (strmHead : A) (strmTail : Strm A)", "REFUSE_NAME");
  ok &= domain("def strmCorec : Type 1 := Type 0\n"
               "cofamily Strm (A : Type 0) := strmCorec (strmHead : A) (strmTail : Strm A)", "REFUSE_NAME");
  /* The elaborated destructors and corecursor compute by conversion: the
     head of the corecursor is the observation of the seed, and its tail is
     the corecursor at the next seed. */
  ok &= domain("family Size := sz0 | szS (szPrev : Size)\n"
               "cofamily Strm (A : Type 0) := strmCorec (strmHead : A) (strmTail : Strm A)\n"
               "def nats : Strm Size := strmCorec Size Size (fun (s : Size) => s) (fun (s : Size) => szS s) sz0\n"
               "def headLaw : Path Size (strmHead Size nats) sz0 := refl\n"
               "def tailLaw : Path (Strm Size) (strmTail Size nats) (strmCorec Size Size (fun (s : Size) => s) (fun (s : Size) => szS s) (szS sz0)) := refl", NULL);
  ok &= domain("family Lab := l0 | l1\n"
               "cofamily Br (A : Type 0) := brCorec (brHere : A) (brNext : Lab -> Br A)\n"
               "def brStep : Lab -> Lab -> Lab := fun (s : Lab) (e : Lab) => e\n"
               "def brAll : Br Lab := brCorec Lab Lab (fun (s : Lab) => s) brStep l0\n"
               "def brLaw : Path Lab (brHere Lab (brNext Lab brAll l1)) l1 := refl", NULL);
  ok &= normal_roundtrip("Nat -> (A : Type 0) -> A -> A",
    "fun (i0 : Nat) (A : Type 0) (a : A) => transp (fun (i : I) => A) (~i1) a");
  /* Guest mode (orbit-lang S3): a program with no kit name checks, and a Nat
     literal is a kit item. The second row passes in kit mode only if the mode
     is wrong. */
  ok &= guest("def keep : Sum Unit Unit -> Sum Unit Unit := fun c => c", NULL);
  ok &= guest("def three : Nat := 3", "REFUSE_KIT");
  /* S12 (Q-S12-12): the json quote escape and the fix reset of diag_vfail. */
  ok &= json_quote();
  ok &= diag_fix_reset();
  ok &= signature_fixes();
  if (ok) puts("domain regressions: passed");
  return ok ? 0 : 1;
}
