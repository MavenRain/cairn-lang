#include <string.h>

#include "front/contract.h"
#include "front/front.h"

static int load_one(Arena *arena, const char *source, Origin origin, const char *text, size_t len, DeclList *out, Diag *diag) {
  TokenList toks;
  if (!lex_source(arena, source, text, len, &toks, diag)) return 0;
  return parse_source(arena, source, origin, &toks, out, diag);
}

/* Orbit as a kernel family (orbit-lang S4, PLAN section 8; intent-lang S3,
   design C 2a): the set quotient of the voter assignments by act, with no
   parameter. orbitEq is a path from orbitProj a to orbitProj b for each g
   with act g b = a, and orbitSquash has the squash0 shape, so the
   orbitSquash fold case has the type isSet RES. */
#define ORBIT_SOURCE_NAME "kernel/orbit.lang"

static const char orbit_source[] =
  "family Orbit := orbitProj (orbitOf : Member -> Choice)\n"
  "  | orbitEq (orbitA : Member -> Choice) (orbitB : Member -> Choice) (orbitR : Sigma (g : Group) (Path (Member -> Choice) (act g orbitB) orbitA)) (i : I)\n"
  "    [ ~i -> orbitProj orbitA, i -> orbitProj orbitB ]\n"
  "  | orbitSquash (orbitFrom : Orbit) (orbitTo : Orbit) (orbitLeft : Path Orbit orbitFrom orbitTo) (orbitRight : Path Orbit orbitFrom orbitTo) (i : I) (j : I)\n"
  "    [ ~i -> orbitLeft @ j, i -> orbitRight @ j, ~j -> orbitFrom, j -> orbitTo ]\n";

/* The DAO prelude as kernel declarations (orbit-lang S7, PLAN section 8):
   the defined names of the design signature after Orbit. aggregate is the
   Orbit fold of the rule of a constitution, with the outcomeSet slot as its
   orbitSquash case, so the prelude needs the slot. */
#define DAO_SOURCE_NAME "kernel/dao.lang"

static const char dao_source[] =
  "def Config : Type 0 := Member -> Choice\n"
  "def Constitution : Type 0 := Sigma (F : Config -> Outcome) ((g : Group) -> (c : Config) -> Path Outcome (F (act g c)) (F c))\n"
  "def Aggregation : Type 0 := Orbit -> Outcome\n"
  "def Tally : Type 0 := Orbit\n"
  "def aggregate : Constitution -> Aggregation := fun (F : Constitution) (z : Orbit) => fold (witness F)\n"
  "  (fun (a : Config) (b : Config) (r : Sigma (g : Group) (Path Config (act g b) a)) =>\n"
  "    trans Outcome (witness F a) (witness F (act (witness r) b)) (witness F b) (sym (cong (witness F) (payload r))) (payload F (witness r) b))\n"
  "  outcomeSet z\n"
  "def gov : Aggregation -> Constitution := fun (L : Aggregation) => pack (fun (c : Config) => L (orbitProj c))\n"
  "  (fun (g : Group) (c : Config) => fun (i : I) => L (orbitEq (act g c) c (pack g refl) i))\n"
  "def amend : Aggregation -> Aggregation := fun (L : Aggregation) => aggregate (gov L)\n";

/* The ZK surface as kernel declarations (orbit-lang S8 B2b, PLAN
   "Carriers"): the primitive names of the design signature over the
   internal carriers ZkBallot and ZkWitness, after Member, Choice and the
   DAO prelude (Tally present means the prelude is in). The member and the
   choice go to the host by constructor index, the poll id is 0, and
   verifyZK leaves t unread in S8. */
#define ZK_SOURCE_NAME "kernel/zk.lang"

static const char zk_source[] =
  "def Ballot_priv : Type 0 := ZkBallot Choice\n"
  "def ZKWitness : Type 0 := ZkWitness\n"
  "def castPrivate : Member -> Choice -> Ballot_priv := fun (m : Member) (c : Choice) => zkCast Member Choice m c\n"
  "def proveZK : Ballot_priv -> ZKWitness := fun (b : Ballot_priv) => zkProve b\n"
  "def project : Ballot_priv -> Choice := fun (b : Ballot_priv) => zkProject Choice b\n"
  "def verifyZK : ZKWitness -> Choice -> Tally -> Type 0 := fun (w : ZKWitness) (c : Choice) (t : Tally) => zkCheck Member Choice w c\n";

/* The first definition or family named NAME in OUT from FIRST on, or
   OUT->count. */
static size_t def_at(const DeclList *out, size_t first, const char *name) {
  size_t at;
  for (at = first; at < out->count && ((out->items[at].kind != DECL_DEF && out->items[at].kind != DECL_FAMILY) || strcmp(out->items[at].name, name) != 0); at++)
    ;
  return at;
}

/* When OUT from FIRST on defines each of the COUNT names AFTER, put the
   kernel declarations of TEXT directly after the last of them, with the
   trusted origin ORIGIN_DOMAIN. Check order is declaration order, so a
   refused slot stops before them. */
static int splice_after(Arena *arena, size_t first, const char *const *after, size_t count, const char *source, const char *text, size_t len, DeclList *out, Diag *diag) {
  DeclList kernel = {NULL, 0, 0};
  Decl *items;
  size_t at = first;
  size_t i;
  for (i = 0; i < count; i++) {
    size_t k = def_at(out, first, after[i]);
    if (k == out->count)
      return 1;
    at = k > at ? k : at;
  }
  if (!load_one(arena, source, ORIGIN_DOMAIN, text, len, &kernel, diag))
    return 0;
  items = arena_alloc(arena, (out->count + kernel.count) * sizeof *items);
  if (items == NULL)
    return 0;
  memcpy(items, out->items, (at + 1u) * sizeof *items);
  memcpy(items + at + 1u, kernel.items, kernel.count * sizeof *items);
  memcpy(items + at + 1u + kernel.count, out->items + at + 1u, (out->count - at - 1u) * sizeof *items);
  out->items = items;
  out->count += kernel.count;
  out->cap = out->count;
  return 1;
}

static const char *const ORBIT_AFTER[] = {"act"};
static const char *const DAO_AFTER[] = {"Orbit", "outcomeSet"};
static const char *const ZK_AFTER[] = {"Member", "Choice", "Tally"};

/* The family Orbit goes directly after act. The DAO prelude goes after the
   last of Orbit (so after act) and outcomeSet. The ZK surface goes
   after the last of Member, Choice and Tally (so after the DAO prelude). */
static int orbit_after_act(Arena *arena, size_t first, DeclList *out, Diag *diag) {
  return splice_after(arena, first, ORBIT_AFTER, 1u, ORBIT_SOURCE_NAME, orbit_source, sizeof orbit_source - 1u, out, diag)
      && splice_after(arena, first, DAO_AFTER, 2u, DAO_SOURCE_NAME, dao_source, sizeof dao_source - 1u, out, diag)
      && splice_after(arena, first, ZK_AFTER, 3u, ZK_SOURCE_NAME, zk_source, sizeof zk_source - 1u, out, diag);
}

int front_load(Arena *arena, const char *source, const char *text, size_t len, DeclList *out, Diag *diag) {
  size_t first;
  out->items = NULL;
  out->count = 0;
  out->cap = 0;
  if (!load_one(arena, "domain/domain.lang", ORIGIN_DOMAIN, (const char *)domain_source, domain_source_len, out, diag)
      || !load_one(arena, CONTRACT_SOURCE_NAME, ORIGIN_KIT, contract_source, contract_source_len, out, diag))
    return 0;
  first = out->count;
  return load_one(arena, source, ORIGIN_PROGRAM, text, len, out, diag)
      && orbit_after_act(arena, first, out, diag);
}

int front_load_domain(Arena *arena, const char *source, const char *domain, const char *text, size_t len, DeclList *out, Diag *diag) {
  size_t first;
  out->items = NULL;
  out->count = 0;
  out->cap = 0;
  if (!load_one(arena, "domain/domain.lang", ORIGIN_DOMAIN, (const char *)domain_source, domain_source_len, out, diag)
      || !load_one(arena, CONTRACT_SOURCE_NAME, ORIGIN_KIT, contract_source, contract_source_len, out, diag)
      || !load_one(arena, source, ORIGIN_DOMAIN, domain, strlen(domain), out, diag))
    return 0;
  first = out->count;
  return load_one(arena, source, ORIGIN_PROGRAM, text, len, out, diag)
      && orbit_after_act(arena, first, out, diag);
}
