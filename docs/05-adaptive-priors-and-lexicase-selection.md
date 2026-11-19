# Era V — Adaptive Priors and Lexicase Selection

Sources: [`conversation.md`](../mithrax-splicing-session/conversation.md) Turns 20-22, [`code-13-loop-call-adaptive-weights.c`](../mithrax-splicing-session/code-13-loop-call-adaptive-weights.c), [`inline-element-17`](../mithrax-splicing-session/inline-element-17-adaptive-probabilities-vs-mmap-dag-table.html) through [`19`](../mithrax-splicing-session/inline-element-19-triangular-sum-vs-targets-table.html), plus the post-session proposals and C11 generations: [`proposals/2026-11-lexicase-and-triangular-sum.md`](proposals/2026-11-lexicase-and-triangular-sum.md), `substrates/c11/v3_evogenesis.c`, `substrates/c11/v4_unigenesis.c`.

## Turing-bounded control flow and in-cell adaptive weights (`code-13`)

- Structured `LOOP` (`0x07`) / `END` (`0x08`), verified statically balanced at load time and bounded by remaining fuel, plus content-addressed `CALL` (`0x09` + 32-byte arena hash).
- An in-Cell adaptive prior vector (`HDR_WEIGHTS`) lives inside `Cell.body` and is updated by multiplicative weight updates (`adapt_prior()`): codons in winning mutant strands get their sampling weight boosted inside the child cell itself. The evolutionary search prior travels on the wire with the genome, not in an external `mmap` controller.

## Proposal 1 — Two-Tiered Lexicase Anchor Gating

Problem: pure fuel tournaments collapse to `0x00 HALT` (zero fuel always wins), and rigid all-or-nothing test suites create zero-gradient cliffs that kill multi-step loop mutations.

Mechanism: 8 bytes in `Cell.body` carry 4 `(in_byte, expected_out_byte)` behavioral anchors, self-verifying on UDP arrival with no external test packets:

- **Tier 1, canonical promotion.** Strict `4/4` anchors plus superior fuel efficiency. Promotes to `champ` and broadcasts. Blocks regression and `HALT` collapse.
- **Tier 2, stepping-stone pool.** Relaxed `>= 3/4` anchors. Admits near-miss mutants into `ancestor_b` as crossover donors, keeping partial `LOOP` scaffolds and adapted priors alive.

## Proposal 2 — Triangular Sum selection and stack ergonomics

`T(n) = n(n+1)/2 mod 256`. Affine targets favor straight-line code and work against `LOOP`; fixed byte parity unrolls into flat `XOR` chains. Packing the four anchors `(0, 0)`, `(5, 15)`, `(13, 91)`, `(37, 191)` (where `T(37) = 703 ≡ 191 mod 256`) defeats unrolling: no fixed `ADD` chain satisfies both `n=5` and `n=37`, and `(0, 0)` gives identity programs an immediate foothold to feed gradient into `adapt_prior()`.

ISA evolution across the salvaged generations:

- `evogenesis.c` / `g3n.c` (786 lines, now `substrates/c11/v3_evogenesis.c`): adds `DUP` (`0x0A`) and `OVER` (`0x0B`), so an `(acc, n)` loop fits in about 9 bytes while keeping Δsp == 0.
- `unigenesis.c` / `gen2.c` (574 lines, now `substrates/c11/v4_unigenesis.c`): a clean 64-byte `int32_t`-indexed header (`HDR_GENERATION`, `HDR_FITNESS`, `HDR_WEIGHTS[8]`, `HDR_ANCHOR_LO`/`HDR_ANCHOR_HI`) and a 24-codon deterministic branching ISA (`OP_JMP`, `OP_JZ`, `OP_JNZ`, `OP_CMP`, modulo NOP space for neutral drift).

The full proposal text is preserved verbatim at [`proposals/2026-11-lexicase-and-triangular-sum.md`](proposals/2026-11-lexicase-and-triangular-sum.md).
