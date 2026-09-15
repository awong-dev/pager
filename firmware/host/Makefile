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
# F6.1 (render_png) similarly links zlib for its PNG IDAT chunk (a PNG
# IDAT payload *is* a zlib stream, RFC 2083 §9.3, so this is the one
# dependency needed, not a general-purpose PNG library) — same
# pkg-config-then-bare-flag pattern, minus the brew fallback since zlib
# ships with the OS (Xcode CLT / glibc-based distros) rather than being a
# keg-only formula.
#
# F6.2 (test_input) needs no external dependency: input.c's
# `#ifdef ESP_PLATFORM` split keeps input_decode_key() (the only thing this
# test exercises) free of FreeRTOS/driver/esp_timer, same pattern as
# cbor.c/auth.c above.
#
# F6.4 (test_msg) exercises only msg.c's UTF-8 composer section, which sits
# ahead of msg.c's own `#ifdef ESP_PLATFORM` split (same pattern again) —
# everything else in msg.c (ingest/pump/NVS msgq/RTC) needs ESP-IDF/NVS/
# net.cpp and is not built here.
#
# `make test` builds and runs every test binary; `make png` builds and runs
# render_png (task F6.1's host PNG test); `make` alone just builds the tests.

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

ZLIB_CFLAGS := $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS := $(shell pkg-config --libs zlib 2>/dev/null)
ifeq ($(strip $(ZLIB_LIBS)),)
  ZLIB_LIBS := -lz
endif

MAIN_SRC := ../main
BUILD := build

TESTS := test_cbor test_auth test_setup test_input test_msg
TEST_BINS := $(TESTS:%=$(BUILD)/%)

# ASSETS: tools/mkassets.py's default output location (docs/DEVICE_TASKS.md
# F6.1's Verify: `python tools/mkassets.py --lang sc -o build/assets.bin`,
# run from the repo root), resolved relative to this directory.
ASSETS ?= ../../build/assets.bin

.PHONY: all test png clean
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

# test_input: input.c (only its ESP_PLATFORM-free input_decode_key(), per
# input.c's own #ifdef split) + test_input.c.
$(BUILD)/input.o: $(MAIN_SRC)/input.c $(MAIN_SRC)/input.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/input.c

$(BUILD)/test_input.o: test_input.c $(MAIN_SRC)/input.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_input.c

$(BUILD)/test_input: $(BUILD)/input.o $(BUILD)/test_input.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/input.o $(BUILD)/test_input.o

# test_msg: msg.c's composer section only (its own `#ifdef ESP_PLATFORM`
# split keeps NVS/RTC/net/auth/cbor-dependent code, which this test does not
# exercise, out of the host build) + test_msg.c. msg.h pulls in auth.h,
# which is already host-safe (test_auth.c above links the real auth.c
# against it), so no extra include path is needed.
$(BUILD)/msg.o: $(MAIN_SRC)/msg.c $(MAIN_SRC)/msg.h $(MAIN_SRC)/auth.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/msg.c

$(BUILD)/test_msg.o: test_msg.c $(MAIN_SRC)/msg.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_msg.c

$(BUILD)/test_msg: $(BUILD)/msg.o $(BUILD)/test_msg.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/msg.o $(BUILD)/test_msg.o

# render_png: gfx.c (the exact framebuffer/text/glyph module that ships
# on-device, compiled for the host per its own #ifdef ESP_PLATFORM split)
# + render_png.c, linked against zlib for the PNG IDAT chunk.
$(BUILD)/gfx.o: $(MAIN_SRC)/gfx.c $(MAIN_SRC)/gfx.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/gfx.c

$(BUILD)/render_png.o: render_png.c $(MAIN_SRC)/gfx.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(ZLIB_CFLAGS) -c -o $@ render_png.c

$(BUILD)/render_png: $(BUILD)/gfx.o $(BUILD)/render_png.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/gfx.o $(BUILD)/render_png.o $(ZLIB_LIBS)

png: $(BUILD)/render_png
	./$(BUILD)/render_png $(ASSETS)

clean:
	rm -rf $(BUILD)
