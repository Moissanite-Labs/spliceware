# Era I — Polymerized Splicer Broker

Sources: [`conversation.md`](../mithrax-splicing-session/conversation.md) Turns 1-4, [`inline-element-01`](../mithrax-splicing-session/inline-element-01-mithrax-splicer-youtube-card.html) through [`05`](../mithrax-splicing-session/inline-element-05-visual-explanation-splicer-hub.jpg).

## Core thesis

Brute-force coding agents emit raw text patches over opaque files. The Eliksni "Splicer" agent interfaces directly with live structural, semantic, and runtime graphs instead, so edits land as surgical splices on a known structure rather than line-based guesses.

## The 4-plane tech-agnostic stack

1. **Capability and context plane.** Model Context Protocol (MCP) brokers tools and resources. The agent asks for capabilities; the broker decides what exists and what is allowed.
2. **Syntactic and structural plane.** Language Server Protocol (LSP) plus Tree-sitter plus Code Property Graphs (CPG) give the agent an AST-valid view for surgical splicing, instead of regex or unified diffs.
3. **Execution and sandbox plane.** Debug Adapter Protocol (DAP) plus OCI containers or microVMs give deterministic state inspection before and after a change.
4. **Semantic index and provenance plane.** SCIP code intelligence plus an embedded SQLite graph store answer "what depends on this" and "where did this come from".

## What this era established

- The broker is tech-agnostic: the same four planes map onto any language or toolchain that exposes LSP/DAP-style adapters.
- Patching is a degenerate case of splicing, kept only for text files with no structural view available.
- Every plane has a failure mode the agent can see and report, rather than silently emitting a bad patch.

Related: [Era II](02-causal-cryptographic-determinism.md) adds determinism and auditability to the broker model.
