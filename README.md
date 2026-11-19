# spliceware

Explorations on new interface methods with code and software.

This repository holds the raw capture of one discovery session (2026-11) and the materials extracted from it: a written synthesis, compilable C11 substrate generations, and a Rust/Wasm sketch. The exploration moved through five eras, from "agents that patch text" to a self-bootstrapping 1,088-byte evolutionary cell that gossips over UDP.

## Layout

- `mithrax-splicing-session/` — verbatim raw capture (PR #3, `7a1a27f`). Immutable evidentiary record: `_raw/` browser captures, the unified `conversation.md` (22 turns), extracted code blocks, and HTML/SVG tables.
- `docs/` — era syntheses `01` through `05`, plus `INDEX.md` (master index, turn ledger, PR and branch provenance) and `proposals/`.
- `substrates/c11/` — compilable C11 generations `v1_genesis` through `v4_unigenesis`. See `substrates/c11/CHANGELOG.md` for the lineage and alias reconciliation.
- `substrates/extiroh/` — Rust/Wasm/Iroh/Loro sketch (`runtime_loop.rs`) and dependency notes.

## Quickstart

```sh
make -C substrates/c11
substrates/c11/run_peers.sh substrates/c11/v2_adaptive_loop 9001 9002
```

## Architecture in one paragraph

An Eliksni-style "Splicer" agent edits through live structural and runtime graphs instead of text diffs: MCP brokers capabilities, LSP/Tree-sitter/CPG supply the structural view, DAP/OCI the sandbox, SCIP/SQLite the semantic index. Determinism comes from Merkle lineage and RFC 6979/DAEAD cryptography: people cannot predict what evolution invents, but every node replays the exact same trajectory. The design first materialized as the Extiroh Rust/Wasm organ triad, then collapsed to a single packed C11 `Cell` (RAM == WIRE == HASH) with fuel-metered execution, `0xFFFF` Mitosis A/B tournaments, homologous crossover constrained to Δsp == 0 seams, in-cell adaptive priors, and Triangular Sum lexicase anchor gating.

## Reading order

1. `docs/INDEX.md` — where everything is and where it came from
2. `docs/01` … `docs/05` — the five eras
3. `mithrax-splicing-session/conversation.md` — the primary source

## Related repositories (Moissanite-Labs)

- [MDE](https://github.com/Moissanite-Labs/MDE) — weight-space research; the anchor-gating design maps onto its open questions Q1-Q6.
- [weft-loom](https://github.com/Moissanite-Labs/weft-loom) — experiments for ML Weft; substrates here are reference material for cell-arena experiments.
- [nullmodem](https://github.com/Moissanite-Labs/nullmodem) — agent-to-agent communication; the UDP cell gossip design is a benchmark candidate.
- [exo](https://github.com/Moissanite-Labs/exo), [synbios](https://github.com/Moissanite-Labs/synbios) — harness and forge; see their RFC issues for distilled patterns.
