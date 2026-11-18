In-memory guest Wasm synthesis paired with deterministic instruction fuel and page-level memory caps lets an agent forge live bytecode adapters in microseconds while making infinite loops, memory bombs, and non-deterministic drift physically impossible.

Synthesis Latency vs. Expressive Power

To stay completely free of disk files and clunky build pipelines, the Extiroh host gives the agent two ways to forge .wasm bytes directly in RAM:

Synthesis Tier	Crate / Engine	Compile Time	Best Used When...
Fast-Path Micro-Splice	wat / wasm-encoder	< 0.5 ms	Wiring pure byte transforms, header adapters, routing filters, or binary math directly in memory.
Complex Business Logic	Pre-warmed extism-rust-pdk / AssemblyScript	50–300 ms	Synthesizing rich JSON schema migrations or stateful parsers against cached standard libraries.


The 4 Laws of Physics (Deterministic Guardrails)

Before any agent-forged Wasm hash touches your speculative CRDT fork, extism::PluginBuilder and extism::Manifest lock the cell inside four non-negotiable hardware boundaries:
• Instruction Fuel Metering. Configuring PluginBuilder::with_fuel_limit(fuel_cap) forces the underlying Wasmtime engine to deduct deterministic "gas" on every single WebAssembly instruction. If an agent synthesizes an accidental loop {}, execution traps at the exact instruction threshold rather than hanging a thread—and unlike wall-clock timeouts, instruction counts are 100% deterministic across slow laptops and fast servers.
• Linear Memory Page Ceilings. Setting Manifest::with_memory_max(pages) caps allocation in 64 KiB increments (e.g., 16 pages = hard 1 MiB ceiling). Any runaway vector allocation inside the guest immediately traps cleanly without touching host RAM.
• Ambient Zero-Trust. Passing .with_wasi(false) and .disallow_all_hosts() strips system clocks, random number generators, sockets, and filesystems out of the guest environment. Without clocks or entropy, floating-point NaN canonicalization and pure byte-in/byte-out ABI calls guarantee cryptographic determinism.
• Metabolic Fitness Selection. Calling Plugin::fuel_consumed() right after the trial run measures the exact computational cost of the splice. If two agents synthesize competing fixes on concurrent CRDT branches, nodes automatically prefer the hash that burns less fuel.

Implementation: Zero-Disk Synthesis + Guarded Execution

This module plugs straight into ExtirohCell. It compiles raw WebAssembly Text (WAT) S-expressions emitted by an agent directly in RAM via wat::parse_str, enforces strict fuel and memory physics, and returns a metabolic telemetry receipt.
