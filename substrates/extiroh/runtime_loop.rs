#![forbid(unsafe_code)]

mod triad_shim;

use triad_shim::anyhow::{anyhow, Result};
use triad_shim::dashmap::{DashMap, Entry};
use triad_shim::extism::{Manifest, Plugin, Wasm};
use triad_shim::iroh_blobs::Hash as Blake3Hash;
use triad_shim::loro::{ExportMode, LoroDoc};

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

pub const DEFAULT_MAX_BLOBS: usize = 1024;
pub const DEFAULT_MAX_WARM_CELLS: usize = 256;

/// RAII capacity reservation backed by an `AtomicUsize` CAS ceiling.
/// Releases the slot automatically on drop unless `commit()` is called.
struct CapacityReservation<'a> {
    counter: &'a AtomicUsize,
    active: bool,
}

impl<'a> CapacityReservation<'a> {
    fn try_new(counter: &'a AtomicUsize, cap: usize) -> Option<Self> {
        let mut cur = counter.load(Ordering::Acquire);
        loop {
            if cur >= cap {
                return None;
            }
            match counter.compare_exchange_weak(
                cur,
                cur + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => {
                    return Some(Self {
                        counter,
                        active: true,
                    });
                }
                Err(actual) => cur = actual,
            }
        }
    }

    fn commit(mut self) {
        self.active = false;
    }
}

impl Drop for CapacityReservation<'_> {
    fn drop(&mut self) {
        if self.active {
            self.counter.fetch_sub(1, Ordering::AcqRel);
        }
    }
}

pub struct ExtirohCell {
    /// Causal routing table: maps capability names ("parse_order") -> BLAKE3 Wasm Hash
    causal_state: Arc<Mutex<LoroDoc>>,
    /// Warm, pre-compiled Extism Wasm plugins indexed by their exact BLAKE3 hash.
    /// Wrapped in `Arc<Mutex<Plugin>>` so the DashMap shard guard is dropped before `call`.
    warm_cells: DashMap<Blake3Hash, Arc<Mutex<Plugin>>>,
    /// Content-addressed store (backed by iroh-blobs in full P2P deployment)
    blob_store: DashMap<Blake3Hash, Vec<u8>>,
    /// Atomic occupancy counters enforcing race-free hard ceilings across all shards.
    blob_count: AtomicUsize,
    warm_count: AtomicUsize,
    max_blobs: usize,
    max_warm_cells: usize,
}

impl Default for ExtirohCell {
    fn default() -> Self {
        Self::new()
    }
}

impl ExtirohCell {
    pub fn new() -> Self {
        Self::with_limits(DEFAULT_MAX_BLOBS, DEFAULT_MAX_WARM_CELLS)
    }

    pub fn with_limits(max_blobs: usize, max_warm_cells: usize) -> Self {
        Self {
            causal_state: Arc::new(Mutex::new(LoroDoc::new())),
            warm_cells: DashMap::new(),
            blob_store: DashMap::new(),
            blob_count: AtomicUsize::new(0),
            warm_count: AtomicUsize::new(0),
            max_blobs,
            max_warm_cells,
        }
    }

    pub fn blob_count(&self) -> usize {
        self.blob_store.len()
    }

    pub fn warm_cell_count(&self) -> usize {
        self.warm_cells.len()
    }

    /// Insert a content-addressed Wasm blob while strictly enforcing `max_blobs`.
    pub fn put_blob(&self, wasm: Vec<u8>) -> Result<Blake3Hash> {
        let hash = Blake3Hash::new(&wasm);
        match self.blob_store.entry(hash) {
            Entry::Occupied(_) => Ok(hash),
            Entry::Vacant(vacant) => {
                let res = CapacityReservation::try_new(&self.blob_count, self.max_blobs)
                    .ok_or_else(|| anyhow!("Blob store at capacity ({})", self.max_blobs))?;
                vacant.insert(wasm);
                res.commit();
                Ok(hash)
            }
        }
    }

    /// 1. LIVE FLOW: Route traffic through whatever hash the causal graph currently points to
    pub fn dispatch(&self, capability: &str, payload: &[u8]) -> Result<Vec<u8>> {
        let active_hash = {
            let doc = self
                .causal_state
                .lock()
                .map_err(|_| anyhow!("Causal state mutex poisoned"))?;
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

    /// Deterministic execution of an immutable Wasm hash without holding any DashMap
    /// shard guard across plugin compilation or guest execution.
    fn run_hash(&self, hash: Blake3Hash, entry: &str, input: &[u8]) -> Result<Vec<u8>> {
        // Fast path: clone the Arc and drop the DashMap shard guard immediately.
        let existing = self.warm_cells.get(&hash).map(|r| Arc::clone(r.value()));

        let cell = match existing {
            Some(cell) => cell,
            None => {
                // Copy blob bytes and drop the blob_store shard guard before compiling.
                let wasm_bytes = self
                    .blob_store
                    .get(&hash)
                    .map(|r| r.value().clone())
                    .ok_or_else(|| anyhow!("Missing Wasm blob {hash} - fetch via iroh-blobs"))?;

                // Fast-path check before compiling; if full, re-check whether another thread
                // concurrently warmed this exact hash before failing closed.
                if self.warm_count.load(Ordering::Acquire) >= self.max_warm_cells {
                    if let Some(cell) = self.warm_cells.get(&hash).map(|r| Arc::clone(r.value())) {
                        cell
                    } else {
                        return Err(anyhow!(
                            "Warm cell cache at capacity ({})",
                            self.max_warm_cells
                        ));
                    }
                } else {
                    // Pure sandbox: WASI disabled, no filesystem, no network, deterministic memory.
                    // Compiled outside any DashMap shard lock.
                    let manifest = Manifest::new([Wasm::data(wasm_bytes)]);
                    let compiled = Plugin::new(&manifest, [], false)?;

                    // Atomic single-entry check + CAS hard-cap reservation under shard write lock;
                    // guard drops immediately after Arc::clone.
                    match self.warm_cells.entry(hash) {
                        Entry::Occupied(occupied) => Arc::clone(occupied.get()),
                        Entry::Vacant(vacant) => {
                            let res = CapacityReservation::try_new(
                                &self.warm_count,
                                self.max_warm_cells,
                            )
                            .ok_or_else(|| {
                                anyhow!(
                                    "Warm cell cache at capacity ({})",
                                    self.max_warm_cells
                                )
                            })?;
                            let cell = Arc::new(Mutex::new(compiled));
                            vacant.insert(Arc::clone(&cell));
                            res.commit();
                            cell
                        }
                    }
                }
            }
        };

        let mut plugin = match cell.lock() {
            Ok(guard) => guard,
            Err(_) => {
                // Evict only this exact poisoned Arc so a healthy concurrent replacement
                // is never removed, then release its warm-cache slot for clean re-warming.
                if self
                    .warm_cells
                    .remove_if(&hash, |_, v| Arc::ptr_eq(v, &cell))
                    .is_some()
                {
                    self.warm_count.fetch_sub(1, Ordering::AcqRel);
                }
                return Err(anyhow!(
                    "Plugin mutex poisoned for {hash}; evicted warm cell"
                ));
            }
        };
        let out = plugin.call::<&[u8], Vec<u8>>(entry, input)?;
        Ok(out)
    }

    /// 2. THE SPLICE: Speculatively compile & verify new Wasm on an isolated fresh `Plugin`
    /// BEFORE modifying shared maps, enforcing race-free hard caps and exact rollback.
    pub fn splice_capability(
        &self,
        capability: &str,
        new_wasm: Vec<u8>,
        faulting_payload: &[u8],
        invariant_check: impl Fn(&[u8]) -> bool,
    ) -> Result<(Blake3Hash, Vec<u8>)> {
        let new_hash = Blake3Hash::new(&new_wasm);

        // Always trial-run on an isolated fresh Plugin outside all locks so rejected
        // candidates leave zero cache residue and never mutate a shared live Plugin.
        let manifest = Manifest::new([Wasm::data(new_wasm.clone())]);
        let mut trial_plugin = Plugin::new(&manifest, [], false)?;
        let trial_output = trial_plugin.call::<&[u8], Vec<u8>>("handle", faulting_payload)?;

        if !invariant_check(&trial_output) {
            return Err(anyhow!(
                "Splice rejected: failed downstream invariant proof"
            ));
        }

        // Serialize CRDT fork/delta export and route commit under causal_state lock.
        let mut live_doc = self
            .causal_state
            .lock()
            .map_err(|_| anyhow!("Causal state mutex poisoned"))?;

        let prev_frontiers = live_doc.oplog_frontiers();
        let speculative_branch = live_doc.fork();
        let routes = speculative_branch.get_map("routes");
        routes.insert(capability, new_hash.to_string())?;
        speculative_branch.commit();
        let delta_bytes = speculative_branch.export(ExportMode::updates(&prev_frontiers))?;

        // Fixed lock hierarchy: causal_state -> blob_store shard -> warm_cells shard.
        // Holding both shard Entry guards across capacity reservation and live_doc.import
        // guarantees zero premature publication: no concurrent put_blob or run_hash can
        // observe new_hash before CRDT import succeeds, and any error or panic drops both
        // Entry guards without inserting while CapacityReservation::drop releases slots.
        let blob_entry = self.blob_store.entry(new_hash);
        let blob_res = match &blob_entry {
            Entry::Occupied(_) => None,
            Entry::Vacant(_) => Some(
                CapacityReservation::try_new(&self.blob_count, self.max_blobs)
                    .ok_or_else(|| anyhow!("Blob store at capacity ({})", self.max_blobs))?,
            ),
        };

        let warm_entry = self.warm_cells.entry(new_hash);
        let warm_res = match &warm_entry {
            Entry::Occupied(_) => None,
            Entry::Vacant(_) => Some(
                CapacityReservation::try_new(&self.warm_count, self.max_warm_cells)
                    .ok_or_else(|| anyhow!("Warm cell cache at capacity ({})", self.max_warm_cells))?,
            ),
        };

        if let Err(err) = live_doc.import(&delta_bytes) {
            return Err(err.into());
        }

        // Infallible publication: CRDT import succeeded, causal_state is still held, and
        // reservations are committed only after each vacant entry is populated.
        if let (Entry::Vacant(vacant), Some(res)) = (blob_entry, blob_res) {
            vacant.insert(new_wasm);
            res.commit();
        }
        if let (Entry::Vacant(vacant), Some(res)) = (warm_entry, warm_res) {
            vacant.insert(Arc::new(Mutex::new(trial_plugin)));
            res.commit();
        }

        Ok((new_hash, delta_bytes))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::triad_shim::extism::register_blocking_gate;
    use std::sync::{mpsc, Barrier};
    use std::thread;
    use std::time::Duration;

    fn find_same_shard_wasm(
        cell: &ExtirohCell,
        target_shard: usize,
        prefix: &[u8],
    ) -> (Vec<u8>, Blake3Hash) {
        for nonce in 0u32..10_000 {
            let mut bytes = prefix.to_vec();
            bytes.extend_from_slice(&nonce.to_le_bytes());
            let h = Blake3Hash::new(&bytes);
            if cell.warm_cells.shard_index(&h) == target_shard {
                return (bytes, h);
            }
        }
        panic!("could not find hash colliding onto shard {target_shard}");
    }

    #[test]
    fn test_s5_no_shard_lock_held_across_plugin_call() {
        let cell = Arc::new(ExtirohCell::new());
        let blocking_wasm = b"BLOCK:shard_contention_gate".to_vec();
        let h1 = cell.put_blob(blocking_wasm).unwrap();
        let target_shard = cell.warm_cells.shard_index(&h1);
        let (other_wasm, h2) = find_same_shard_wasm(&cell, target_shard, b"OK_PEER_");
        assert_ne!(h1, h2);
        assert_eq!(cell.warm_cells.shard_index(&h2), target_shard);

        // Pre-warm h1 without blocking first, then arm the gate
        let _ = cell.run_hash(h1, "handle", b"warmup").unwrap();

        let (entered_rx, release_tx) = register_blocking_gate("shard_contention_gate");

        let c1 = Arc::clone(&cell);
        let t1 = thread::spawn(move || c1.run_hash(h1, "handle", b"req1"));

        // Wait until Thread 1 is actively executing inside plugin.call
        entered_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("thread 1 should enter plugin.call");

        // Thread 2 inserts & executes h2 on the EXACT SAME DashMap shard while Thread 1 is blocked.
        // If run_hash still held the DashMap shard guard across plugin.call, this would deadlock.
        let c2 = Arc::clone(&cell);
        let (t2_tx, t2_rx) = mpsc::channel();
        let t2 = thread::spawn(move || {
            let _ = c2.put_blob(other_wasm).unwrap();
            let res = c2.run_hash(h2, "handle", b"req2");
            let _ = t2_tx.send(res);
        });

        let t2_res = t2_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("same-shard insert/call must not block while another plugin.call is running");
        assert!(t2_res.is_ok());

        release_tx.send(()).unwrap();
        assert!(t1.join().unwrap().is_ok());
        t2.join().unwrap();
    }

    #[test]
    fn test_s5_rejected_splices_do_not_leak_blob_or_warm_cells() {
        let cell = ExtirohCell::new();
        for i in 0u32..100 {
            let candidate = format!("CANDIDATE_{i}").into_bytes();
            let res = cell.splice_capability("parse_order", candidate, b"payload", |_| false);
            assert!(res.is_err());
        }
        // Invalid Wasm & guest runtime trap must also leave zero residue
        assert!(cell
            .splice_capability("parse_order", b"INVALID_MOD".to_vec(), b"p", |_| true)
            .is_err());
        assert!(cell
            .splice_capability("parse_order", b"FAIL_TRAP".to_vec(), b"p", |_| true)
            .is_err());
        assert_eq!(cell.blob_count(), 0);
        assert_eq!(cell.warm_cell_count(), 0);
        assert_eq!(cell.blob_count.load(Ordering::Acquire), 0);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), 0);
    }

    #[test]
    fn test_s5_rejected_duplicate_splice_preserves_live_capability_and_state() {
        let cell = ExtirohCell::new();
        let live_wasm = b"STATEFUL_ORGAN_V1".to_vec();
        // Trial run executes on fresh plugin (call_count becomes 1) then becomes live warm plugin.
        let (h_live, _) = cell
            .splice_capability("cap_a", live_wasm.clone(), b"ok", |out| out == b"1ok")
            .unwrap();
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.warm_cell_count(), 1);

        // Re-submitting identical bytes with a failing invariant must run on an isolated
        // fresh Plugin so the shared live Plugin's call_count is NOT incremented!
        let rej = cell.splice_capability("cap_b", live_wasm, b"trial", |_| false);
        assert!(rej.is_err());
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.warm_cell_count(), 1);

        // Next call on live cap_a must observe call_count == 2 (not 3!).
        assert_eq!(cell.dispatch("cap_a", b"ping").unwrap(), b"2ping");
        assert_eq!(cell.run_hash(h_live, "handle", b"x").unwrap(), b"3x");
    }

    #[test]
    fn test_s5_enforces_capacity_caps_and_import_rollback() {
        let cell = ExtirohCell::with_limits(2, 2);
        cell.splice_capability("c1", b"A_WASM".to_vec(), b"1", |_| true)
            .unwrap();
        cell.splice_capability("c2", b"B_WASM".to_vec(), b"2", |_| true)
            .unwrap();
        assert_eq!(cell.blob_count(), 2);
        assert_eq!(cell.warm_cell_count(), 2);

        // Third distinct capability hits the hard cap and leaves both maps at 2.
        let over = cell.splice_capability("c3", b"C_WASM".to_vec(), b"3", |_| true);
        assert!(over.is_err());
        assert_eq!(cell.blob_count(), 2);
        assert_eq!(cell.warm_cell_count(), 2);

        // Simulated CRDT import failure rolls back newly inserted blob & warm cell cleanly.
        let cell_rb = ExtirohCell::with_limits(4, 4);
        cell_rb
            .causal_state
            .lock()
            .unwrap()
            .set_fail_next_import(true);
        let err = cell_rb.splice_capability("c_rb", b"D_WASM".to_vec(), b"4", |_| true);
        assert!(err.is_err());
        assert_eq!(cell_rb.blob_count(), 0);
        assert_eq!(cell_rb.warm_cell_count(), 0);
        assert_eq!(cell_rb.blob_count.load(Ordering::Acquire), 0);
        assert_eq!(cell_rb.warm_count.load(Ordering::Acquire), 0);
    }

    #[test]
    fn test_s5_warm_cap_failure_rolls_back_newly_inserted_blob() {
        let cell = ExtirohCell::with_limits(4, 1);
        cell.splice_capability("c1", b"FIRST_WASM".to_vec(), b"1", |_| true)
            .unwrap();
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.warm_cell_count(), 1);

        // Second splice has room in blob_store (cap 4) but hits warm_cells cap (1):
        // newly inserted blob must be rolled back atomically.
        let err = cell.splice_capability("c2", b"SECOND_WASM".to_vec(), b"2", |_| true);
        assert!(err.is_err());
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.warm_cell_count(), 1);
        assert_eq!(cell.blob_count.load(Ordering::Acquire), 1);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), 1);
    }

    #[test]
    fn test_s5_import_rollback_preserves_preexisting_blob_and_warm_entry() {
        let cell = ExtirohCell::with_limits(4, 4);
        let shared_wasm = b"PREEXISTING_ORGAN".to_vec();
        let h = cell.put_blob(shared_wasm.clone()).unwrap();
        assert_eq!(cell.run_hash(h, "handle", b"hi").unwrap(), b"Phi");
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.warm_cell_count(), 1);

        // Force CRDT import failure on a splice targeting the same already-resident hash.
        // Entry::Occupied ensures rollback does NOT delete the pre-existing blob or warm cell.
        cell.causal_state.lock().unwrap().set_fail_next_import(true);
        let err = cell.splice_capability("cap_fail", shared_wasm, b"ok", |_| true);
        assert!(err.is_err());
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.warm_cell_count(), 1);
        assert_eq!(cell.blob_count.load(Ordering::Acquire), 1);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), 1);
        assert_eq!(cell.run_hash(h, "handle", b"still").unwrap(), b"Pstill");
    }

    #[test]
    fn test_s5_concurrent_run_hash_and_splice_never_exceed_hard_caps() {
        const MAX_BLOBS: usize = 12;
        const MAX_WARM: usize = 3;
        let cell = Arc::new(ExtirohCell::with_limits(MAX_BLOBS, MAX_WARM));

        // Pre-load 8 distinct blobs for concurrent run_hash threads.
        let mut pre_hashes = Vec::new();
        for i in 0u32..8 {
            let wasm = format!("RUNHASH_CONC_{i}").into_bytes();
            pre_hashes.push(cell.put_blob(wasm).unwrap());
        }

        let barrier = Arc::new(Barrier::new(12));
        let mut handles = Vec::new();

        // 8 threads racing on run_hash across distinct un-warmed blobs
        for h in pre_hashes {
            let c = Arc::clone(&cell);
            let b = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                b.wait();
                let _ = c.run_hash(h, "handle", b"q");
                assert!(c.warm_cell_count() <= MAX_WARM);
                assert!(c.blob_count() <= MAX_BLOBS);
            }));
        }

        // 4 threads racing on splice_capability with distinct blobs concurrently
        for i in 0u32..4 {
            let c = Arc::clone(&cell);
            let b = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                b.wait();
                let wasm = format!("SPLICE_CONC_{i}").into_bytes();
                let _ = c.splice_capability(&format!("cap_{i}"), wasm, b"q", |_| true);
                assert!(c.warm_cell_count() <= MAX_WARM);
                assert!(c.blob_count() <= MAX_BLOBS);
            }));
        }

        for h in handles {
            h.join().unwrap();
        }

        assert!(cell.warm_cell_count() <= MAX_WARM);
        assert!(cell.blob_count() <= MAX_BLOBS);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), cell.warm_cell_count());
        assert_eq!(cell.blob_count.load(Ordering::Acquire), cell.blob_count());
    }

    #[test]
    fn test_s5_poisoned_plugin_evicted_and_recovers_and_causal_poison_returns_err() {
        let cell = Arc::new(ExtirohCell::new());
        let panic_wasm = b"PANIC_ON_BOOM_WASM".to_vec();
        let h_panic = cell.put_blob(panic_wasm).unwrap();
        {
            let doc = cell.causal_state.lock().unwrap();
            doc.get_map("routes")
                .insert("poison_cap", h_panic.to_string())
                .unwrap();
        }

        // Spawn a worker that panics on b"boom" while holding the plugin mutex to poison it.
        let c_worker = Arc::clone(&cell);
        let join = thread::spawn(move || {
            let _ = c_worker.run_hash(h_panic, "handle", b"boom");
        });
        assert!(join.join().is_err());
        assert_eq!(cell.warm_cell_count(), 1);

        // Next run_hash observes the poisoned mutex, returns Err (never panics), AND evicts it.
        assert!(cell.run_hash(h_panic, "handle", b"after").is_err());
        assert_eq!(cell.warm_cell_count(), 0);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), 0);

        // Because the poisoned Arc was evicted, a clean subsequent call transparently re-warms
        // from blob_store and succeeds!
        assert_eq!(cell.dispatch("poison_cap", b"recover").unwrap(), b"Precover");
        assert_eq!(cell.warm_cell_count(), 1);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), 1);

        // Also poison causal_state mutex and confirm dispatch + splice_capability return Err.
        let c_poison_doc = Arc::clone(&cell);
        let j2 = thread::spawn(move || {
            let _guard = c_poison_doc.causal_state.lock().unwrap();
            panic!("poison causal_state");
        });
        assert!(j2.join().is_err());
        assert!(cell.dispatch("poison_cap", b"after2").is_err());
        assert!(cell
            .splice_capability("other", b"Z_WASM".to_vec(), b"x", |_| true)
            .is_err());
    }

    #[test]
    fn test_s5_failed_import_never_publishes_or_deletes_concurrent_put_blob() {
        let cell = Arc::new(ExtirohCell::with_limits(4, 4));
        let shared_wasm = b"CONCURRENT_SHARED_ORGAN".to_vec();
        let expected_hash = Blake3Hash::new(&shared_wasm);

        let (entered_rx, release_tx) = {
            let mut doc = cell.causal_state.lock().unwrap();
            doc.set_fail_next_import(true);
            doc.set_import_gate()
        };

        let c1 = Arc::clone(&cell);
        let w1 = shared_wasm.clone();
        let t1 = thread::spawn(move || c1.splice_capability("cap_fail", w1, b"ok", |_| true));

        // Wait until Thread 1 holds both shard Entry guards inside live_doc.import.
        entered_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("splice_capability should enter live_doc.import");

        // Thread 2 races put_blob for the exact same content hash during the import window.
        let c2 = Arc::clone(&cell);
        let w2 = shared_wasm.clone();
        let t2 = thread::spawn(move || c2.put_blob(w2));

        // Let Thread 2 reach the shard lock, then release Thread 1's failing CRDT import.
        thread::sleep(Duration::from_millis(20));
        release_tx.send(()).unwrap();

        assert!(t1.join().unwrap().is_err());
        assert_eq!(t2.join().unwrap().unwrap(), expected_hash);

        // Thread 2's blob must still be present & runnable; failed splice never published or deleted it!
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.blob_count.load(Ordering::Acquire), 1);
        assert_eq!(cell.warm_cell_count(), 0);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), 0);
        assert_eq!(
            cell.run_hash(expected_hash, "handle", b"alive").unwrap(),
            b"Calive"
        );
        assert_eq!(cell.warm_cell_count(), 1);
        assert_eq!(cell.warm_count.load(Ordering::Acquire), 1);
    }

    #[test]
    fn test_s5_valid_splice_and_dispatch_roundtrip() {
        let cell = ExtirohCell::new();
        let wasm = b"V1_ORGAN".to_vec();
        let (h, delta) = cell
            .splice_capability("parse_order", wasm, b"42", |out| out == b"V42")
            .unwrap();
        assert!(!delta.is_empty());
        assert_eq!(cell.blob_count(), 1);
        assert_eq!(cell.warm_cell_count(), 1);
        assert_eq!(cell.dispatch("parse_order", b"99").unwrap(), b"V99");
        assert_eq!(cell.run_hash(h, "handle", b"7").unwrap(), b"V7");
    }
}
