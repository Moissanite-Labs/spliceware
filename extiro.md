Extiroh is a minimal Rust runtime loop that fuses Extism (deterministic WebAssembly cells), Iroh (BLAKE3 content-addressed P2P streams and gossip), and Loro (causal Merkle-DAG state) into a single hot-splicing fabric. [1] (https://crates.io/crates/loro), [2] (https://github.com/extism/extism/releases), [3] (https://crates.io/crates/iroh-blobs), [4] (https://www.iroh.computer/blog/iroh-gossip-road-to-1-0), [5] (https://crates.io/crates/iroh/1.3.0), [6] (https://loro.dev/)
In an Extiroh node, functions do not live in static files on disk. Every active route points to a 32-byte BLAKE3 hash inside a causal CRDT map, allowing an agent to synthesize, verify, and hot-swap live execution pathways mid-flight.

The Extiroh Architecture (Cargo.toml Triad)


Organ	Crate Stack	Role in the Extiroh Loop
Execution Cell	extism	Compiles and runs isolated Wasm bytecode in memory with strict fuel/instruction metering and zero ambient host access.
Content & Wire	iroh + iroh-blobs + iroh-gossip	Streams Wasm bytecode by its verified BLAKE3 hash over QUIC and broadcasts causal state deltas across peer swarms.
Causal Nervous System	loro (v1.13+)	Tracks the live routing table (route_name -> blake3_hash) with Git-like causal frontiers, zero-cost forks, and instant time-travel rollbacks.


Minimal Proof-of-Concept: The Live Hot-Splice Loop

The Rust skeleton below shows how a running node executes live traffic, catches a data-shape fault, lets an agent splice in a newly compiled Wasm hash inside a causal fork, and broadcasts the verified mutation.
rust
use anyhow::{anyhow, Result};
use dashmap::DashMap;
use extism::{Manifest, Plugin, Wasm};
use iroh_blobs::Hash as Blake3Hash;
use loro::{ExportMode, LoroDoc};
use std::sync::{Arc, Mutex};

pub struct ExtirohCell {
    /// Causal routing table: maps capability names ("parse_order") -> BLAKE3 Wasm Hash
    causal_state: Arc<Mutex<LoroDoc>>,
    /// Warm, pre-compiled Extism Wasm plugins indexed by their exact BLAKE3 hash
    warm_cells: DashMap<Blake3Hash, Mutex<Plugin>>,
    /// Content-addressed store (backed by iroh-blobs in full P2P deployment)
    blob_store: DashMap<Blake3Hash, Vec<u8>>,
}

impl ExtirohCell {
    pub fn new() -> Self {
        Self {
            causal_state: Arc::new(Mutex::new(LoroDoc::new())),
            warm_cells: DashMap::new(),
            blob_store: DashMap::new(),
        }
    }

    /// 1. LIVE FLOW: Route traffic through whatever hash the causal graph currently points to
    pub fn dispatch(&self, capability: &str, payload: &[u8]) -> Result<Vec<u8>> {
        let active_hash = {
            let doc = self.causal_state.lock().unwrap();
            let routes = doc.get_map("routes");
            let hash_hex = routes
                .get(capability)
                .and_then(|v| v.into_value().ok())
                .and_then(|v| v.into_string().ok())
                .ok_or_else(|| anyhow!("No active cell for capability: {capability}"))?;
            hash_hex.parse::<Blake3Hash>()?
        };

        self.run_hash(active_hash, "handle", payload)
    }

    /// Deterministic execution of an immutable Wasm hash
    fn run_hash(&self, hash: Blake3Hash, entry: &str, input: &[u8]) -> Result<Vec<u8>> {
        if !self.warm_cells.contains_key(&hash) {
            let wasm_bytes = self
                .blob_store
                .get(&hash)
                .ok_or_else(|| anyhow!("Missing Wasm blob {hash} - fetch via iroh-blobs"))?;

            // Pure sandbox: WASI disabled, no filesystem, no network, deterministic memory
            let manifest = Manifest::new([Wasm::data(wasm_bytes.value().clone())]);
            let plugin = Plugin::new(&manifest, [], false)?;
            self.warm_cells.insert(hash, Mutex::new(plugin));
        }

        let cell = self.warm_cells.get(&hash).unwrap();
        let mut plugin = cell.lock().unwrap();
        let out = plugin.call::<&[u8], Vec<u8>>(entry, input)?;
        Ok(out)
    }

    /// 2. THE SPLICE: Speculatively test new Wasm on a causal fork, then commit & gossip
    pub fn splice_capability(
        &self,
        capability: &str,
        new_wasm: Vec<u8>,
        faulting_payload: &[u8],
        invariant_check: impl Fn(&[u8]) -> bool,
    ) -> Result<(Blake3Hash, Vec<u8>)> {
        // Content-address the synthesized Wasm bytecode with BLAKE3
        let new_hash = Blake3Hash::new(&new_wasm);
        self.blob_store.insert(new_hash, new_wasm);

        // Fork the causal state in microseconds without touching live traffic
        let speculative_branch = {
            let doc = self.causal_state.lock().unwrap();
            doc.fork()
        };

        // Trial-run the stalled payload against the candidate hash BEFORE mutating state
        let trial_output = self.run_hash(new_hash, "handle", faulting_payload)?;
        if !invariant_check(&trial_output) {
            return Err(anyhow!("Splice rejected: failed downstream invariant proof"));
        }

        // Record the mutation on the fork and capture the causal delta
        let routes = speculative_branch.get_map("routes");
        routes.insert(capability, new_hash.to_string())?;
        speculative_branch.commit();

        // Merge verified causal delta into live node state & export bytes for iroh-gossip
        let mut live_doc = self.causal_state.lock().unwrap();
        let prev_frontiers = live_doc.oplog_frontiers();
        let delta_bytes = speculative_branch.export(ExportMode::updates(&prev_frontiers))?;
        live_doc.import(&delta_bytes)?;

        // Return (Verified BLAKE3 Hash, Gossip Delta Packet)
        Ok((new_hash, delta_bytes))
    }
}
Use code with caution.

What Happens Across the Network in Real Time

• Zero-Downtime Cutover. While splice_capability compiles and tests the candidate Wasm against faulting_payload inside speculative_branch, concurrent calls to dispatch() keep flowing uninterrupted on the previous hash.
• Sub-Millisecond Gossip Sync. Once live_doc.import(&delta_bytes) succeeds, your node broadcasts delta_bytes (typically under 200 bytes) over an iroh-gossip swarm topic. [1] (https://www.iroh.computer/blog/iroh-gossip-road-to-1-0)
• Lazy Verified Streaming. When a peer node imports that Loro delta, its routing map immediately updates parse_order -> 0x9a4f.... On the very next request, the peer pulls those few kilobytes of Wasm over a direct QUIC stream via iroh-blobs, which verifies every streamed chunk against the BLAKE3 Merkle tree before extism ever instantiates it. [1] (https://crates.io/crates/iroh-blobs), [2] (https://crates.io/crates/loro), [3] (https://docs.rs/iroh-blobs/latest/iroh_blobs/)
• Instant Causal Time-Travel. If downstream telemetry later flags higher latency on 0x9a4f..., Loro's checkout(&prev_frontiers) snaps the node back to the exact prior causal state without recompiling or redeploying anything. [1] (https://crates.io/crates/loro)

How the Agent Plugs In

Instead of giving an LLM filesystem access, you expose one host callback triggered whenever dispatch() returns an error or schema mismatch:
1. The runtime hands the agent the failing payload bytes, the current Wasm source/AST metadata (stored as an Iroh HashSeq alongside the bytecode), and the expected output invariant.
2. The agent emits a minimal guest snippet (Rust, AssemblyScript, or TinyGo compiled in-memory to .wasm).
3. The node feeds those bytes straight into splice_capability(). Either the invariant holds and the network heals itself, or the fork evaporates harmlessly. [1] (https://docs.rs/iroh-blobs/latest/iroh_blobs/)
Would you like to flesh out the P2P networking glue (iroh::Endpoint wiring iroh-blobs + iroh-gossip together) so two local terminal nodes can live-sync a splice, or write the guest Wasm synthesis + fuel-metering guardrails first?
