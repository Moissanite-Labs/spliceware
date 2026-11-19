# Era IV — Cellular Substrate and the Sacred Splicer

Sources: [`conversation.md`](../mithrax-splicing-session/conversation.md) Turns 13-19, [`code-02`](../mithrax-splicing-session/code-02-zero-trust-execution-cell.txt) through [`code-12`](../mithrax-splicing-session/code-12-ignite-evolutionary-peers.sh), [`inline-element-12`](../mithrax-splicing-session/inline-element-12-zero-trust-execution-cell-diagram.svg) through [`16`](../mithrax-splicing-session/inline-element-16-will-i-ever-be-more-ready-web-results.html). Compilable form: [`substrates/c11/`](../substrates/c11/) (`v1_genesis.c`).

## Zero-dependency C11 collapse (`RAM == WIRE == HASH`)

The Rust/Wasm stack from Era III collapses to one packed C11 atom, a 1,088-byte `Cell`:

- `uint8_t left[32]`: 32-byte Merkle link to `ancestor_a`
- `uint8_t right[32]`: 32-byte Merkle link to `ancestor_b`
- `uint8_t body[1024]`: header plus bytecode/data payload

One 256-bit ARX sponge hashes the whole cell. An O(1) open-addressed in-memory arena indexes cells by hash. A cell fits in a single unfragmented UDP datagram (1,088 < 1,472 MTU) and is lazily fetched on arena cache miss inside `resolve()`. RAM, the wire format, and the hash preimage are the same bytes.

## Fault-driven synthesis and Mitosis (`0xFFFF`)

- A missing hash does not crash. It traps into a Fault Cell (`synth_hash`), which marks the gap for repair instead of aborting the tick.
- **Mitosis (`0xFFFF`).** Execution forks into `ancestor_a` and `ancestor_b` branches inside `step_branch()`, and an A/B fuel tournament picks the survivor.

## The Three Laws of the Sacred Splicer (`code-08-sacred-splice.c`)

1. **Codon integrity.** Instruction boundaries are scanned through an explicit Opcode Valence Table, `(arity_bytes, delta_sp)`, so crossover never slices mid-immediate (never splitting a 2-byte `PUSH imm` or a 33-byte `CALL hash32`).
2. **Zero-net stack seams (Δsp == 0).** Homologous crossover is permitted only at program counters where cumulative stack depth returns to zero, so spliced child bytecode can never underflow or overflow the stack expectations at load time.
3. **Deterministic entropy from lineage.** Crossover cut points and point mutations derive their PRNG stream from `ARX_sponge(parent_a.hash || parent_b.hash || tick)`, making evolution replayable on every peer with no central coordination.

## What this era established

- The whole runtime is one file of freestanding-ish C11 with no dependencies beyond the socket API.
- Selection pressure, crossover, and verification are all expressible inside the cell format itself.
- The raw capture's three non-compilable artifacts (raw elided, truncated, inverted shell) are catalogued in [`INDEX.md`](INDEX.md) and were not promoted into `substrates/`.

Related: [Era V](05-adaptive-priors-and-lexicase-selection.md).
