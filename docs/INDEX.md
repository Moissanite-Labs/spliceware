# Corpus Index

Master index for the `spliceware` repository: what is here, where each artifact came from, and which pull request it arrived on.

## Layout

| Path | What it is |
| :--- | :--- |
| [`mithrax-splicing-session/`](../mithrax-splicing-session/) | Verbatim raw capture of the discovery session (PR #3, commit `7a1a27f`). Immutable. |
| [`docs/01-polymerized-splicer-broker.md`](01-polymerized-splicer-broker.md) | Era I synthesis (Turns 1-4) |
| [`docs/02-causal-cryptographic-determinism.md`](02-causal-cryptographic-determinism.md) | Era II synthesis (Turns 5-9) |
| [`docs/03-extiroh-wasm-substrate.md`](03-extiroh-wasm-substrate.md) | Era III synthesis (Turns 10-12) |
| [`docs/04-cellular-substrate-and-sacred-splicer.md`](04-cellular-substrate-and-sacred-splicer.md) | Era IV synthesis (Turns 13-19) |
| [`docs/05-adaptive-priors-and-lexicase-selection.md`](05-adaptive-priors-and-lexicase-selection.md) | Era V synthesis (Turns 20-22 + post-session proposals) |
| [`docs/proposals/2026-11-lexicase-and-triangular-sum.md`](proposals/2026-11-lexicase-and-triangular-sum.md) | Verbatim copy of `proposals.md` from the `sacred-splicer` branch |
| [`substrates/c11/`](../substrates/c11/) | Compilable C11 substrate generations, see its [CHANGELOG](../substrates/c11/CHANGELOG.md) |
| [`substrates/extiroh/`](../substrates/extiroh/) | Rust/Wasm/Iroh/Loro sketch and dependency notes |

## Turn-to-artifact ledger

`conversation.md` is the unified 22-turn transcript. `_raw/` holds the three source tab captures (DOM, frames, `inventory.json`, `page.mhtml`).

| Turns | Theme | Artifacts |
| :--- | :--- | :--- |
| 1-4 | Splicer paradigm, polymerized protocol broker | [`inline-element-01`](../mithrax-splicing-session/inline-element-01-mithrax-splicer-youtube-card.html) through [`05`](../mithrax-splicing-session/inline-element-05-visual-explanation-splicer-hub.jpg) |
| 5-9 | Fluid software, causal and cryptographic determinism | [`inline-element-06`](../mithrax-splicing-session/inline-element-06-fluid-living-system-table.html) through [`09`](../mithrax-splicing-session/inline-element-09-merkle-crdt-wasm-fabric-table.html) |
| 10-12 | Extiroh Rust/Wasm triad, Wasm synthesis tiers | [`code-01`](../mithrax-splicing-session/code-01-extiroh-runtime-loop.rs), [`inline-element-10`](../mithrax-splicing-session/inline-element-10-extiroh-organs-table.html), [`inline-element-11`](../mithrax-splicing-session/inline-element-11-wasm-synthesis-tiers-table.html) |
| 13-19 | C11 collapse (`genesis.c`), Mitosis, Sacred Splicer | [`code-02`](../mithrax-splicing-session/code-02-zero-trust-execution-cell.txt) through [`code-12`](../mithrax-splicing-session/code-12-ignite-evolutionary-peers.sh), [`inline-element-12`](../mithrax-splicing-session/inline-element-12-zero-trust-execution-cell-diagram.svg) through [`16`](../mithrax-splicing-session/inline-element-16-will-i-ever-be-more-ready-web-results.html) |
| 20-22 | Adaptive priors, lexicase anchor gating, Triangular Sum | [`code-13`](../mithrax-splicing-session/code-13-loop-call-adaptive-weights.c), [`inline-element-17`](../mithrax-splicing-session/inline-element-17-adaptive-probabilities-vs-mmap-dag-table.html) through [`19`](../mithrax-splicing-session/inline-element-19-triangular-sum-vs-targets-table.html) |

`session.md` (from `sacred-splicer`, 170 lines) was a code-stripped extract of Turns 18-21. It is not republished; read the turns in [`conversation.md`](../mithrax-splicing-session/conversation.md) directly.

## Provenance by pull request

| PR | Branch | Disposition |
| :--- | :--- | :--- |
| [#3 Sacred Splicer Corpus](https://github.com/Moissanite-Labs/spliceware/pull/3) | `kell-of-light` | Merged into `main` via the consolidation PR (merge commit preserves `7a1a27f`). |
| [#2 unigenesis](https://github.com/Moissanite-Labs/spliceware/pull/2) | `sacred-splicer` | Merged into `main` via the consolidation PR (history `f546255..97b8feb` preserved). Unique files salvaged; `session.md` and `gen.sh` dropped as redundant. |
| [#1 Create polymerization.md](https://github.com/Moissanite-Labs/spliceware/pull/1) | `mithrax-splice` | Closed unmerged. Every file is a verbatim slice of PR #3's `conversation.md` or extracted code blocks. |

## Archived branch provenance

Final tip SHAs, recorded before branch deletion. All are reachable from `main` through the consolidation merge commits, except where noted.

| Branch | Tip SHA | Status |
| :--- | :--- | :--- |
| `kell-of-light` | `7a1a27fec1c9ad886aaac0468846b4a6965e20de` | Reachable via consolidation merge |
| `sacred-splicer` | `97b8feb135605f0570b7537fc5a4fd49f68d4816` | Reachable via consolidation merge |
| `mithrax-splice` | `89c54bc61fdf71e03e24dc1ecfa4c63795837a42` | Closed unmerged; content duplicated by `kell-of-light`. Deleted. |
| `mithrax-exo-patch-1` | `17282f8145031d7f42f660117c02c40598f8bb01` | No PR; content identical to `mithrax-splice/atomic.md` (Turn 14). Deleted. |

## Raw-capture defects (not promoted)

1. `code-04-continuous-substrate-flow-raw-elided.c`: browser UI elision turned `1 << 16` into `(1 51)`. Complete form at `code-04-continuous-substrate-flow-complete.c`.
2. `code-09-genesis-substrate-raw-truncated.c`: truncated mid-file. Complete form at `code-10-genesis-substrate-complete.c`.
3. `code-11-ignite-evolutionary-peers-raw-inverted.txt`: inverted shell capture. Runnable form at `code-12-ignite-evolutionary-peers.sh`.

## Build and run

```sh
make -C substrates/c11                       # v1_genesis v2_adaptive_loop v3_evogenesis v4_unigenesis
substrates/c11/run_peers.sh substrates/c11/v2_adaptive_loop 9001 9002
```
