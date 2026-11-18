To build a tech-agnostic polymerized harness, design it as an intermediate protocol broker that stands between any agent client, any target codebase language, and any execution environment. Because MCP, LSP, DAP, and Tree-sitter are already universal open standards, your custom glue simply routes universal JSON-RPC intents to dynamically loaded drivers.

Universal Abstraction Layers


Agnostic Dimension	Standard Protocol / Format	How Your Broker Abstracts It
Host Agent / LLM	MCP (JSON-RPC over stdio/SSE)	Exposes a single unified server (splicer-gateway). Works identically with Claude Code, Cursor, custom SDKs, or local models.
Target Language	LSP + Tree-sitter + CPG	Auto-detects file extensions and workspace manifests (Cargo.toml, package.json, go.mod, pyproject.toml) to spin up matching language grammars and servers on the fly.
Runtime & Debugger	DAP + OCI Containers	Talks to headless Debug Adapter Protocol endpoints over TCP sockets, whether running on bare metal, WSL, Docker, or remote microVMs.
Graph & State Cache	SCIP / SQLite	Indexes symbols into a flat, language-neutral relational schema (like Sourcegraph's SCIP standard) so graph queries look identical across C++, Rust, Python, or TypeScript.


The 4-Piece Agnostic Architecture

• Capability-Driven Registry. Instead of hardcoding language tools, use a declarative manifest (capabilities.toml or JSON) that maps file types to binary adapters. When the agent inspects .rs or .py files, the broker lazily boots rust-analyzer or pyright alongside the matching Tree-sitter grammar in the background.
• Unified Intent Primitives. Strip tool-specific quirks away from the LLM. Expose just 5 universal verbs to the host client. Those verbs are locate_symbol, trace_impact, inspect_runtime, surgical_patch, and verify_contract. The broker translates trace_impact into an LSP reference check or CPG query under the hood without the model needing to know which analyzer ran.
• Stateful Session State Machine. Track every agent task with an internal session token inside your gateway. Keep surgical_patch locked at the protocol level (returning a clean state error or hiding the tool via MCP notifications/tools/list_changed) until the session log shows a completed trace_impact call on the target node.
• Normalized Compact IR (Intermediate Representation). Every compiler, AST parser, and debugger outputs wildly different JSON schemas. Your broker normalizes all responses into one minimal, token-compressed schema—such as [SymbolID | File:Line | Signature | UpstreamCallers | Downstream Consumers]—so the LLM reasons over the exact same structure regardless of the underlying stack.

Minimal Agnostic Data Flow

1. Mount. Point splicer-gateway at any Git repository root and attach any MCP-compatible agent.
2. Fingerprint. The gateway scans root manifests, initializes an ephemeral Git worktree, and warms up the matching LSP/Tree-sitter/DAP adapters inside a sidecar or local process pool.
3. Broker. The agent issues universal structural queries. The gateway routes them to the active language adapters, normalizes the graph output, enforces the read-before-write state lock, and validates the final AST patch against the compiler before allowing worktree promotion.
Would you like me to draft the universal MCP tool schemas (the 5 core primitives and normalized IR format) and the state-machine routing specification so you have a language-neutral blueprint ready to implement?
