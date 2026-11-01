# Builds the cairn-lang kernel port (the intent-lang kernel at 67445ae) on the
# tcc-wasm host with TinyCC. See README.md. TARGET (src/front/lower.c and
# src/wasm.c, on src/ir.h) is the Wasm target (S8), and src/js.c the JS
# writer (S9); the tools leave it out.
TCC ?= tcc
CC ?= cc
CFLAGS = -std=c99 -Wall -Werror -Isrc
CLANG_FLAGS = -std=c99 -Wall -Wextra -Wswitch-enum -Werror -fsyntax-only -Isrc
FRONT = src/front/base.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/frost.c src/front/lexer.c src/front/parser.c src/front/front.c src/front/eval.c src/front/check.c src/front/fmt.c src/sha256.c
TARGET = src/front/lower.c src/wasm.c src/js.c src/wgsl.c
SRC = src/main.c src/front/json.c src/front/jsonin.c src/front/codes.c src/mcp.c $(FRONT) $(TARGET)
FRONT_TOOL = test/fronttool.c $(filter-out src/front/front.c,$(FRONT)) src/front/json.c
BLSAG_TOOL = test/blsagtool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/sha256.c
ELGAMAL_TOOL = test/elgamaltool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/sha256.c
THRESHOLD_TOOL = test/thresholdtool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/sha256.c
DKG_TOOL = test/dkgtool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/dkg.c src/sha256.c
FROST_TOOL = test/frosttool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/dkg.c src/front/frost.c src/sha256.c
SIGMA_TOOL = test/sigmatool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/sigma.c src/sha256.c
BALLOT_TOOL = test/ballottool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/sigma.c src/sha256.c
SECP_TOOL = test/secptool.c src/front/u256.c src/front/field.c src/front/secp256k1.c
SHA_TOOL = test/shatool.c src/sha256.c
# The Wasm runner (Q-S8-7) links the wasmtime C API. A missing file fails the
# build of build/wasmtool, so the gate fails; nothing is downloaded.
WASMTIME ?= /opt/homebrew
WASM_TOOL = test/wasmtool.c src/front/u256.c
JS_TOOL = test/jstool.c src/front/u256.c
WGSL_TOOL = test/wgsltool.c src/front/u256.c
WASMTIME_FILES = $(WASMTIME)/include/wasmtime.h $(WASMTIME)/lib/libwasmtime.dylib
PATH_TOOL = test/pathtool.c $(filter-out src/front/front.c,$(FRONT))
RUN_TOOL = test/runtool.c $(FRONT)
MCP_TOOL = test/mcptool.c
SKILL_TOOL = test/skilltool.c
TOK_TOOL = test/tokcount.c src/sha256.c
HEADERS = $(wildcard src/*.h src/front/*.h)

.PHONY: build check check-clang clean gpu-check tokens

build: build/langc build/fronttool build/blsagtool build/elgamaltool build/thresholdtool build/dkgtool build/sigmatool build/frosttool build/ballottool build/secptool build/shatool build/pathtool build/runtool build/wasmtool build/jstool build/wgsltool build/mcptool build/skilltool build/tokcount

build/domain.c: domain/domain.lang gen/embed.c
	mkdir -p build
	$(TCC) -run gen/embed.c domain/domain.lang build/domain.c

build/langc: $(SRC) $(HEADERS) build/domain.c
	$(TCC) $(CFLAGS) -o build/langc $(SRC) build/domain.c

build/fronttool: $(FRONT_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/fronttool $(FRONT_TOOL)

build/blsagtool: $(BLSAG_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/blsagtool $(BLSAG_TOOL)

build/elgamaltool: $(ELGAMAL_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/elgamaltool $(ELGAMAL_TOOL)

build/thresholdtool: $(THRESHOLD_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/thresholdtool $(THRESHOLD_TOOL)

build/dkgtool: $(DKG_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/dkgtool $(DKG_TOOL)

build/sigmatool: $(SIGMA_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/sigmatool $(SIGMA_TOOL)

build/frosttool: $(FROST_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/frosttool $(FROST_TOOL)

build/ballottool: $(BALLOT_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/ballottool $(BALLOT_TOOL)

build/secptool: $(SECP_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/secptool $(SECP_TOOL)

build/shatool: $(SHA_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/shatool $(SHA_TOOL)

build/pathtool: $(PATH_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/pathtool $(PATH_TOOL)

build/runtool: $(RUN_TOOL) $(HEADERS) build/domain.c
	$(TCC) $(CFLAGS) -o build/runtool $(RUN_TOOL) build/domain.c

build/wasmtool: $(WASM_TOOL) $(HEADERS) $(WASMTIME_FILES)
	mkdir -p build
	$(TCC) $(CFLAGS) -I$(WASMTIME)/include -o build/wasmtool $(WASM_TOOL) $(WASMTIME)/lib/libwasmtime.dylib

build/jstool: $(JS_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/jstool $(JS_TOOL)

build/wgsltool: $(WGSL_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/wgsltool $(WGSL_TOOL)

build/jstest: test/jstest.c
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/jstest test/jstest.c

build/mcptool: $(MCP_TOOL)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/mcptool $(MCP_TOOL)

build/skilltool: $(SKILL_TOOL)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/skilltool $(SKILL_TOOL)

build/tokcount: $(TOK_TOOL) $(HEADERS)
	mkdir -p build
	$(TCC) $(CFLAGS) -o build/tokcount $(TOK_TOOL)

check-clang: build/domain.c
	$(CC) $(CLANG_FLAGS) $(SRC) build/domain.c
	$(CC) $(CLANG_FLAGS) $(FRONT_TOOL)
	$(CC) $(CLANG_FLAGS) $(BLSAG_TOOL)
	$(CC) $(CLANG_FLAGS) $(ELGAMAL_TOOL)
	$(CC) $(CLANG_FLAGS) $(THRESHOLD_TOOL)
	$(CC) $(CLANG_FLAGS) $(DKG_TOOL)
	$(CC) $(CLANG_FLAGS) $(SIGMA_TOOL)
	$(CC) $(CLANG_FLAGS) $(FROST_TOOL)
	$(CC) $(CLANG_FLAGS) $(BALLOT_TOOL)
	$(CC) $(CLANG_FLAGS) $(SECP_TOOL)
	$(CC) $(CLANG_FLAGS) $(SHA_TOOL)
	$(CC) $(CLANG_FLAGS) $(PATH_TOOL)
	$(CC) $(CLANG_FLAGS) $(RUN_TOOL) build/domain.c
	$(CC) $(CLANG_FLAGS) -isystem $(WASMTIME)/include $(WASM_TOOL)
	$(CC) $(CLANG_FLAGS) $(JS_TOOL)
	$(CC) $(CLANG_FLAGS) $(WGSL_TOOL)
	$(CC) $(CLANG_FLAGS) test/jstest.c
	$(CC) $(CLANG_FLAGS) $(MCP_TOOL)
	$(CC) $(CLANG_FLAGS) $(SKILL_TOOL)
	$(CC) $(CLANG_FLAGS) $(TOK_TOOL)

check: build/langc build/fronttool build/blsagtool build/elgamaltool build/thresholdtool build/dkgtool build/sigmatool build/frosttool build/ballottool build/secptool build/shatool build/pathtool build/runtool build/wasmtool build/jstool build/wgsltool build/mcptool build/skilltool build/tokcount build/jstest check-clang
	sh test/gate.sh
	sh test/run.sh
	sh test/review.sh
	build/fronttool
	build/blsagtool
	build/elgamaltool
	build/thresholdtool
	build/dkgtool
	build/sigmatool
	build/frosttool
	build/ballottool
	build/secptool
	build/shatool
	build/pathtool
	build/runtool
	build/langc --kit build test/wasm/hash.lang -o build/wasm-hash.wasm
	build/langc --kit abi test/wasm/hash.lang --text > build/wasm-hash.abi
	awk -F ' => ' '/^[^#]/ {print $$1}' test/wasm/hash.txt | build/wasmtool build/wasm-hash.wasm build/wasm-hash.abi > build/wasm-hash.out
	awk -F ' => ' '/^[^#]/ {print $$2}' test/wasm/hash.txt | cmp -s - build/wasm-hash.out && echo "wasm regressions: passed" || { echo "FAIL wasm regressions: build/wasm-hash.out is not the RESULT text of test/wasm/hash.txt"; exit 1; }
	build/langc --kit build test/wasm/secp.lang -o build/wasm-secp.wasm
	build/langc --kit abi test/wasm/secp.lang --text > build/wasm-secp.abi
	awk -F ' => ' '/^[^#]/ {print $$1}' test/wasm/secp.txt | build/wasmtool build/wasm-secp.wasm build/wasm-secp.abi > build/wasm-secp.out
	awk -F ' => ' '/^[^#]/ {print $$2}' test/wasm/secp.txt | cmp -s - build/wasm-secp.out && echo "wasm regressions: passed" || { echo "FAIL wasm regressions: build/wasm-secp.out is not the RESULT text of test/wasm/secp.txt"; exit 1; }
	build/langc --kit build test/wasm/traps.lang -o build/wasm-traps.wasm
	build/langc --kit abi test/wasm/traps.lang --text > build/wasm-traps.abi
	awk -F ' => ' '/^[^#]/ {print $$1}' test/wasm/traps.txt | build/wasmtool build/wasm-traps.wasm build/wasm-traps.abi > build/wasm-traps.out
	awk -F ' => ' '/^[^#]/ {print $$2}' test/wasm/traps.txt | cmp -s - build/wasm-traps.out || { echo "FAIL trap regressions: Wasm output differs"; exit 1; }
	awk -F ' => ' '/^[^#]/ {print $$1}' test/wasm/traps.txt | while IFS= read -r line; do set -- $$line; build/langc --kit eval test/wasm/traps.lang "$$@" 2>/dev/null || :; done > build/eval-traps.out
	awk -F ' => ' '/^[^#]/ {print $$2}' test/wasm/traps.txt | cmp -s - build/eval-traps.out && echo "trap regressions: passed" || { echo "FAIL trap regressions: evaluator output differs"; exit 1; }
	build/langc --kit build test/wasm/hash.lang --js build/js-hash.mjs
	awk -F ' => ' '/^[^#]/ {print $$1}' test/wasm/hash.txt | build/jstool build/js-hash.mjs build/wasm-hash.abi > build/js-hash.out
	awk -F ' => ' '/^[^#]/ {print $$2}' test/wasm/hash.txt | cmp -s - build/js-hash.out && echo "js regressions: passed" || { echo "FAIL js regressions: build/js-hash.out is not the RESULT text of test/wasm/hash.txt"; exit 1; }
	build/langc --kit build test/wasm/secp.lang --js build/js-secp.mjs
	awk -F ' => ' '/^[^#]/ {print $$1}' test/wasm/secp.txt | build/jstool build/js-secp.mjs build/wasm-secp.abi > build/js-secp.out
	awk -F ' => ' '/^[^#]/ {print $$2}' test/wasm/secp.txt | cmp -s - build/js-secp.out && echo "js regressions: passed" || { echo "FAIL js regressions: build/js-secp.out is not the RESULT text of test/wasm/secp.txt"; exit 1; }
	build/langc --kit build test/wasm/traps.lang --js build/js-traps.mjs
	awk -F ' => ' '/^[^#]/ {print $$1}' test/wasm/traps.txt | build/jstool build/js-traps.mjs build/wasm-traps.abi > build/js-traps.out
	awk -F ' => ' '/^[^#]/ {print $$2}' test/wasm/traps.txt | cmp -s - build/js-traps.out && echo "js regressions: passed" || { echo "FAIL js regressions: build/js-traps.out is not the RESULT text of test/wasm/traps.txt"; exit 1; }
	build/langc --kit build test/js/names.lang --js 'build/js-names #?%.mjs'
	build/langc --kit abi test/js/names.lang --text > build/js-names.abi
	build/jstest
	build/langc --kit abi test/ir/transp.lang --text > build/transp.abi
	printf 'moved (nat) -> nat\n' | cmp -s - build/transp.abi || { echo "FAIL ABI: checked signatures must be available independently of lowering"; exit 1; }
	build/tokcount --check docs/TOKEN-BASELINE.md
	build/tokcount --estimate > build/tokens-estimate.md
	awk '/^<!-- tokcount end -->/ {p = 0} p; /^<!-- tokcount begin -->/ {p = 1}' docs/TOKEN-BASELINE.md | cmp -s - build/tokens-estimate.md && echo "token estimate: passed" || { echo "FAIL tokens: the estimate block of docs/TOKEN-BASELINE.md is not the build/tokcount --estimate output"; exit 1; }

# S15 (A6, Q-S15-6 (a)): the counts of docs/TOKEN-BASELINE.md. Run it outside
# the sandbox with ANTHROPIC_API_KEY set: it builds the Rust crate and calls
# the Anthropic API. It is not in check (Q-S0-8 (a)); check runs the offline
# `build/tokcount --check` (Q-S15-7 (a)) and compares the estimate block of the
# report with the `build/tokcount --estimate` output (Q-S15-10 (c)). A
# `make tokens` run that replaces the estimate block must also remove that
# compare.
TOKEN_API = https://api.anthropic.com/v1
TOKEN_HEADERS = -H "x-api-key: $$ANTHROPIC_API_KEY" -H "anthropic-version: 2023-06-01"
TOKEN_PROGRAMS = g1-record g2-committee g3-vote g4-amend g5-self-amend
TOKEN_EVALS = g1-record:verifyHit g2-committee:appGood g2-committee:appBad g3-vote:tallyThree g4-amend:g4Append g4-amend:g4Refused g5-self-amend:g5Open g5-self-amend:g5Refused
TOKEN_FILES = $(foreach g,$(TOKEN_PROGRAMS),examples/skill/$(g).lang test/baseline/$(g).py test/baseline/rust/src/bin/$(firstword $(subst -, ,$(g))).rs)
RUST_BUILD ?= cargo build -j 2

tokens: build/langc build/tokcount
	@test -n "$$ANTHROPIC_API_KEY" || { echo "FAIL tokens: ANTHROPIC_API_KEY is not set"; exit 1; }
	rm -rf build/tokens
	mkdir -p build/tokens
	{ date -u +%Y-%m-%dT%H:%M:%SZ; python3 --version; rustc --version; cargo --version; } > build/tokens/versions.txt
	cd test/baseline/rust && $(RUST_BUILD)
	for row in $(TOKEN_EVALS); do g=$${row%%:*}; kit=; test $$g = g2-committee && kit=--kit; build/langc $$kit eval examples/skill/$$g.lang $${row#*:} >> build/tokens/$$g.want || exit 1; done
	for g in $(TOKEN_PROGRAMS); do python3 -I test/baseline/$$g.py > build/tokens/$$g.py.out && cmp -s build/tokens/$$g.want build/tokens/$$g.py.out || { echo "FAIL tokens: test/baseline/$$g.py failed or its output is not the langc eval output"; exit 1; }; test/baseline/rust/target/debug/$${g%%-*} > build/tokens/$$g.rs.out && cmp -s build/tokens/$$g.want build/tokens/$$g.rs.out || { echo "FAIL tokens: the Rust bin $${g%%-*} failed or its output is not the langc eval output"; exit 1; }; done
	@echo "token outputs: 10 of 10"
	curl -sS --fail-with-body $(TOKEN_HEADERS) "$(TOKEN_API)/models?limit=1000" -o build/tokens/models.json
	build/tokcount --model build/tokens/models.json > build/tokens/model.txt
	model=$$(cut -d ' ' -f 1 build/tokens/model.txt); for f in frame $(TOKEN_FILES); do n=$${f##*/}; src=$$f; test $$f = frame && src=; build/tokcount --request "$$model" $$src > build/tokens/$$n.req.json || exit 1; curl -sS --fail-with-body $(TOKEN_HEADERS) -H "content-type: application/json" --data-binary @build/tokens/$$n.req.json "$(TOKEN_API)/messages/count_tokens" -o build/tokens/$$n.json || exit 1; done
	build/tokcount --table build/tokens > build/tokens/table.md
	cat build/tokens/model.txt build/tokens/versions.txt build/tokens/table.md

clean:
	rm -rf build

# S10: the WebGPU rows of test/gate.sh. Run it outside the sandbox (deno needs
# a GPU adapter).
gpu-check: build build/jstest check-clang
	sh test/gate.sh gpu
