# firmware/host/Makefile — plain host-compiler build for firmware unit
# tests, no ESP-IDF (docs/DEVICE_TASKS.md, Track F preamble). Task F3.2
# adds cbor.c's test; F3.3 adds auth.c's (auth.c has no ESP-IDF/mbedTLS
# dependency in its host build — see auth.c's `#ifdef ESP_PLATFORM` split —
# so no external crypto library is needed here either). Later F tasks add
# their own test_*.c + object here without needing a second build system.
#
# F3.5 (test_setup) is the one exception to "no external crypto library":
# setup.c's HKDF/AES-256-GCM logic is tested against the *real* mbedtls
# (both ESP-IDF and the host link the same well-tested implementation of
# those two primitives) rather than a hand-rolled fallback — see setup.c's
# module comment for why. `pkg-config mbedcrypto` is tried first; Homebrew's
# keg-only `mbedtls@3` (macOS) is not on the default pkg-config search path,
# so `brew --prefix` is the fallback; a bare `-lmbedcrypto` is the last
# resort for a system install with a discoverable default library path
# (e.g. `apt install libmbedtls-dev` on Linux CI).
#
# `make test` builds and runs every test binary; `make` alone just builds.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
INCLUDES := -I../main

MBEDTLS_CFLAGS := $(shell pkg-config --cflags mbedcrypto 2>/dev/null)
MBEDTLS_LIBS := $(shell pkg-config --libs mbedcrypto 2>/dev/null)
ifeq ($(strip $(MBEDTLS_LIBS)),)
  BREW_MBEDTLS := $(shell brew --prefix mbedtls@3 2>/dev/null || brew --prefix mbedtls 2>/dev/null)
  ifneq ($(strip $(BREW_MBEDTLS)),)
    MBEDTLS_CFLAGS := -I$(BREW_MBEDTLS)/include
    MBEDTLS_LIBS := -L$(BREW_MBEDTLS)/lib -lmbedcrypto
  else
    MBEDTLS_LIBS := -lmbedcrypto
  endif
endif

MAIN_SRC := ../main
BUILD := build

TESTS := test_cbor test_auth test_setup
TEST_BINS := $(TESTS:%=$(BUILD)/%)

.PHONY: all test clean
all: $(TEST_BINS)

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "--- running $$t ---"; \
		./$$t || status=1; \
	done; \
	exit $$status

$(BUILD):
	mkdir -p $(BUILD)

# test_cbor: cbor.c + test_cbor.c
$(BUILD)/cbor.o: $(MAIN_SRC)/cbor.c $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/cbor.c

$(BUILD)/test_cbor.o: test_cbor.c $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_cbor.c

$(BUILD)/test_cbor: $(BUILD)/cbor.o $(BUILD)/test_cbor.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/cbor.o $(BUILD)/test_cbor.o

# test_auth: auth.c + test_auth.c (no cbor.c needed: auth.c signs/verifies
# raw byte buffers, agnostic to what is encoded inside them)
$(BUILD)/auth.o: $(MAIN_SRC)/auth.c $(MAIN_SRC)/auth.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/auth.c

$(BUILD)/test_auth.o: test_auth.c $(MAIN_SRC)/auth.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_auth.c

$(BUILD)/test_auth: $(BUILD)/auth.o $(BUILD)/test_auth.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/auth.o $(BUILD)/test_auth.o

# test_setup: setup.c (host-only top section, ESP_PLATFORM-guarded bottom
# section excluded) + cbor.c (test_setup.c CBOR-decodes the plaintext bundle
# itself, to check the decrypted bytes against the vector's fields) +
# test_setup.c, linked against the real mbedtls.
$(BUILD)/setup.o: $(MAIN_SRC)/setup.c $(MAIN_SRC)/setup.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(MBEDTLS_CFLAGS) -c -o $@ $(MAIN_SRC)/setup.c

$(BUILD)/test_setup.o: test_setup.c $(MAIN_SRC)/setup.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(MBEDTLS_CFLAGS) -c -o $@ test_setup.c

$(BUILD)/test_setup: $(BUILD)/setup.o $(BUILD)/cbor.o $(BUILD)/test_setup.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/setup.o $(BUILD)/cbor.o $(BUILD)/test_setup.o $(MBEDTLS_LIBS)

clean:
	rm -rf $(BUILD)
