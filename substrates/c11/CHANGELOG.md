# C11 Substrate Lineage

Generations of the zero-dependency C11 evolutionary substrate (`RAM == WIRE == HASH`),
in order of increasing design maturity. All build with `-O3 -Wall -Wextra -std=c11`
(see `Makefile`); `run_peers.sh` launches a two-node UDP pair.

| File | Lineage | Notes |
| :--- | :--- | :--- |
| `v1_genesis.c` | `mithrax-splicing-session/code-10-genesis-substrate-complete.c` (PR #3, `7a1a27f`) | Self-bootstrapping `genesis.c`: 1,088-byte packed `Cell` (32B `left` + 32B `right` + 1,024B `body`), 256-bit ARX sponge, O(1) open-addressed arena, fuel-metered stack reducer, `0xFFFF` Mitosis A/B tournament, homologous `sacred_splice()` crossover. |
| `v2_adaptive_loop.c` | `v1_genesis.c` + `mithrax-splicing-session/code-13-loop-call-adaptive-weights.c` (PR #3, `7a1a27f`) | Adds statically balanced `LOOP` (`0x07`) / `END` (`0x08`) with load-time `Δsp == 0` verification (`verify_and_map`), content-addressed `CALL` (`0x09`), and the 10-byte in-Cell adaptive prior vector (`HDR_WEIGHTS`, `body[2..11]`) with multiplicative `adapt_prior()` updates on tournament victory. |
| `v3_evogenesis.c` | `sacred-splicer` branch, salvaged in this consolidation (PR #2; merged history `f546255..97b8feb`) | 12-codon ISA: adds `DUP` (`0x0A`) and `OVER` (`0x0B`); 12-byte `adapt_prior()`; 8-byte Triangular Sum two-tiered Lexicase anchor gate (`4/4` canonical promotion, `>= 3/4` stepping-stone pool, anchors `(0,0) (5,15) (13,91) (37,191)`). |
| `v4_unigenesis.c` | `sacred-splicer` branch, salvaged in this consolidation (PR #2; merged history `f546255..97b8feb`) | Refactors the body around a 64-byte `int32_t`-indexed header (`HDR_GENERATION`, `HDR_FITNESS`, `HDR_WEIGHTS[8]`, `HDR_ANCHOR_LO`/`HDR_ANCHOR_HI`) and a 24-codon deterministic branching ISA (`OP_JMP`, `OP_JZ`, `OP_JNZ`, `OP_CMP`, modulo NOP space for neutral drift). |

## Alias reconciliation (PR #2 paired evolution)

- `g3n.c` (786 lines) and `evogenesis.c` (786 lines) differ only by a trailing newline
  (verified with `git diff` during consolidation, 2026-11-18). `g3n.c` was promoted to
  `v3_evogenesis.c`; `g3n.c` is retained as an alias ancestor in git history (merge commit
  `3cc023c`), not duplicated on disk.
- `gen2.c` (574 lines) and `unigenesis.c` (574 lines) likewise differ only by a trailing
  newline. `unigenesis.c` was promoted to `v4_unigenesis.c`; `gen2.c` is retained in git
  history, not duplicated on disk.

## Provenance

- Raw corpus (verbatim, immutable): `../../mithrax-splicing-session/`
- Formal proposals (Two-Tiered Lexicase + Triangular Sum): `../../docs/proposals/2026-11-lexicase-and-triangular-sum.md`
- Branch archive and SHAs: `../../docs/INDEX.md` § Archived Branch Provenance
- `gen.sh` (PR #2) was removed: it hardcoded a `genesis.c` filename missing on that branch
  and duplicated `mithrax-splicing-session/code-12-ignite-evolutionary-peers.sh`. Use
  `make` + `run_peers.sh <generation_binary> [port_a] [port_b]` instead.

## Defect notes

The raw capture contains non-compilable artifacts that were **not** promoted here:
`code-04-continuous-substrate-flow-raw-elided.c` (browser-elided `#define ARENA_CAP (1 51)`),
`code-09-genesis-substrate-raw-truncated.c`, and `code-11-ignite-evolutionary-peers-raw-inverted.txt`.
See `../../mithrax-splicing-session/code-04-continuous-substrate-flow-complete.c` for the complete form.
The `INADDR_LOOPBACK` feature-test visibility fix (`<netinet/in.h>` plus a portable fallback)
was applied to the substrate copies only; the raw archive is untouched.
