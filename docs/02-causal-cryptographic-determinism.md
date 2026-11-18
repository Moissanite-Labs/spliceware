# Era II — Causal and Cryptographic Determinism

Sources: [`conversation.md`](../mithrax-splicing-session/conversation.md) Turns 5-9, [`inline-element-06`](../mithrax-splicing-session/inline-element-06-fluid-living-system-table.html) through [`09`](../mithrax-splicing-session/inline-element-09-merkle-crdt-wasm-fabric-table.html).

## Core thesis

Replace static build-deploy pipelines with a fluid, living substrate, while keeping every state transition auditable. The separation that makes this possible is human predictability versus causal determinism:

- **Human predictability** asks whether a person can know in advance which code branch an evolutionary system will invent. The answer is no, and the design stops pretending otherwise.
- **Causal determinism** says given identical parent lineage hashes and identical inputs, every node replays the exact bit-for-bit trajectory. This is the property the audit trail rests on.

## The determinism stack

- **Content addressing and history.** Merkle DAGs for immutable lineage, Merkle-CRDTs for causal partial order without a central consensus coordinator.
- **Deterministic signatures and encryption.** RFC 6979 deterministic ECDSA and Ed25519 (the nonce derives from key plus message, so there is no RNG side channel), plus AES-SIV / deterministic AEAD so identical plaintexts deduplicate cleanly in storage and on the wire.
- **Verifiable entropy and key hierarchies.** Verifiable Random Functions (VRFs) give provable tournament and mutation randomness; BIP-32 hierarchical deterministic derivation gives capability subkeys without exposing the root key.

## What this era established

- Evolution can be unpredictable to people and fully replayable by machines.
- Every mutation carries its ancestry in its hash, so audit does not depend on logs that can be edited after the fact.
- Randomness for tournaments and crossover comes from seeded, verifiable sources, not from wall-clock entropy.

Related: [Era III](03-extiroh-wasm-substrate.md) builds the Wasm organ runtime, [Era IV](04-cellular-substrate-and-sacred-splicer.md) collapses the design to C11.
