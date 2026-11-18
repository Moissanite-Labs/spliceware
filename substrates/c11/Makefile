# C11 evolutionary substrate lineage (RAM == WIRE == HASH)
# Builds every canonical generation with the flags this repo verifies in CI/manual runs.
CC      ?= gcc
CFLAGS  ?= -O3 -Wall -Wextra -std=c11
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

clean:
	rm -f $(BINS)

.PHONY: all clean
