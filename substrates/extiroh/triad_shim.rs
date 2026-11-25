//! Zero-dependency, `#![forbid(unsafe_code)]` stdlib shim for offline compilation and
//! multi-threaded regression testing of `ExtirohCell` (`runtime_loop.rs`) under
//! `RUSTFLAGS="-D warnings" cargo test`.
//!
//! `dashmap::DashMap` uses 16 real `RwLock<HashMap<K, V>>` shards returning real RAII
//! `Ref<'_, K, V>` / `Entry<'_, K, V>` guards that hold the shard lock until dropped, so
//! holding a `Ref` across `plugin.call` genuinely deadlocks same-shard writers in tests.

pub mod anyhow {
    use std::fmt;

    #[derive(Debug, Clone)]
    pub struct Error(pub String);

    impl fmt::Display for Error {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str(&self.0)
        }
    }

    impl std::error::Error for Error {}

    impl From<super::loro::LoroError> for Error {
        fn from(err: super::loro::LoroError) -> Self {
            Self(err.0)
        }
    }

    impl From<super::iroh_blobs::ParseHashError> for Error {
        fn from(err: super::iroh_blobs::ParseHashError) -> Self {
            Self(err.0)
        }
    }

    pub type Result<T> = std::result::Result<T, Error>;

    macro_rules! anyhow {
        ($($arg:tt)*) => {
            crate::triad_shim::anyhow::Error(format!($($arg)*))
        };
    }
    pub(crate) use anyhow;
}

pub mod dashmap {
    use std::collections::hash_map::{DefaultHasher, HashMap};
    use std::hash::{Hash, Hasher};
    use std::ops::Deref;
    use std::sync::{RwLock, RwLockReadGuard, RwLockWriteGuard};

    pub const NUM_SHARDS: usize = 16;

    pub struct DashMap<K, V> {
        shards: [RwLock<HashMap<K, V>>; NUM_SHARDS],
    }

    impl<K: Eq + Hash + Clone, V> DashMap<K, V> {
        pub fn new() -> Self {
            Self {
                shards: std::array::from_fn(|_| RwLock::new(HashMap::new())),
            }
        }

        pub fn shard_index(&self, key: &K) -> usize {
            let mut h = DefaultHasher::new();
            key.hash(&mut h);
            (h.finish() as usize) % NUM_SHARDS
        }

        pub fn get<'a>(&'a self, key: &K) -> Option<Ref<'a, K, V>> {
            let guard = self.shards[self.shard_index(key)]
                .read()
                .unwrap_or_else(|e| e.into_inner());
            if guard.contains_key(key) {
                Some(Ref {
                    guard,
                    key: key.clone(),
                })
            } else {
                None
            }
        }

        pub fn remove_if(&self, key: &K, f: impl FnOnce(&K, &V) -> bool) -> Option<(K, V)> {
            let mut guard = self.shards[self.shard_index(key)]
                .write()
                .unwrap_or_else(|e| e.into_inner());
            if guard.get(key).is_some_and(|v| f(key, v)) {
                guard.remove_entry(key)
            } else {
                None
            }
        }

        pub fn len(&self) -> usize {
            self.shards
                .iter()
                .map(|s| s.read().unwrap_or_else(|e| e.into_inner()).len())
                .sum()
        }

        pub fn entry(&self, key: K) -> Entry<'_, K, V> {
            let idx = self.shard_index(&key);
            let guard = self.shards[idx]
                .write()
                .unwrap_or_else(|e| e.into_inner());
            if guard.contains_key(&key) {
                Entry::Occupied(OccupiedEntry { guard, key })
            } else {
                Entry::Vacant(VacantEntry { guard, key })
            }
        }
    }

    pub struct Ref<'a, K, V> {
        guard: RwLockReadGuard<'a, HashMap<K, V>>,
        key: K,
    }

    impl<K: Eq + Hash, V> Ref<'_, K, V> {
        pub fn value(&self) -> &V {
            self.guard
                .get(&self.key)
                .expect("Ref key verified present under shard read lock")
        }
    }

    impl<K: Eq + Hash, V> Deref for Ref<'_, K, V> {
        type Target = V;

        fn deref(&self) -> &Self::Target {
            self.value()
        }
    }

    pub enum Entry<'a, K, V> {
        Occupied(OccupiedEntry<'a, K, V>),
        Vacant(VacantEntry<'a, K, V>),
    }

    pub struct OccupiedEntry<'a, K, V> {
        guard: RwLockWriteGuard<'a, HashMap<K, V>>,
        key: K,
    }

    impl<K: Eq + Hash, V> OccupiedEntry<'_, K, V> {
        pub fn get(&self) -> &V {
            self.guard
                .get(&self.key)
                .expect("OccupiedEntry key verified present under shard write lock")
        }
    }

    pub struct VacantEntry<'a, K, V> {
        guard: RwLockWriteGuard<'a, HashMap<K, V>>,
        key: K,
    }

    impl<'a, K: Eq + Hash + Clone, V> VacantEntry<'a, K, V> {
        pub fn insert(mut self, value: V) -> RefMut<'a, K, V> {
            self.guard.insert(self.key.clone(), value);
            RefMut {
                _guard: self.guard,
            }
        }
    }

    pub struct RefMut<'a, K, V> {
        _guard: RwLockWriteGuard<'a, HashMap<K, V>>,
    }
}

pub mod iroh_blobs {
    use std::fmt;
    use std::str::FromStr;

    #[derive(Debug, Clone)]
    pub struct ParseHashError(pub String);

    impl fmt::Display for ParseHashError {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str(&self.0)
        }
    }

    impl std::error::Error for ParseHashError {}

    #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
    pub struct Hash([u8; 32]);

    impl Hash {
        /// Deterministic 256-bit ARX content digest for offline testing.
        pub fn new(bytes: &[u8]) -> Self {
            let mut s: [u64; 4] = [
                0x6a09e667f3bcc908,
                0xbb67ae8584caa73b,
                0x3c6ef372fe94f82b,
                0xa54ff53a5f1d36f1,
            ];
            for (i, chunk) in bytes.chunks(8).enumerate() {
                let mut buf = [0u8; 8];
                buf[..chunk.len()].copy_from_slice(chunk);
                s[i & 3] ^= u64::from_le_bytes(buf);
                for _ in 0..4 {
                    s[0] = s[0].wrapping_add(s[1]);
                    s[1] = s[1].rotate_left(13) ^ s[0];
                    s[2] = s[2].wrapping_add(s[3]);
                    s[3] = s[3].rotate_left(17) ^ s[2];
                    s[0] = s[0].wrapping_add(s[3]);
                    s[3] = s[3].rotate_left(21) ^ s[0];
                    s[2] = s[2].wrapping_add(s[1]);
                    s[1] = s[1].rotate_left(29) ^ s[2];
                }
            }
            s[3] ^= bytes.len() as u64;
            for _ in 0..4 {
                s[0] = s[0].wrapping_add(s[1]);
                s[1] = s[1].rotate_left(13) ^ s[0];
                s[2] = s[2].wrapping_add(s[3]);
                s[3] = s[3].rotate_left(17) ^ s[2];
            }
            let mut out = [0u8; 32];
            for (i, w) in s.iter().enumerate() {
                out[i * 8..(i + 1) * 8].copy_from_slice(&w.to_le_bytes());
            }
            Self(out)
        }
    }

    impl fmt::Display for Hash {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            for b in &self.0 {
                write!(f, "{b:02x}")?;
            }
            Ok(())
        }
    }

    impl FromStr for Hash {
        type Err = ParseHashError;

        fn from_str(s: &str) -> std::result::Result<Self, Self::Err> {
            if s.len() != 64 {
                return Err(ParseHashError(format!("invalid hash length: {}", s.len())));
            }
            let mut out = [0u8; 32];
            for i in 0..32 {
                out[i] = u8::from_str_radix(&s[i * 2..i * 2 + 2], 16)
                    .map_err(|_| ParseHashError("invalid hex in hash".to_string()))?;
            }
            Ok(Self(out))
        }
    }
}

pub mod extism {
    use super::anyhow::{anyhow, Result};
    #[cfg(test)]
    use std::collections::HashMap;
    #[cfg(test)]
    use std::sync::{mpsc, Mutex, OnceLock};

    #[cfg(test)]
    type GateEntry = (mpsc::Sender<()>, mpsc::Receiver<()>);

    #[cfg(test)]
    fn call_gates() -> &'static Mutex<HashMap<String, GateEntry>> {
        static GATES: OnceLock<Mutex<HashMap<String, GateEntry>>> = OnceLock::new();
        GATES.get_or_init(|| Mutex::new(HashMap::new()))
    }

    #[cfg(test)]
    pub fn register_blocking_gate(tag: &str) -> (mpsc::Receiver<()>, mpsc::Sender<()>) {
        let (entered_tx, entered_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        call_gates()
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(tag.to_string(), (entered_tx, release_rx));
        (entered_rx, release_tx)
    }

    pub struct Wasm {
        pub(crate) bytes: Vec<u8>,
    }

    impl Wasm {
        pub fn data(bytes: Vec<u8>) -> Self {
            Self { bytes }
        }
    }

    pub struct Manifest {
        pub(crate) wasm: Vec<u8>,
    }

    impl Manifest {
        pub fn new(sources: [Wasm; 1]) -> Self {
            let [w] = sources;
            Self { wasm: w.bytes }
        }
    }

    pub struct Plugin {
        bytes: Vec<u8>,
        call_count: usize,
    }

    impl Plugin {
        pub fn new(manifest: &Manifest, _host_fns: [(); 0], _wasi: bool) -> Result<Self> {
            if manifest.wasm.is_empty() || manifest.wasm.starts_with(b"INVALID") {
                return Err(anyhow!("invalid wasm payload"));
            }
            Ok(Self {
                bytes: manifest.wasm.clone(),
                call_count: 0,
            })
        }

        pub fn call<I, O>(&mut self, entry: &str, input: &[u8]) -> Result<Vec<u8>> {
            if entry != "handle" {
                return Err(anyhow!("unknown entry point: {entry}"));
            }
            if self.bytes.starts_with(b"PANIC_ON_BOOM") {
                if input == b"boom" {
                    panic!("simulated guest panic on boom payload inside plugin mutex");
                }
            } else if self.bytes.starts_with(b"PANIC") {
                panic!("simulated guest panic inside plugin mutex");
            }
            if self.bytes.starts_with(b"FAIL") {
                return Err(anyhow!("simulated guest runtime trap"));
            }
            #[cfg(test)]
            if let Some(rest) = self.bytes.strip_prefix(b"BLOCK:") {
                let tag = String::from_utf8_lossy(rest).into_owned();
                let gate = call_gates()
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .remove(&tag);
                if let Some((entered_tx, release_rx)) = gate {
                    let _ = entered_tx.send(());
                    let _ = release_rx.recv();
                }
            }
            self.call_count = self.call_count.saturating_add(1);
            if self.bytes.starts_with(b"STATEFUL") {
                let mut out = Vec::with_capacity(input.len() + 1);
                out.push(b'0'.saturating_add((self.call_count.min(9)) as u8));
                out.extend_from_slice(input);
                return Ok(out);
            }
            let mut out = Vec::with_capacity(input.len() + 1);
            out.push(self.bytes[0]);
            out.extend_from_slice(input);
            Ok(out)
        }
    }
}

pub mod loro {
    use std::collections::BTreeMap;
    use std::fmt;
    #[cfg(test)]
    use std::sync::mpsc;
    use std::sync::{Arc, Mutex};

    #[derive(Debug, Clone)]
    pub struct LoroError(pub String);

    impl fmt::Display for LoroError {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str(&self.0)
        }
    }

    impl std::error::Error for LoroError {}

    pub type LoroResult<T> = std::result::Result<T, LoroError>;

    #[derive(Clone, Default)]
    pub struct Frontiers;

    pub struct ExportMode;

    impl ExportMode {
        pub fn updates(_frontiers: &Frontiers) -> Self {
            Self
        }
    }

    pub struct LoroValue(String);

    impl LoroValue {
        pub fn into_value(self) -> LoroResult<Self> {
            Ok(self)
        }

        pub fn into_string(self) -> LoroResult<String> {
            Ok(self.0)
        }
    }

    pub struct LoroMap {
        inner: Arc<Mutex<BTreeMap<String, String>>>,
    }

    impl LoroMap {
        pub fn get(&self, key: &str) -> Option<LoroValue> {
            let guard = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            guard.get(key).cloned().map(LoroValue)
        }

        pub fn insert(&self, key: &str, value: String) -> LoroResult<()> {
            let mut guard = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            guard.insert(key.to_string(), value);
            Ok(())
        }
    }

    pub struct LoroDoc {
        routes: Arc<Mutex<BTreeMap<String, String>>>,
        fail_next_import: bool,
        #[cfg(test)]
        import_gate: Option<(mpsc::Sender<()>, mpsc::Receiver<()>)>,
    }

    impl LoroDoc {
        pub fn new() -> Self {
            Self {
                routes: Arc::new(Mutex::new(BTreeMap::new())),
                fail_next_import: false,
                #[cfg(test)]
                import_gate: None,
            }
        }

        pub fn fork(&self) -> Self {
            let snap = self.routes.lock().unwrap_or_else(|e| e.into_inner()).clone();
            Self {
                routes: Arc::new(Mutex::new(snap)),
                fail_next_import: false,
                #[cfg(test)]
                import_gate: None,
            }
        }

        pub fn get_map(&self, _name: &str) -> LoroMap {
            LoroMap {
                inner: Arc::clone(&self.routes),
            }
        }

        pub fn commit(&self) {}

        pub fn oplog_frontiers(&self) -> Frontiers {
            Frontiers
        }

        pub fn export(&self, _mode: ExportMode) -> LoroResult<Vec<u8>> {
            let guard = self.routes.lock().unwrap_or_else(|e| e.into_inner());
            let mut buf = Vec::new();
            for (k, v) in guard.iter() {
                buf.extend_from_slice(k.as_bytes());
                buf.push(b'=');
                buf.extend_from_slice(v.as_bytes());
                buf.push(b'\n');
            }
            Ok(buf)
        }

        pub fn import(&mut self, bytes: &[u8]) -> LoroResult<()> {
            #[cfg(test)]
            if let Some((entered_tx, release_rx)) = self.import_gate.take() {
                let _ = entered_tx.send(());
                let _ = release_rx.recv();
            }
            if self.fail_next_import {
                self.fail_next_import = false;
                return Err(LoroError("simulated CRDT import failure".to_string()));
            }
            let text = std::str::from_utf8(bytes)
                .map_err(|_| LoroError("invalid UTF-8 in delta".to_string()))?;
            let mut guard = self.routes.lock().unwrap_or_else(|e| e.into_inner());
            for line in text.lines() {
                if let Some((k, v)) = line.split_once('=') {
                    guard.insert(k.to_string(), v.to_string());
                }
            }
            Ok(())
        }

        #[cfg(test)]
        pub fn set_fail_next_import(&mut self, fail: bool) {
            self.fail_next_import = fail;
        }

        #[cfg(test)]
        pub fn set_import_gate(&mut self) -> (mpsc::Receiver<()>, mpsc::Sender<()>) {
            let (entered_tx, entered_rx) = mpsc::channel();
            let (release_tx, release_rx) = mpsc::channel();
            self.import_gate = Some((entered_tx, release_rx));
            (entered_rx, release_tx)
        }
    }
}
