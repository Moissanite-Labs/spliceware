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
