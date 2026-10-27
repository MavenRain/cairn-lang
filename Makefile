# Builds the cairn-lang kernel port (the intent-lang kernel at 67445ae) on the
# tcc-wasm host with TinyCC. See README.md. src/front/lower.c, src/wasm.c and
# src/ir.h stay out of the build until the target slice (S8).
TCC ?= tcc
CC ?= cc
CFLAGS = -std=c99 -Wall -Werror -Isrc
CLANG_FLAGS = -std=c99 -Wall -Wextra -Wswitch-enum -Werror -fsyntax-only -Isrc
FRONT = src/front/base.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/lexer.c src/front/parser.c src/front/front.c src/front/eval.c src/front/check.c src/sha256.c
SRC = src/main.c $(FRONT)
FRONT_TOOL = test/fronttool.c $(filter-out src/front/front.c,$(FRONT))
BLSAG_TOOL = test/blsagtool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/sha256.c
ELGAMAL_TOOL = test/elgamaltool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/sha256.c
THRESHOLD_TOOL = test/thresholdtool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/sha256.c
DKG_TOOL = test/dkgtool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/dkg.c src/sha256.c
SIGMA_TOOL = test/sigmatool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/sigma.c src/sha256.c
BALLOT_TOOL = test/ballottool.c src/front/u256.c src/front/field.c src/front/secp256k1.c src/front/blsag.c src/front/elgamal.c src/front/threshold.c src/front/sigma.c src/sha256.c
SECP_TOOL = test/secptool.c src/front/u256.c src/front/field.c src/front/secp256k1.c
SHA_TOOL = test/shatool.c src/sha256.c
PATH_TOOL = test/pathtool.c $(filter-out src/front/front.c,$(FRONT))
RUN_TOOL = test/runtool.c $(FRONT)
HEADERS = $(wildcard src/*.h src/front/*.h)

.PHONY: build check check-clang clean

build: build/langc build/fronttool build/blsagtool build/elgamaltool build/thresholdtool build/dkgtool build/sigmatool build/ballottool build/secptool build/shatool build/pathtool build/runtool

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

check-clang: build/domain.c
	$(CC) $(CLANG_FLAGS) $(SRC) build/domain.c
	$(CC) $(CLANG_FLAGS) $(FRONT_TOOL)
	$(CC) $(CLANG_FLAGS) $(BLSAG_TOOL)
	$(CC) $(CLANG_FLAGS) $(ELGAMAL_TOOL)
	$(CC) $(CLANG_FLAGS) $(THRESHOLD_TOOL)
	$(CC) $(CLANG_FLAGS) $(DKG_TOOL)
	$(CC) $(CLANG_FLAGS) $(SIGMA_TOOL)
	$(CC) $(CLANG_FLAGS) $(BALLOT_TOOL)
	$(CC) $(CLANG_FLAGS) $(SECP_TOOL)
	$(CC) $(CLANG_FLAGS) $(SHA_TOOL)
	$(CC) $(CLANG_FLAGS) $(PATH_TOOL)
	$(CC) $(CLANG_FLAGS) $(RUN_TOOL) build/domain.c

check: build/langc build/fronttool build/blsagtool build/elgamaltool build/thresholdtool build/dkgtool build/sigmatool build/ballottool build/secptool build/shatool build/pathtool build/runtool check-clang
	sh test/gate.sh
	sh test/run.sh
	sh test/review.sh
	build/fronttool
	build/blsagtool
	build/elgamaltool
	build/thresholdtool
	build/dkgtool
	build/sigmatool
	build/ballottool
	build/secptool
	build/shatool
	build/pathtool
	build/runtool

clean:
	rm -rf build
