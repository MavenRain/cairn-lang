# `langc check --json`

`langc check --json` checks a program as `langc check` does. It writes one
JSON line on stdout. A tool reads this line in place of `ok` or the text
diagnostic on stderr. S12 adds it (PLAN row A2, rulings Q-S12-1 to
Q-S12-12).

## Usage

```sh
build/langc check --json PROG
build/langc --kit check --json PROG
```

| Exit | stdout | stderr |
|---|---|---|
| 0 | the JSON line, with an empty diagnostic list | nothing |
| 1 | the JSON line, with one diagnostic | nothing |
| 2 | nothing | the `OOM` diagnostic |

## The line

The line is one JSON object and a newline. The keys come in this order:

```
{"diagnostics":[DIAG],"progress":{"parsed":P,"typed":T,"laws_discharged":D,"laws_total":L}}
```

When `langc` accepts the program, `diagnostics` is empty. Else it holds
one object, the first failure. An example (`test/json/REFUSE_SIG.json`):

```json
{"code":"REFUSE_SIG","span":{"file":"test/json/REFUSE_SIG.lang","line":1,"col":1,"end_line":1,"end_col":31},"message":"Outcome: the slot Outcome must have the type Type 0","fix":{"span":{"file":"test/json/REFUSE_SIG.lang","line":1,"col":15,"end_line":1,"end_col":21},"text":"Type 0"}}
```

- `code`: the refusal code. The `REFUSE_` codes are in
  `docs/host/README.md` (section Source language).
- `span`: the file and the source text that fails. The file is the path
  as given on the command line. Lines and columns start at 1, one byte
  is one column, and the end column is exclusive.
- `message`: the text of the diagnostic.
- `fix`: `null`, or one edit: put `fix.text` in place of the text of
  `fix.span`. The edit removes this failure. Another failure can come
  next. Some `LEX_` and `PARSE_` diagnostics and the `REFUSE_CELL`,
  `REFUSE_SIG` and `REFUSE_STATE` diagnostics have a fix.

Each string is JSON: `"`, `\`, newline and tab get a short escape, other
control bytes get `\u00XX`, an invalid UTF-8 byte gets `�`, and
valid UTF-8 stays as it is (`src/front/json.h`).

## Progress

| Key | What it counts |
|---|---|
| `parsed` | the program decls that parse (a block counts each sort) |
| `typed` | the program decls that check before the first failure |
| `laws_discharged` | the typed program defs that are laws only (`law_only`) |
| `laws_total` | `laws_discharged`, plus the program defs that did not check and that a syntax test of rule O7 marks as laws (`LawScan` in `src/front/check.c`) |

Only program decls count. Domain decls, kit decls and the spliced orbit,
dao and zk sources do not count. After a `LEX_` or a `PARSE_` failure,
the 4 counts are 0.

## FIXFILE

`build/fronttool fix PROG FIXFILE` applies one fix to PROG and writes the
result on stdout. FIXFILE has this form:

- line 1: `LINE COL END_LINE END_COL`, the span of the fix. Columns start
  at 1, one byte is one column, and the end is exclusive.
- the remaining text of the file: the new text, less one final newline.

The new text can be empty or end in a space. Thus `.gitattributes` turns
off the git whitespace check for `test/json/*.fix`.

An example, `test/json/REFUSE_SIG.fix`:

```
1 15 1 21
Type 0
```

The row `test/json/REFUSE_SIG.lang` is `def Outcome : Type 1 := Type 0`.
The fix puts `Type 0` in place of `Type 1` (line 1, columns 15 to 20).

## The test rows

The `json rows` block of `test/gate.sh` runs each `test/json/*.lang` file
as a row:

- `CODE.lang`: a row for the code CODE. `ok.lang` and `kit.lang` are
  accepted rows. `langc` refuses each other row, and its first code is
  CODE.
- `CODE.TAG.lang`: one more row for CODE. The code is the file name up to
  the first dot.
- `X.json`: the expected line of the row X. `langc check --json` must give
  these bytes and write nothing on stderr.
- `X.fix`: the FIXFILE of each refused row X. After `fronttool fix`,
  `langc check --json` must exit 0 or give a first code other than CODE.
- The rows `kit`, `REFUSE_HISTORY_WRITE`, `REFUSE_MAP` and `REFUSE_STATE`
  run with `--kit`.
- Each `REFUSE_` code of `src/front/check.c` must have a row `CODE.lang`.
- Each row must be `langc fmt` canonical (`build/langc fmt X.lang | cmp -
  X.lang`). The `LEX_` and `PARSE_` rows are not in this check, because
  `langc fmt` refuses them. The gate line `fmt canonical` counts the
  other rows.

To add a row X:

1. Write `test/json/X.lang`, and make sure that it is canonical.
2. From the repo root, run `build/langc [--kit] check --json
   test/json/X.lang > test/json/X.json`. The span file is the path as
   given, thus use this relative path.
3. Read the line. Count the progress values by hand.
4. For a refused row, write `test/json/X.fix`.

At S12 the gate prints `json rows: 20 passed`, `json refusal codes: 13 of
13` and `json fixes: 18 passed`.
