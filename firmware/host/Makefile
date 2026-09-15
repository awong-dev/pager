# firmware/host/Makefile — plain host-compiler build for firmware unit
# tests, no ESP-IDF (docs/DEVICE_TASKS.md, Track F preamble). Task F3.2
# adds cbor.c's test; F3.3 adds auth.c's (auth.c has no ESP-IDF/mbedTLS
# dependency in its host build — see auth.c's `#ifdef ESP_PLATFORM` split —
# so no external crypto library is needed here either). Later F tasks add
# their own test_*.c + object here without needing a second build system.
#
# `make test` builds and runs every test binary; `make` alone just builds.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
INCLUDES := -I../main

MAIN_SRC := ../main
BUILD := build

TESTS := test_cbor test_auth
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

clean:
	rm -rf $(BUILD)
