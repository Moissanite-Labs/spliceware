# C11 evolutionary substrate lineage (RAM == WIRE == HASH)
# Builds every canonical generation with the flags this repo verifies in CI/manual runs.
CC      ?= gcc
CFLAGS  ?= -O3 -Wall -Wextra -std=c11
SANFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all
BINS     = v1_genesis v2_adaptive_loop v3_evogenesis v4_unigenesis

all: $(BINS)

v1_genesis: v1_genesis.c
	$(CC) $(CFLAGS) $< -o $@

v2_adaptive_loop: v2_adaptive_loop.c
	$(CC) $(CFLAGS) $< -o $@

v3_evogenesis: v3_evogenesis.c
	$(CC) $(CFLAGS) $< -o $@

v4_unigenesis: v4_unigenesis.c
	$(CC) $(CFLAGS) $< -o $@

test: test_c11_hardening.c v1_genesis.c v2_adaptive_loop.c v3_evogenesis.c v4_unigenesis.c
	@set -eu; \
	tmpdir="$$(mktemp -d)"; \
	trap 'rm -rf "$$tmpdir"' EXIT; \
	$(CC) $(SANFLAGS) -I. -DTEST_SUBSTRATE_V1 test_c11_hardening.c -o "$$tmpdir/test_hardening_v1"; \
	$(CC) $(SANFLAGS) -I. -DTEST_SUBSTRATE_V2 test_c11_hardening.c -o "$$tmpdir/test_hardening_v2"; \
	$(CC) $(SANFLAGS) -I. -DTEST_SUBSTRATE_V3 test_c11_hardening.c -o "$$tmpdir/test_hardening_v3"; \
	$(CC) $(SANFLAGS) -I. -DTEST_SUBSTRATE_V4 test_c11_hardening.c -o "$$tmpdir/test_hardening_v4"; \
	"$$tmpdir/test_hardening_v1"; \
	"$$tmpdir/test_hardening_v2"; \
	"$$tmpdir/test_hardening_v3"; \
	"$$tmpdir/test_hardening_v4"

clean:
	rm -f $(BINS)

.PHONY: all test clean
