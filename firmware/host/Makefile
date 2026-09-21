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
# F6.5 (test_lock) exercises lock.c's pure section: lock_passcode_valid()/
# lock_backoff_seconds() (no dependency at all) and lock_pbkdf2()/
# lock_parse_cfg() (real mbedtls + cbor.c respectively, same
# `#ifdef ESP_PLATFORM` split pattern as setup.c/auth.c) — everything else in
# lock.c (NVS/RTC/lock_ingest_cfg_cbor()) needs ESP-IDF and is not built here.
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

TESTS := test_cbor test_auth test_setup test_input test_msg test_lock test_loc test_cafetch test_catrust test_cfg test_sms test_carrier test_chat_layout test_coverage
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

# test_auth: auth.c + test_auth.c. auth.c itself signs/verifies raw byte
# buffers, agnostic to what is encoded inside them, but v0.2 (docs/
# V02_DESIGN.md §3) adds a large-`n` (>= 2^32) check that decodes the
# verified/trimmed buffer with cbor_r_uint() to confirm cbor.c's minimal-
# length uint64 decode agrees with the value the vector was signed with —
# hence cbor.c/cbor.h join the link here too.
$(BUILD)/auth.o: $(MAIN_SRC)/auth.c $(MAIN_SRC)/auth.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/auth.c

$(BUILD)/test_auth.o: test_auth.c $(MAIN_SRC)/auth.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_auth.c

$(BUILD)/test_auth: $(BUILD)/auth.o $(BUILD)/cbor.o $(BUILD)/test_auth.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/auth.o $(BUILD)/cbor.o $(BUILD)/test_auth.o

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

# test_lock: lock.c (its pure section only — everything past its own
# `#ifdef ESP_PLATFORM` split needs NVS/RTC/esp_timer/esp_random and is not
# built here) + cbor.c (lock_parse_cfg() decodes the `cfg` map) +
# test_lock.c, linked against the real mbedtls (lock_pbkdf2()).
$(BUILD)/lock.o: $(MAIN_SRC)/lock.c $(MAIN_SRC)/lock.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(MBEDTLS_CFLAGS) -c -o $@ $(MAIN_SRC)/lock.c

$(BUILD)/test_lock.o: test_lock.c $(MAIN_SRC)/lock.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(MBEDTLS_CFLAGS) -c -o $@ test_lock.c

$(BUILD)/test_lock: $(BUILD)/lock.o $(BUILD)/cbor.o $(BUILD)/test_lock.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/lock.o $(BUILD)/cbor.o $(BUILD)/test_lock.o $(MBEDTLS_LIBS)

# test_loc: loc.c (its pure section only -- everything past its own
# `#ifdef ESP_PLATFORM` split needs ESP-IDF/net.h/accel.h/auth.h/ident.h and
# is not built here) + cbor.c (loc_build_cbor()/loc_parse_req_cbor()) +
# test_loc.c. No mbedtls needed: loc.c's pure section never signs anything
# (auth_sign() is only ever called from its device-only section).
$(BUILD)/loc.o: $(MAIN_SRC)/loc.c $(MAIN_SRC)/loc.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/loc.c

$(BUILD)/test_loc.o: test_loc.c $(MAIN_SRC)/loc.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_loc.c

$(BUILD)/test_loc: $(BUILD)/loc.o $(BUILD)/cbor.o $(BUILD)/test_loc.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/loc.o $(BUILD)/cbor.o $(BUILD)/test_loc.o

# test_cafetch: cafetch.c's pure section (URL parse, HTTP/1.1 incremental
# parser, SHA-256 + exactly-one-PEM body validator) + test_cafetch.c, linked
# against the real mbedtls (cafetch_validate_body() calls mbedtls_sha256()) --
# same pattern as test_lock/test_setup above.
$(BUILD)/cafetch.o: $(MAIN_SRC)/cafetch.c $(MAIN_SRC)/cafetch.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(MBEDTLS_CFLAGS) -c -o $@ $(MAIN_SRC)/cafetch.c

$(BUILD)/test_cafetch.o: test_cafetch.c $(MAIN_SRC)/cafetch.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(MBEDTLS_CFLAGS) -c -o $@ test_cafetch.c

$(BUILD)/test_cafetch: $(BUILD)/cafetch.o $(BUILD)/test_cafetch.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/cafetch.o $(BUILD)/test_cafetch.o $(MBEDTLS_LIBS)

# test_catrust: catrust.c's pure section (trust-state derivation, TLS-fail
# fallback policy, daily/cold-boot revalidation gate, cfg `ca` sub-map decode,
# give-up counter, /status fingerprint) + cbor.c + test_catrust.c. No mbedtls
# needed: the pure section never hashes anything itself (ca_fp is formatted
# from a hash the caller already computed elsewhere).
$(BUILD)/catrust.o: $(MAIN_SRC)/catrust.c $(MAIN_SRC)/catrust.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/catrust.c

$(BUILD)/test_catrust.o: test_catrust.c $(MAIN_SRC)/catrust.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_catrust.c

$(BUILD)/test_catrust: $(BUILD)/catrust.o $(BUILD)/cbor.o $(BUILD)/test_catrust.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/catrust.o $(BUILD)/cbor.o $(BUILD)/test_catrust.o

# test_cfg: cfg.c's pure section (the `cfg` envelope/sub-map dispatcher --
# lock/ca/unknown(sms) byte-span extraction) + cbor.c + test_cfg.c.
$(BUILD)/cfg.o: $(MAIN_SRC)/cfg.c $(MAIN_SRC)/cfg.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/cfg.c

$(BUILD)/test_cfg.o: test_cfg.c $(MAIN_SRC)/cfg.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_cfg.c

$(BUILD)/test_cfg: $(BUILD)/cfg.o $(BUILD)/cbor.o $(BUILD)/test_cfg.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/cfg.o $(BUILD)/cbor.o $(BUILD)/test_cfg.o

# test_sms: sms.c's pure section (allow-list validation/matching, cfg.sms
# decode, GSM-7/UCS-2 encoding decision + encode/decode, the audit ring, the
# sms_log CBOR encoder) + cbor.c + test_sms.c. No mbedtls needed: the pure
# section never hashes/signs anything itself (auth_sign() is only ever
# called from sms.c's device-only section).
$(BUILD)/sms.o: $(MAIN_SRC)/sms.c $(MAIN_SRC)/sms.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/sms.c

$(BUILD)/test_sms.o: test_sms.c $(MAIN_SRC)/sms.h $(MAIN_SRC)/cbor.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_sms.c

$(BUILD)/test_sms: $(BUILD)/sms.o $(BUILD)/cbor.o $(BUILD)/test_sms.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/sms.o $(BUILD)/cbor.o $(BUILD)/test_sms.o

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

# test_chat_layout: scr_chat.c's pure row-layout section only (its own
# `#ifdef ESP_PLATFORM` split, added by the same owner task that added this
# test, keeps gfx.h/msg.h/book.h/ime.h/sms.h/ESP-IDF-dependent rendering
# code out of the host build) + test_chat_layout.c. No shared header (see
# test_chat_layout.c's own comment for why); the two files are linked
# together here so a signature mismatch is a link error, not silent.
$(BUILD)/scr_chat.o: $(MAIN_SRC)/scr_chat.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/scr_chat.c

$(BUILD)/test_chat_layout.o: test_chat_layout.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_chat_layout.c

$(BUILD)/test_chat_layout: $(BUILD)/scr_chat.o $(BUILD)/test_chat_layout.o
	$(CC) $(CFLAGS) -o $@ $(BUILD)/scr_chat.o $(BUILD)/test_chat_layout.o

# test_carrier: carrier.c's pure half (APN validation, SIM-based detection).
$(BUILD)/carrier.o: $(MAIN_SRC)/carrier.c $(MAIN_SRC)/carrier.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/carrier.c

$(BUILD)/test_carrier.o: test_carrier.c $(MAIN_SRC)/carrier.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_carrier.c

$(BUILD)/test_carrier: $(BUILD)/carrier.o $(BUILD)/test_carrier.o
	$(CC) $(CFLAGS) -o $@ $^

# test_coverage: coverage.c (whole file -- no ESP-IDF dependency at all, see
# coverage.h's own module comment) + test_coverage.c.
$(BUILD)/coverage.o: $(MAIN_SRC)/coverage.c $(MAIN_SRC)/coverage.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $(MAIN_SRC)/coverage.c

$(BUILD)/test_coverage.o: test_coverage.c $(MAIN_SRC)/coverage.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ test_coverage.c

$(BUILD)/test_coverage: $(BUILD)/coverage.o $(BUILD)/test_coverage.o
	$(CC) $(CFLAGS) -o $@ $^
