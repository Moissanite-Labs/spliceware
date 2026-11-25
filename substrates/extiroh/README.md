# Extiroh — Rust/Wasm Organ Triad Substrate

Sketch-quality consolidation of the Era III architecture (Turns 10–12 of the
discovery session). This is an **architecture sketch**, not a buildable crate:
it is preserved here as the Rust/Wasm reference point for the C11 collapse in
`../c11/`, not as a production runtime.

- `runtime_loop.rs` — hardened evolution of
  `../../mithrax-splicing-session/code-01-extiroh-runtime-loop.rs` (raw archive
  at `7a1a27f` remains immutable). Stores warm plugins as `Arc<Mutex<Plugin>>` so
  DashMap shard guards drop before `plugin.call`, evicts poisoned plugin mutexes
  and returns errors instead of panicking, trial-runs candidate splices on an
  isolated fresh `Plugin` before touching `blob_store`/`warm_cells`, holds both
  shard `Entry` guards across atomic `AtomicUsize` capacity reservation and CRDT
  `import` before publishing (eliminating post-insert rollback races even on
  import error or panic), and enforces race-free hard capacity limits.
- `Cargo.toml` + `triad_shim.rs` — zero-dependency, `#![forbid(unsafe_code)]`
  offline stdlib test crate (16 real `RwLock` shards, distinct `LoroError` ->
  `anyhow::Error` conversion) so `RUSTFLAGS="-D warnings" cargo test` and
  `cargo check --lib` build and verify `ExtirohCell` cleanly offline with zero
  network downloads. Downstream production workspaces integrating external
  `anyhow`/`dashmap`/`extism`/`iroh-blobs`/`loro` crates replace `triad_shim`
  with the pinned crates listed below.

## The triad

1. **Extism (Wasmtime)** — sandboxed, hot-swappable Wasm "organs" (`run_hash()`,
   `splice_capability()`), keyed by BLAKE3 digest. WASI disabled, no filesystem,
   no network: ambient zero-trust by construction.
2. **Iroh** — content-addressed QUIC P2P blob/ticket gossip for streaming
   compiled Wasm organs across nodes.
3. **Loro** — replayable causal CRDT state tree; zero-copy state forks before
   evaluating a candidate organ.

## Intended `Cargo.toml` dependencies (as sketched in the session)

```toml
[dependencies]
anyhow = "1"
dashmap = "6"
extism = "1"
iroh-blobs = "0.x"   # pin the release current at implementation time
loro = "1"
```

## Wasm synthesis latency tiers

- **Tier 1** — raw `wasm-encoder` / `wat`: sub-millisecond micro-mutations for
  tight evolutionary loops.
- **Tier 2** — `extism-rust-pdk` / `rustc`: macro-organ compilation for organs
  that need real toolchains.

## The 4 Deterministic Guardrails ("Laws of Physics")

1. **Strict fuel metering** — bounded instruction budget halts infinite loops.
2. **Linear memory ceilings** — hard Wasm page caps, zero host-heap pointers.
3. **Ambient zero-trust** — zero syscalls/network/filesystem unless explicitly
   wired via host functions.
4. **Metabolic selection** — candidate splices compete in shadow state forks;
   survivors must pass correctness and burn less fuel.

See `../../docs/03-extiroh-wasm-substrate.md` for the full era synthesis and
`../../docs/04-cellular-substrate-and-sacred-splicer.md` for the C11 collapse
(`RAM == WIRE == HASH`, 1,088-byte `Cell`) that replaced this stack.
