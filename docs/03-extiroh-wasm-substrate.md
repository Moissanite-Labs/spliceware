# Era III — Extiroh, the Rust/Wasm Organ Triad

Sources: [`conversation.md`](../mithrax-splicing-session/conversation.md) Turns 10-12, [`code-01-extiroh-runtime-loop.rs`](../mithrax-splicing-session/code-01-extiroh-runtime-loop.rs), [`inline-element-10`](../mithrax-splicing-session/inline-element-10-extiroh-organs-table.html), [`inline-element-11`](../mithrax-splicing-session/inline-element-11-wasm-synthesis-tiers-table.html). Sketch substrate and dependency notes: [`substrates/extiroh/`](../substrates/extiroh/).

## Architecture ("Extiroh")

Three components, each with one job:

- **Extism (Wasmtime).** Sandboxed, hot-swappable Wasm organs (`run_hash()`, `splice_capability()`), keyed by BLAKE3 digest. An organ is a small compiled capability; dispatch routes a call to whichever hash the causal graph currently points at.
- **Iroh.** Content-addressed QUIC P2P blob and ticket gossip for streaming compiled Wasm organs across nodes. Organs travel by hash, not by URL or release tag.
- **Loro.** A replayable causal CRDT state tree. State forks are zero-copy, so a candidate organ can be evaluated against a fork before anything is committed.

## Wasm synthesis latency tiers

- **Tier 1.** Raw `wasm-encoder` or `wat`: sub-millisecond micro-mutations for tight evolutionary loops.
- **Tier 2.** `extism-rust-pdk` or `rustc`: macro-organ compilation when an organ needs a real toolchain.

## The 4 Laws of Physics (deterministic guardrails)

1. **Strict fuel metering.** A bounded instruction budget halts infinite loops deterministically.
2. **Linear memory ceilings.** Hard Wasm page caps with zero host-heap pointers.
3. **Ambient zero-trust.** Zero syscalls, network, or filesystem unless explicitly wired through host functions.
4. **Metabolic selection.** Candidate splices compete in shadow state forks; survivors must pass correctness and burn less fuel.

## What this era established

- Hot-swapping compiled capabilities is practical if every organ is hash-keyed and sandboxed.
- Evaluation can be forked, scored, and discarded without touching the live state.
- These same laws reappear in the C11 collapse as fuel metering, the 1,088-byte fixed cell, and the Mitosis tournament.

Related: [Era IV](04-cellular-substrate-and-sacred-splicer.md).
